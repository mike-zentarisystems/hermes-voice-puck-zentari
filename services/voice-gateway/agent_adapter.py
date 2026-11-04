"""Opt-in authenticated agent transport. Never falls back to the v1 model probe."""
import asyncio
import json
import logging
import os
from pathlib import Path

from agent_contract import REQUEST_LIMIT, RESPONSE_LIMIT, encode, validate_request, validate_response
from voice_turn import TurnError


class UnixAgent:
    requires_turn_context = True

    def __init__(self, path):
        self.path = path
        self.devices = frozenset(x.strip() for x in os.getenv('VOICE_AGENT_DEVICE_IDS','').split(',') if x.strip())
        self.timeout = int(os.getenv('VOICE_AGENT_TIMEOUT_SECONDS','29'))
        if not 29 <= self.timeout <= 89: raise ValueError('agent_timeout_invalid')
        self.stage_timeout = self.timeout + 1
        from hermes_adapter import UnixHermes
        self.legacy = UnixHermes() if self.devices else None

    def uses_agent_for(self, device):
        return not self.devices or device in self.devices

    @property
    def ready(self):
        return bool(self.path) and Path(self.path).is_socket() and (self.legacy is None or self.legacy.ready)

    async def answer(self, transcript):
        if self.legacy is not None:
            return await self.legacy.answer(transcript)  # Explicit non-agent device route only.
        raise TurnError('hermes_failed')  # No unauthenticated/plain-text entry point.

    async def answer_turn(self, transcript, history, context, approved_profile):
        writer = None
        try:
            request = validate_request(dict(v=2, **context, user_message=transcript,
                                            history=history, approved_profile=approved_profile))
            payload = encode(request)
            if len(payload) > REQUEST_LIMIT or not self.path:
                raise ValueError()
            if not self.uses_agent_for(context['device_id']): raise ValueError('agent_device_not_selected')
            async with asyncio.timeout(self.timeout):
                reader, writer = await asyncio.open_unix_connection(self.path, limit=RESPONSE_LIMIT+5)
                writer.write(len(payload).to_bytes(4, 'big')+payload)
                await writer.drain()
                header = await reader.readexactly(5)
                length = int.from_bytes(header[1:], 'big')
                if header[0] != 1 or not 0 < length <= RESPONSE_LIMIT:
                    raise ValueError()
                result = json.loads(await reader.readexactly(length))
                answer = validate_response(result, request)
                # Trusted backend fields only; never log arguments, answers, profile,
                # principal name, or tool result payloads.
                allowed = ('event', 'tool_name', 'operation_class', 'status', 'device_id',
                           'session_id', 'authenticated_turn_correlation', 'principal_binding_sha256',
                           'tool_request_sha256', 'result_sha256', 'agent_revision', 'config_sha256',
                           'collected_utc')
                for trace in result['tool_trace']:
                    logging.getLogger('voice_agent').info(json.dumps({k:trace[k] for k in allowed if k in trace}))
                return answer
        except asyncio.TimeoutError as error:
            raise TurnError('hermes_timeout') from error
        except (OSError, ValueError, TypeError, KeyError, UnicodeError, asyncio.IncompleteReadError) as error:
            raise TurnError('hermes_failed') from error
        finally:
            if writer is not None:
                writer.close()
                try:
                    await asyncio.wait_for(writer.wait_closed(), 1)
                except (OSError, asyncio.TimeoutError):
                    pass
