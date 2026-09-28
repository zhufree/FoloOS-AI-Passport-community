#!/usr/bin/env python3
"""Qwen file transcription and private, per-user speech configuration."""
from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request

CONFIG_PATH = Path.home() / 'Library/Application Support/FoloOS/speech.json'
DEFAULT_URL = 'https://dashscope.aliyuncs.com/api/v1'
DEFAULT_MODEL = 'qwen-audio-3.1-asr-flash'
NATIVE_PATH = '/api/v1/services/aigc/multimodal-generation/generation'


def validate_config(config: dict) -> dict:
    if not isinstance(config, dict):
        raise ValueError('语音配置格式错误')
    provider = config.get('provider', 'apple')
    if provider not in ('apple', 'qwen'):
        raise ValueError('请选择 Apple 或 Qwen')
    notification = config.get('notification_mode', 'tone')
    if notification not in ('silent', 'tone', 'speech'):
        raise ValueError('提醒方式必须为静音、提示音或语音播报')
    result = {'provider': provider, 'notification_mode': notification}
    for name, default in [('api_key', ''), ('base_url', DEFAULT_URL), ('model', DEFAULT_MODEL)]:
        value = config.get(name, default)
        if not isinstance(value, str):
            raise ValueError('语音配置字段必须是文本')
        result[name] = value.strip()
    if provider == 'qwen':
        if not result['api_key'] or any(c.isspace() for c in result['api_key']):
            raise ValueError('请填写有效的 API Key（不能包含空白）')
        if '://' not in result['base_url']:
            result['base_url'] = 'https://' + result['base_url']
        url = urllib.parse.urlsplit(result['base_url'])
        if (url.scheme != 'https' or not url.hostname or url.username or url.password
                or url.query or url.fragment):
            raise ValueError('接口地址必须是 HTTPS Base URL，不能包含账号、查询参数或片段')
        if not re.fullmatch(r'(?:qwen3-asr-flash(?:-\d{4}-\d{2}-\d{2})?|qwen-audio-3\.[01]-asr-flash)', result['model']):
            raise ValueError('支持 qwen-audio-3.1-asr-flash、3.0 和 qwen3-asr-flash，不支持 streaming/realtime/filetrans')
        if url.path.rstrip('/') not in ('', '/api/v1', NATIVE_PATH, '/compatible-mode/v1', '/compatible-mode/v1/chat/completions'):
            raise ValueError('请填写服务域名或标准百炼 Base URL')
    result['base_url'] = result['base_url'].rstrip('/')
    return result


def load_config(path: Path = CONFIG_PATH) -> dict:
    try:
        return validate_config(json.loads(path.read_text()))
    except FileNotFoundError:
        return validate_config({})
    except (OSError, ValueError):
        raise ValueError('无法读取语音配置，请在 Mac App 中重新保存') from None


def save_config(config: dict, path: Path = CONFIG_PATH) -> None:
    config = validate_config(config)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.speech-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as output:
            json.dump(config, output, ensure_ascii=False)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # Never forward a credential or recording to a redirected host.


def transcribe(wav_path: Path, config: dict) -> str:
    config = validate_config(config)
    if config['provider'] != 'qwen':
        raise ValueError('尚未启用 Qwen')
    audio = wav_path.read_bytes()
    if not audio or len(audio) > 7_000_000:
        raise ValueError('录音为空或超过 Qwen 上传限制')
    payload = {
        'model': config['model'],
        'messages': [{'role': 'user', 'content': [{'type': 'input_audio', 'input_audio': {
            'data': 'data:audio/wav;base64,' + base64.b64encode(audio).decode('ascii')
        }}]}],
        'stream': False,
        'asr_options': {'enable_itn': True},
    }
    native = config['model'].startswith('qwen-audio-')
    url = urllib.parse.urlsplit(config['base_url'])
    endpoint = urllib.parse.urlunsplit((url.scheme, url.netloc,
        NATIVE_PATH if native else '/compatible-mode/v1/chat/completions', '', ''))
    if native:
        payload = {'model': config['model'], 'input': {'messages': payload['messages']},
                   'parameters': {'format': 'wav', 'language_hints': ['zh', 'en']}}
    request = urllib.request.Request(
        endpoint,
        data=json.dumps(payload).encode(),
        headers={'Authorization': 'Bearer ' + config['api_key'], 'Content-Type': 'application/json',
                 'X-DashScope-SSE': 'disable'},
    )
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=30) as response:
            result = json.load(response)
    except urllib.error.HTTPError as error:
        messages = {401: 'API Key 无效或地域不匹配', 403: '无模型访问权限或服务未开通',
                    429: '额度或调用频率受限', 400: '请检查接口地址和模型名称',
                    404: '接口或模型不存在'}
        raise ValueError('Qwen：' + messages.get(error.code, f'服务返回 HTTP {error.code}')) from None
    except (OSError, ValueError):
        raise ValueError('Qwen 连接超时、网络异常或返回格式错误，请重试') from None
    try:
        if native:
            output = result['output']
            text = output.get('text')
            if text is None:
                text = output.get('output', output)['sentence']['text']
        else:
            text = result['choices'][0]['message']['content']
        if not isinstance(text, str) or not text.strip():
            raise ValueError()
    except (KeyError, IndexError, TypeError, ValueError):
        raise ValueError('Qwen 未返回可用文字，请重试') from None
    return ' '.join(text.splitlines()).strip()


def main() -> int:
    try:
        if sys.argv[1:] == ['--save']:
            save_config(json.load(sys.stdin))
            print('语音设置已保存。新版桥接将在下一次录音时读取设置。')
        elif len(sys.argv) == 2:
            print(transcribe(Path(sys.argv[1]), load_config()))
        else:
            raise ValueError('缺少录音文件')
        return 0
    except (OSError, ValueError):
        # Only our curated errors may leave this process; never print server bodies or keys.
        error = sys.exc_info()[1]
        print(str(error) if isinstance(error, ValueError) and not isinstance(error, json.JSONDecodeError)
              else '无法读取或保存语音配置/录音文件', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
