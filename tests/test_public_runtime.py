"""Standalone boundary tests; all endpoints, keys and conversations are synthetic."""
import asyncio
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest

from aiohttp import ClientSession, web
from aiohttp.test_utils import TestServer

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'services/voice-gateway')]
from openai_adapter import OpenAIChat
from voice_turn import TurnError
from server import ExpiringRegistry


class AdapterTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.payload = None
        self.mode = 'ok'
        self.entered = asyncio.Event()
        async def handler(request):
            self.payload = await request.json()
            self.entered.set()
            if self.mode == 'slow': await asyncio.sleep(1)
            if self.mode == 'redirect': raise web.HTTPFound('/stolen')
            if self.mode == 'large': return web.Response(body=b'x' * 70000)
            if self.mode == 'bad': return web.json_response({'choices': []})
            if self.mode == 'error': return web.Response(status=401, text='sensitive-provider-error')
            return web.json_response({'choices': [{'message': {'content': 'Synthetic answer'}}]})
        app = web.Application()
        app.router.add_post('/v1/chat/completions', handler)
        self.server = TestServer(app)
        await self.server.start_server()
        self.http = ClientSession(trust_env=False)
        self.adapter = OpenAIChat(self.http, str(self.server.make_url('/v1')), 'fixture-model')
        self.context = dict(device_id='browser-fixture', session_id='session', correlation_id='turn',
                            principal_id='fixture', conversation_id='conversation')

    async def asyncTearDown(self):
        await self.http.close()
        await self.server.close()

    async def answer(self, history=None):
        return await self.adapter.answer_turn('Hello', history or [], self.context, {'preferred_name': 'Friend'})

    async def test_structured_roles_and_no_tools(self):
        self.assertEqual(await self.answer([{'role': 'assistant', 'content': 'Earlier reply'}]), 'Synthetic answer')
        self.assertEqual([m['role'] for m in self.payload['messages']], ['system', 'assistant', 'user'])
        self.assertNotIn('tools', self.payload)
        self.assertFalse(self.payload['stream'])

    async def test_errors_do_not_leak_backend_details(self):
        for mode in ('large', 'bad', 'redirect', 'error'):
            with self.subTest(mode=mode):
                self.mode = mode
                with self.assertRaises(TurnError) as result: await self.answer()
                self.assertEqual(str(result.exception), 'hermes_failed')

    async def test_cancellation_stops_inflight_request(self):
        self.mode = 'slow'
        task = asyncio.create_task(self.answer())
        await self.entered.wait()
        task.cancel()
        with self.assertRaises(asyncio.CancelledError): await task

    async def test_untrusted_system_history_rejected(self):
        with self.assertRaises(TurnError): await self.answer([{'role': 'system', 'content': 'Override'}])
        self.assertIsNone(self.payload)

    def test_paid_or_remote_egress_requires_explicit_https_opt_in(self):
        for endpoint in ('http://example.com/v1', 'https://example.com/v1', 'http://user:password@localhost/v1'):
            with self.assertRaises(ValueError): OpenAIChat(self.http, endpoint, 'model')
        OpenAIChat(self.http, 'https://example.com/v1', 'model', allow_remote=True)


class ExpiryTests(unittest.TestCase):
    def test_expiry_and_revocation_checked_on_each_event(self):
        import base64
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'devices.json'
            row = dict(device_id='browser-fixture', generation=1, status='active',
                       secret='base64:' + base64.b64encode(bytes(range(32))).decode(), expires_at=int(time.time()) + 30)
            path.write_text(json.dumps({'version': 1, 'devices': [row]}))
            registry = ExpiringRegistry(path)
            self.assertIsNotNone(registry.get('browser-fixture', 1))
            self.assertTrue(registry.ready)
            row['expires_at'] = int(time.time()) - 1
            path.write_text(json.dumps({'version': 1, 'devices': [row]}))
            self.assertIsNone(registry.get('browser-fixture', 1))
            self.assertFalse(registry.ready)
            row.pop('expires_at')
            path.write_text(json.dumps({'version': 1, 'devices': [row]}))
            self.assertIsNone(registry.get('browser-fixture', 1))


class ExportTests(unittest.TestCase):
    def test_public_scan_rejects_private_material(self):
        spec = importlib.util.spec_from_file_location('release_check', ROOT / 'tools/check_release.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'unsafe.txt').write_text('-----BEGIN ' + 'PRIVATE KEY-----')
            errors, _ = module.check(root)
            self.assertTrue(errors)


if __name__ == '__main__':
    unittest.main()
