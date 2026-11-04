"""Private local text transport. No runtime mounts, provider keys or HTTP fallback."""
from __future__ import annotations
import asyncio
import os
from pathlib import Path
from voice_turn import TEXT_LIMIT, TurnError

class UnixHermes:
    def __init__(self, path=None):
        self.path = path if path is not None else os.getenv("VOICE_HERMES_SOCKET", "")

    @property
    def ready(self):
        return bool(self.path) and Path(self.path).is_socket()

    async def answer(self, transcript):
        payload = transcript.encode("utf-8")
        if not payload.strip() or len(payload) > TEXT_LIMIT:
            raise TurnError("text_invalid")
        if not self.path:
            raise TurnError("hermes_readonly_unavailable")
        writer = None
        try:
            async with asyncio.timeout(29):
                reader, writer = await asyncio.open_unix_connection(self.path, limit=8192)
                writer.write(len(payload).to_bytes(4, "big") + payload)
                await writer.drain()
                header = await reader.readexactly(5)
                length = int.from_bytes(header[1:], "big")
                if header[0] not in (0, 1) or not 0 < length <= TEXT_LIMIT:
                    raise TurnError("hermes_failed")
                result = (await reader.readexactly(length)).decode("utf-8")
                if header[0] == 0:
                    # Never forward untrusted backend text as an error category.
                    raise TurnError("hermes_failed")
                if not result.strip():
                    raise TurnError("hermes_failed")
                return result
        except asyncio.TimeoutError as exc:
            raise TurnError("hermes_timeout") from exc
        except (OSError, UnicodeError, asyncio.IncompleteReadError) as exc:
            raise TurnError("hermes_failed") from exc
        finally:
            if writer is not None:
                writer.close()
                try:
                    await asyncio.wait_for(writer.wait_closed(), 1)
                except (OSError, asyncio.TimeoutError):
                    pass
