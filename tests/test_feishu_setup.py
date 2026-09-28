import contextlib
import io
import json
import os
import pty
import select
import threading
import unittest
from unittest.mock import Mock, patch

from tools.feishu_setup import UsbReplyTimeout, exchange, main, probe, provision, validate_settings
from tools.mac_bridge import SerialBridge, configure_serial, encode_message


class FeishuSetupTests(unittest.TestCase):
    def test_probe_recovers_from_first_missing_reply(self):
        with patch("tools.feishu_setup.exchange", side_effect=[UsbReplyTimeout("lost"), {"ok": True, "protocol": 2}]) as send:
            self.assertEqual(probe(42), {"ok": True, "protocol": 2})
            self.assertEqual(send.call_count, 2)
            self.assertTrue(all(call.args[1] == {"type": "feishu_status"} for call in send.call_args_list))

    def test_probe_exhaustion_does_not_send_credentials(self):
        with patch("tools.feishu_setup.os.open", return_value=42), \
                patch("tools.feishu_setup.os.close"), \
                patch("tools.feishu_setup.configure_serial"), \
                patch("tools.feishu_setup.exchange", side_effect=UsbReplyTimeout("lost")) as send:
            with self.assertRaisesRegex(UsbReplyTimeout, "不表示固件未更新"):
                provision("unused", {"app_id": "cli_test", "app_secret": "test_secret"})
            self.assertEqual(send.call_count, 3)
            self.assertTrue(all(call.args[1] == {"type": "feishu_status"} for call in send.call_args_list))

    def test_save_timeout_is_not_retried_or_reported_as_old_firmware(self):
        with patch("tools.feishu_setup.os.open", return_value=42), \
                patch("tools.feishu_setup.os.close"), \
                patch("tools.feishu_setup.configure_serial"), \
                patch("tools.feishu_setup.exchange", side_effect=[{"ok": True, "protocol": 2}, UsbReplyTimeout("lost")]) as send:
            with self.assertRaisesRegex(UsbReplyTimeout, "保存结果尚未确认"):
                provision("unused", {"app_id": "cli_test", "app_secret": "test_secret"})
            self.assertEqual(send.call_count, 2)

    def test_check_mode_does_not_read_credentials(self):
        with patch("tools.feishu_setup.check_device", return_value={"configured": False}) as check, \
                patch("sys.stdin") as stdin, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(["--port", "unused", "--check"]), 0)
            stdin.read.assert_not_called()
            check.assert_called_once_with("unused")

    def test_voice_configuration_firmware_is_detected_before_sending_keys(self):
        with patch("tools.feishu_setup.os.open", return_value=42), \
                patch("tools.feishu_setup.os.close"), \
                patch("tools.feishu_setup.configure_serial"), \
                patch("tools.feishu_setup.exchange", return_value={"protocol": 1, "ok": True}) as send:
            with self.assertRaisesRegex(RuntimeError, "已有飞书凭据会保留"):
                provision("unused", {"app_id": "cli_test", "app_secret": "test_secret"})
            send.assert_called_once_with(42, {"type": "feishu_status"}, timeout=3.0, protocol=1)

    def test_no_engine_field_is_sent_when_saving_credentials(self):
        with patch("tools.feishu_setup.os.open", return_value=42), \
                patch("tools.feishu_setup.os.close"), \
                patch("tools.feishu_setup.configure_serial"), \
                patch("tools.feishu_setup.exchange", side_effect=[{"protocol": 2}, {"configured": True}]) as send:
            provision("unused", {"app_id": "cli_test", "app_secret": "test_secret"})
            self.assertNotIn("engine", send.call_args.args[1])

    def test_credentials_are_optional_only_as_a_pair(self):
        self.assertEqual(validate_settings({})["app_id"], "")
        for settings in (
            {"app_id": "cli_test"},
            {"app_secret": "test_secret"},
            {"app_id": "cli_test", "app_secret": "x\nheader"},
            {"engine": "unknown"}, [],
        ):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                validate_settings(settings)

    def test_clear_removes_credentials_from_request(self):
        self.assertEqual(validate_settings({"clear": True, "app_secret": "test_secret"}),
                         {"clear": True})

    def test_old_firmware_receives_probe_but_no_secret(self):
        with patch("tools.feishu_setup.os.open", return_value=42), \
                patch("tools.feishu_setup.os.close") as close, \
                patch("tools.feishu_setup.configure_serial"), \
                patch("tools.feishu_setup.exchange", side_effect=RuntimeError("old firmware")) as send:
            with self.assertRaises(RuntimeError):
                provision("unused", {"app_id": "cli_test", "app_secret": "test_secret"})
            send.assert_called_once_with(42, {"type": "feishu_status"}, timeout=3.0, protocol=1)
            close.assert_called_once_with(42)

    def test_save_ack_must_confirm_credentials(self):
        with patch("tools.feishu_setup.os.open", return_value=42), \
                patch("tools.feishu_setup.os.close"), \
                patch("tools.feishu_setup.configure_serial"), \
                patch("tools.feishu_setup.exchange", side_effect=[{"protocol": 2}, {"configured": False}]):
            with self.assertRaisesRegex(RuntimeError, "不一致"):
                provision("unused", {"app_id": "cli_test", "app_secret": "test_secret"})

    def test_usb_handshake_and_save_with_fragmented_ack(self):
        master, slave = pty.openpty()
        requests = []
        errors = []
        def device():
            try:
                buffer = bytearray()
                for _ in range(2):
                    while b"\n" not in buffer:
                        if not select.select([master], [], [], 2)[0]:
                            raise TimeoutError("no request")
                        buffer.extend(os.read(master, 1024))
                    line, _, rest = buffer.partition(b"\n")
                    buffer[:] = rest
                    if not line.strip():
                        while b"\n" not in buffer:
                            buffer.extend(os.read(master, 1024))
                        line, _, rest = buffer.partition(b"\n")
                        buffer[:] = rest
                    request = json.loads(line)
                    requests.append(request)
                    response = {"type": "feishu_config_status", "protocol": 2,
                                "request_id": request["request_id"], "ok": True,
                                "configured": True}
                    os.write(master, b"boot log\n")
                    wire = (json.dumps(response) + "\n").encode()
                    os.write(master, wire[:9])
                    os.write(master, wire[9:])
            except Exception as error:
                errors.append(error)
        worker = threading.Thread(target=device)
        worker.start()
        try:
            reply = provision(os.ttyname(slave), {"app_id": "cli_test", "app_secret": "test_secret"})
            worker.join(3)
            self.assertFalse(worker.is_alive())
            self.assertFalse(errors, errors)
            self.assertTrue(reply["configured"])
            self.assertEqual(requests[0]["type"], "feishu_status")
            self.assertNotIn("app_secret", requests[0])
            self.assertEqual(requests[1]["app_secret"], "test_secret")
        finally:
            os.close(master)
            os.close(slave)
            worker.join(3)

    def test_stale_ack_is_not_accepted(self):
        master, slave = pty.openpty()
        try:
            configure_serial(slave)
            os.write(master, b'{"type":"feishu_config_status","protocol":1,"request_id":"stale","ok":true}\n')
            with self.assertRaisesRegex(RuntimeError, "未响应"):
                exchange(slave, {"type": "feishu_status"}, timeout=0.15)
        finally:
            os.close(master)
            os.close(slave)

    def test_malformed_input_does_not_echo_secrets(self):
        with patch("sys.stdin", io.StringIO('{"app_secret":"test_secret"')), \
                contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(main(["--port", "unused", "--stdin"]), 1)
        self.assertNotIn("test_secret", errors.getvalue())

    def test_busy_device_error_is_sanitized(self):
        master, slave = pty.openpty()
        def reply():
            wire = bytearray()
            while b"\n" not in wire:
                wire.extend(os.read(master, 1024))
            req = json.loads(wire)
            os.write(master, (json.dumps({"type": "feishu_config_status", "protocol": 2,
                "request_id": req["request_id"], "ok": False, "error": "busy",
                "app_secret": "must_not_be_displayed"}) + "\n").encode())
        configure_serial(slave)
        worker = threading.Thread(target=reply)
        worker.start()
        try:
            with self.assertRaisesRegex(RuntimeError, "录音或升级"):
                exchange(slave, {"type": "feishu_status"})
        finally:
            worker.join(2)
            os.close(master)
            os.close(slave)

    def test_cancel_discards_pending_apple_result(self):
        bridge = SerialBridge("unused")
        process = Mock()
        process.poll.return_value = None
        bridge.speech_process = process
        bridge.capture.start({})
        bridge.handle_line(encode_message({"type": "record_cancel"}))
        process.terminate.assert_called_once()
        self.assertIsNone(bridge.speech_process)
        self.assertFalse(bridge.capture.active)


if __name__ == "__main__":
    unittest.main()
