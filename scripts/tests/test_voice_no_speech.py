"""A successful empty STT result differs from crashed or empty TTS workers."""
import asyncio
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'services/voice-gateway'))
import local_adapters
from voice_turn import TurnError


class EmptyWorkerTests(unittest.IsolatedAsyncioTestCase):
    async def test_stt_child_bounds_threads_and_does_not_inherit_gateway_environment(self):
        launch = asyncio.create_subprocess_exec
        async def spawn(*args, **kwargs):
            return await launch(sys.executable, '-c',
                'import os,json;print(json.dumps({k:os.getenv(k) for k in '
                '["OPENBLAS_NUM_THREADS","OMP_NUM_THREADS","VOICE_TEST_SECRET"]}))', **kwargs)
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {
                'OPENBLAS_NUM_THREADS':'32', 'OMP_NUM_THREADS':'32', 'VOICE_TEST_SECRET':'fixture-only'}):
            with patch.object(local_adapters.asyncio,'create_subprocess_exec',side_effect=spawn):
                output=await local_adapters._run('stt',b'\0\0',temp,4096)
        self.assertEqual(json.loads(output), {'OPENBLAS_NUM_THREADS':'1',
                         'OMP_NUM_THREADS':'1','VOICE_TEST_SECRET':None})

    async def test_real_subprocess_exit_status_preserves_failure_categories(self):
        launch = asyncio.create_subprocess_exec
        processes = []
        with tempfile.TemporaryDirectory() as temp:
            for kind, exit_code, payload, expected in (
                ('stt',0,b'',None), ('stt',7,b'','stt_failed'),
                ('tts',0,b'','tts_failed'), ('stt',0,b'x'*17,'stt_failed')):
                with self.subTest(kind=kind,exit_code=exit_code,size=len(payload)):
                    async def spawn(*args,**kwargs):
                        process = await launch(sys.executable,'-c',
                            'import sys;sys.stdin.buffer.read();sys.stdout.buffer.write('+repr(payload)+');sys.exit('+str(exit_code)+')',**kwargs)
                        processes.append(process)
                        return process
                    model = Path(temp) if kind=='stt' else Path(temp)/'tts.onnx'
                    if kind=='tts': model.touch()
                    with patch.object(local_adapters.asyncio,'create_subprocess_exec',side_effect=spawn):
                        if expected:
                            with self.assertRaises(TurnError) as error:
                                await local_adapters._run(kind,b'\0\0',str(model),16)
                            self.assertEqual(error.exception.code,expected)
                        else:
                            self.assertEqual(await local_adapters._run(kind,b'\0\0',str(model),16),b'')
        self.assertTrue(all(process.returncode is not None for process in processes))
