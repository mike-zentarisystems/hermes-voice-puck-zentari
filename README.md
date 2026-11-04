# Zenti by Zentari

A local-first voice companion for your browser and a round ESP32-S3 puck.
Talk, type, cancel a reply, reset a conversation, and choose a companion.

**Public preview candidate.** The browser demo is scripted. Hardware source is
experimental. A stable 1.0 release is not accepted; see [release status](docs/RELEASE-STATUS.md).

## Try the web demo — no account or hardware

Python 3.11+ is enough. From this directory:

```bash
python3 tools/build_site.py
python3 -m http.server 8080 --bind 127.0.0.1 --directory dist/site
```

Open <http://localhost:8080>. Everything stays in the page: no model, microphone,
network API, analytics, account, or stored messages. **Stop** cancels the pending
scripted reply. This is an interaction demo, not a live AI performance claim.
The same seven-file site can be served from a GitHub project Pages subdirectory.

## Run the real browser client on Linux

Requires Ubuntu 24.04+, Docker with Compose v2, Python 3.11+, OpenSSL, a local
OpenAI-compatible chat backend, and separately licensed STT/TTS models. Start
with a machine with at least 8 GB RAM; the gateway is capped at 4 GB and four CPUs.
Model performance depends on the CPU and model. No inference speed is promised.

1. Run your local backend with a model you have already installed. The default
   configuration targets Ollama's OpenAI-compatible `/v1` API on port 11434 and
   model `qwen3.5:4b`; choose the exact name your backend serves with `--model`.
   Make the backend reachable from this container using its restricted host
   interface. Do not expose Ollama to the public internet.
2. Put a CTranslate2 faster-whisper model in `models/stt/` (including `model.bin`,
   `config.json`, `tokenizer.json`, and its `vocabulary.txt` or `vocabulary.json`); put a Piper voice in `models/voice.onnx` and
   its configuration in `models/voice.onnx.json`. Review each model card and
   license before downloading or redistributing it. Weights are not included.
   See [model integrity](docs/INSTALL.md#model-integrity).
3. Create private configuration in an ordinary Linux terminal:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r pwa/requirements-bridge.txt
.venv/bin/python tools/setup.py
printf 'ZENTI_UID=%s\nZENTI_GID=%s\n' "$(id -u)" "$(id -g)" > .env
.venv/bin/python tools/doctor.py
openssl x509 -in .runtime/ca.pem -noout -fingerprint -sha256
docker compose up --build -d --wait
.venv/bin/python tools/doctor.py --live
```

Setup asks for a hidden password of at least 16 characters and generates a
dedicated browser identity, a 30-day expiry, and local TLS material. Never paste
the private files into an issue. It refuses an existing configuration directory.
Only trust `.runtime/ca.pem` in your local browser/OS after checking the
displayed SHA-256 fingerprint against that file. Never import `server-key.pem`.
Open <https://localhost:8443>, open **Settings**, and sign in. Do not bypass TLS
verification. [Installation](docs/INSTALL.md) covers trust, models and recovery.

The default Compose publishes only host loopback. It creates no public endpoint,
cloud subscription, privileged mount, Docker socket, or production enrollment.
For a remote HTTPS backend, pass `--allow-remote --backend` explicitly during
setup and store its optional API key in `.runtime/backend-key` with mode 600.
There is no automatic paid fallback. Model requests leave the gateway only for
the configured backend; read its retention terms before opting in.

## Daily controls

- **Hold to talk:** starts capture while held. Microphone access is requested
  only after an explicit user action.
- **Mute microphone:** stops input; it preserves an already playing reply.
- **Cancel response / Stop audio:** cancels processing and queued playback.
- **Hands-free:** half duplex; it pauses input during replies. Stop it with
  microphone mute. Wake words and full duplex are not supported.
- **New conversation:** resets server context and clears the visible transcript.
- **Settings:** microphone selection, volume, neutral Zenti/Orbit avatars,
  voice replies, and reduced motion. Preferences stay on that browser.

Conversation memory is RAM-only, bounded to 16 messages/12 KB per conversation,
64 conversations and a 30-minute idle expiry. It is scoped to authenticated
device plus server-bound principal. A browser bridge is a single-owner service,
not a multi-user account system; its password sessions share that owner's
conversation. Browser and puck contexts do not synchronize. No raw audio or
transcript logs are enabled. Explicit Markdown export downloads a local file.

## Hardware and development

The reference board is Waveshare ESP32-S3-Touch-LCD-1.85C; see the upstream
revision and pin provenance in [hardware](hardware/pinmap.json) and
[vendor notices](third_party/waveshare/UPSTREAM.md). Similar names do not establish
pin compatibility. ESP-IDF 6.1 is the firmware baseline.

```bash
cmake -S tests/host -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
python3 -m unittest discover -s scripts/tests -p 'test_*.py'
python3 -m unittest discover -s tests -p 'test_*.py'
node --test tests/pwa/*.test.cjs
npm ci --prefix tests/pwa --ignore-scripts
node tests/pwa/node_modules/playwright/cli.js install chromium firefox webkit
node --test tests/pwa/browser.e2e.cjs tests/pwa/demo.e2e.cjs
```

Browser tests use synthetic microphones, identities, and speech/model fixtures.
They do not prove phone audio, a real model, or charging. Firmware, approval,
OTA and hardware recovery instructions and limits are in [the hardware guide](docs/HARDWARE.md).
Do not flash the accepted private installation with this preview.

## Source, privacy and contribution

- [Architecture and protocol](docs/ARCHITECTURE.md)
- [Security and reporting](SECURITY.md) · [Threat model](docs/THREAT-MODEL.md)
- [Contributing](CONTRIBUTING.md) · [Support](SUPPORT.md) · [License inventory](docs/LICENSES.md)
- [Release and rollback](docs/RELEASE.md) · [Changelog](CHANGELOG.md)

The source export has no private repository history, production configuration,
customer records or model weights. Procedural public artwork is generated from
source; the original reference portraits are omitted. Existing Apache-2.0 and
vendor notices are retained. Public redistribution of the extracted gateway
still requires the ownership/license decision recorded in release status.
