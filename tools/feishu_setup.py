"""Provision Feishu application settings over physical USB; secrets only via stdin."""
from __future__ import annotations

import argparse
import json
import os
import re
import secrets
import select
import sys
import time

if __package__:
    from .mac_bridge import configure_serial
else:
    from mac_bridge import configure_serial


def validate_settings(settings: dict) -> dict:
    if not isinstance(settings, dict):
        raise ValueError("配置格式无效")
    if "engine" in settings:
        raise ValueError("语音切换已移除；此入口只保存飞书应用凭据")
    if settings.get("clear") is True:
        return {"clear": True}
    app_id = settings.get("app_id", "")
    secret = settings.get("app_secret", "")
    if not isinstance(app_id, str) or not isinstance(secret, str):
        raise ValueError("应用凭据格式无效")
    if app_id or secret:
        if (not re.fullmatch(r"cli_[A-Za-z0-9_-]{1,59}", app_id)
                or not re.fullmatch(r"[A-Za-z0-9_-]{1,127}", secret)):
            raise ValueError("请同时填写有效的 App ID 和 App Secret，或同时留空保留原配置")
    return {"app_id": app_id, "app_secret": secret, "clear": False}


class UsbReplyTimeout(RuntimeError):
    """No matching reply; safe to retry only a read-only capability probe."""


def exchange(fd: int, message: dict, timeout: float = 5.0, protocol: int = 2) -> dict:
    request_id = secrets.token_hex(16)
    # A leading delimiter discards a partial line left by an interrupted USB session.
    prefix = "\n" if message.get("type") == "feishu_status" else ""
    wire = memoryview((prefix + json.dumps({**message, "protocol": protocol, "request_id": request_id}) + "\n").encode())
    if len(wire) > 768:
        raise ValueError("配置超过设备协议长度限制")
    deadline = time.monotonic() + timeout
    while wire:
        if time.monotonic() >= deadline:
            raise RuntimeError("USB 写入超时")
        _, writable, _ = select.select([], [fd], [], 0.1)
        if not writable:
            continue
        try:
            count = os.write(fd, wire)
            if count <= 0:
                raise RuntimeError("USB 已断开")
            wire = wire[count:]
        except BlockingIOError:
            pass
    buffer = bytearray()
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.1)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 1024)
        except BlockingIOError:
            continue
        if not chunk:
            raise RuntimeError("USB 已断开")
        buffer.extend(chunk)
        if len(buffer) > 16384:
            raise RuntimeError("设备返回了异常数据")
        while b"\n" in buffer:
            raw, _, rest = buffer.partition(b"\n")
            buffer[:] = rest
            try:
                reply = json.loads(raw)
            except (ValueError, UnicodeDecodeError):
                continue
            if not isinstance(reply, dict) or reply.get("type") != "feishu_config_status":
                continue
            if reply.get("request_id") != request_id:
                continue
            versions = (1, 2) if message.get("type") == "feishu_status" else (2,)
            if reply.get("protocol") not in versions:
                raise RuntimeError("设备配置协议不兼容，请更新固件")
            if reply.get("ok") is not True:
                if reply.get("error") == "busy":
                    raise RuntimeError("设备正在录音或升级，请结束后重试")
                raise RuntimeError("设备拒绝配置：请检查凭据；首次保存需填写 App ID 和 Secret")
            return reply
    raise UsbReplyTimeout("设备未响应：等待 USB 回执超时")


def probe(fd: int) -> dict:
    for attempt in range(3):
        try:
            reply = exchange(fd, {"type": "feishu_status"}, timeout=3.0, protocol=1)
            if reply.get("protocol") != 2:
                raise RuntimeError("设备仍使用带语音切换的旧版协议，请先更新本版固件。已有飞书凭据会保留，无需清除。")
            return reply
        except UsbReplyTimeout:
            if attempt == 2:
                raise UsbReplyTimeout(
                    "USB 握手未完成（已重试 3 次）。这不表示固件未更新；"
                    "请重新插拔 USB，并关闭其他串口程序后重试。"
                ) from None
    raise AssertionError("unreachable")


def check_device(port: str) -> dict:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_serial(fd)
        return probe(fd)
    finally:
        os.close(fd)


def provision(port: str, settings: dict) -> dict:
    settings = validate_settings(settings)
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_serial(fd)
        # Probe before sending any secret; legacy firmware receives no credentials.
        probe(fd)
        try:
            reply = exchange(fd, {"type": "feishu_configure", **settings})
        except UsbReplyTimeout:
            raise UsbReplyTimeout(
                "设备已确认支持飞书配置，但未收到保存回执。保存结果尚未确认；"
                "请重新连接 USB 后重试，无需重复刷机。"
            ) from None
        expected = not settings.get("clear", False)
        if reply.get("configured") is not expected:
            raise RuntimeError("设备回执与凭据保存操作不一致，请重试")
        return reply
    finally:
        os.close(fd)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--stdin", action="store_true")
    mode.add_argument("--check", action="store_true", help="只检测 USB 和固件能力，不写凭据")
    args = parser.parse_args(argv)
    try:
        if args.check:
            reply = check_device(args.port)
            print("USB 通信正常，设备支持飞书配置。"
                  + ("设备已有应用凭据。" if reply.get("configured") else "设备尚未保存应用凭据。"))
            return 0
        raw = sys.stdin.read(4097)
        if len(raw) > 4096:
            raise ValueError("配置输入过长")
        try:
            settings = json.loads(raw)
        except ValueError:
            raise ValueError("配置格式无效") from None
        reply = provision(args.port, settings)
        if settings.get("clear") is True:
            print("设备上的飞书应用凭据已清除。")
        else:
            print("飞书应用凭据已保存到设备，供后续飞书功能使用。语音识别保持使用 Mac。")
        return 0
    except (ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
    except OSError:
        print("USB 通信失败，请检查连接和串口占用。", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
