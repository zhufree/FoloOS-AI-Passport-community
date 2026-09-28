import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, MagicMock
from urllib.error import HTTPError

from tools import qwen_speech
from tools.mac_bridge import MacSpeechRecognizer


class QwenSpeechTests(unittest.TestCase):
    def config(self):
        return {'provider': 'qwen', 'notification_mode': 'tone', 'api_key': 'test-secret',
                'base_url': 'https://dashscope.aliyuncs.com/compatible-mode/v1', 'model': 'qwen3-asr-flash'}

    def test_private_save_and_default(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'speech.json'
            self.assertEqual(qwen_speech.load_config(path)['provider'], 'apple')
            qwen_speech.save_config(self.config(), path)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(qwen_speech.load_config(path), self.config())
            before = path.read_bytes()
            with self.assertRaises(ValueError):
                qwen_speech.save_config({**self.config(), 'api_key': ''}, path)
            self.assertEqual(path.read_bytes(), before)

    def test_validation(self):
        for changes in ({'base_url': 'http://example.com'}, {'model': 'qwen-plus'},
                        {'model': 'qwen3-asr-flash-realtime'}, {'api_key': 'x\ny'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                qwen_speech.validate_config({**self.config(), **changes})

    def test_wav_payload_and_mixed_transcript(self):
        response = MagicMock()
        response.__enter__.return_value = io.StringIO(json.dumps({
            'choices': [{'message': {'content': '请修复 WebSocket\n不要改 LVGL'}}]}))
        with patch.object(Path, 'read_bytes', return_value=b'RIFF-test'), \
                patch('urllib.request.build_opener') as factory:
            factory.return_value.open.return_value = response
            text = qwen_speech.transcribe(Path('test.wav'), self.config())
            request = factory.return_value.open.call_args.args[0]
            payload = json.loads(request.data)
            self.assertNotIn('language', payload['asr_options'])
            self.assertEqual(payload['messages'][0]['content'][0]['input_audio']['data'],
                             'data:audio/wav;base64,UklGRi10ZXN0')
            self.assertEqual(text, '请修复 WebSocket 不要改 LVGL')

    def test_errors_never_echo_server_body(self):
        with patch.object(Path, 'read_bytes', return_value=b'RIFF'), \
                patch('urllib.request.build_opener') as factory:
            factory.return_value.open.side_effect = HTTPError(
                'https://example.com', 401, 'test-secret', {}, io.BytesIO(b'test-secret'))
            with self.assertRaisesRegex(ValueError, 'API Key') as error:
                qwen_speech.transcribe(Path('test.wav'), self.config())
            self.assertNotIn('test-secret', str(error.exception))

    def test_qwen_bypasses_apple_and_keeps_key_out_of_argv(self):
        recognizer = MacSpeechRecognizer(Path('/tools'))
        with patch('tools.mac_bridge.load_speech_config', return_value=self.config()), \
                patch.object(recognizer, 'ensure_helper') as apple, \
                patch('tools.mac_bridge.subprocess.Popen') as process:
            recognizer.start(Path('/record.wav'))
            apple.assert_not_called()
            self.assertIn('/tools/qwen_speech.py', process.call_args.args[0])
            self.assertNotIn('test-secret', str(process.call_args))

    def test_apple_remains_default(self):
        recognizer = MacSpeechRecognizer(Path('/tools'))
        with patch('tools.mac_bridge.load_speech_config', return_value={'provider': 'apple'}), \
                patch.object(recognizer, 'ensure_helper') as apple, \
                patch('tools.mac_bridge.subprocess.Popen'):
            recognizer.start(Path('/record.wav'))
            apple.assert_called_once()

    def test_native_31_protocol_and_domain_normalization(self):
        for output in ({'text': '修改 WebSocket'},
                       {'sentence': {'text': '修改 WebSocket'}},
                       {'output': {'sentence': {'text': '修改 WebSocket'}}}):
            response = MagicMock()
            response.__enter__.return_value = io.StringIO(json.dumps({'output': output}))
            config = {**self.config(), 'model': 'qwen-audio-3.1-asr-flash',
                      'base_url': 'ws-example.cn-beijing.maas.aliyuncs.com'}
            with patch.object(Path, 'read_bytes', return_value=b'RIFF-test'), \
                    patch('urllib.request.build_opener') as factory:
                factory.return_value.open.return_value = response
                self.assertEqual(qwen_speech.transcribe(Path('test.wav'), config), '修改 WebSocket')
                request = factory.return_value.open.call_args.args[0]
                self.assertEqual(request.full_url,
                    'https://ws-example.cn-beijing.maas.aliyuncs.com' + qwen_speech.NATIVE_PATH)
                body = json.loads(request.data)
                self.assertIn('input', body)
                self.assertEqual(body['parameters']['language_hints'], ['zh', 'en'])
                self.assertNotIn('asr_options', body)

    def test_native_endpoint_paths_are_not_duplicated(self):
        for path in ('', '/api/v1', qwen_speech.NATIVE_PATH, '/compatible-mode/v1'):
            config = {**self.config(), 'model': 'qwen-audio-3.1-asr-flash',
                      'base_url': 'https://example.com' + path}
            response = MagicMock()
            response.__enter__.return_value = io.StringIO('{"output":{"text":"ok"}}')
            with patch.object(Path, 'read_bytes', return_value=b'RIFF'), \
                    patch('urllib.request.build_opener') as factory:
                factory.return_value.open.return_value = response
                qwen_speech.transcribe(Path('test.wav'), config)
                self.assertEqual(factory.return_value.open.call_args.args[0].full_url,
                                 'https://example.com' + qwen_speech.NATIVE_PATH)

    def test_ready_reports_active_provider_without_credentials(self):
        from tools.mac_bridge import SerialBridge
        bridge = SerialBridge('/unused', speech=MacSpeechRecognizer(Path('/tools')))
        for provider in ('apple', 'qwen'):
            config = {**self.config(), 'provider': provider,
                      'model': 'qwen-audio-3.1-asr-flash', 'notification_mode': 'silent'}
            with patch('tools.mac_bridge.load_speech_config', return_value=config):
                message = bridge.ready_message()
            self.assertEqual(message['asr_provider'], provider)
            self.assertEqual(message['notification_mode'], 'silent')
            self.assertNotIn('test-secret', json.dumps(message))
            self.assertNotIn('base_url', message)
        with patch('tools.mac_bridge.load_speech_config', side_effect=ValueError('invalid')):
            self.assertEqual(bridge.ready_message()['asr_provider'], 'unavailable')

    def test_notification_defaults_migration_and_validation(self):
        old = self.config()
        del old['notification_mode']
        self.assertEqual(qwen_speech.validate_config(old)['notification_mode'], 'tone')
        for mode in ('silent', 'tone', 'speech'):
            self.assertEqual(qwen_speech.validate_config({**old, 'notification_mode': mode})[
                'notification_mode'], mode)
        with self.assertRaises(ValueError):
            qwen_speech.validate_config({**old, 'notification_mode': 'invalid'})
