# Hermes Voice Agent

**A private voice helper you run yourself, in your browser or on a small round puck.**

Talk or type, and it answers out loud. Speech recognition, the chat model and
the voice all run on hardware you control. Nothing goes to a cloud service
unless you point it at one.

New here? Read the [plain-English overview (eli10.md)](eli10.md) first.

[Try the demo](#try-the-demo-in-one-minute) ·
[Run it for real](#run-the-real-browser-client-on-linux) ·
[Install the app](docs/INSTALL.md#install-the-browser-pwa) ·
[Puck parts](docs/PUCK-MATERIALS.md) ·
[How it works](docs/ARCHITECTURE.md)

![Hermes Voice Agent browser PWA and experimental ESP32-S3 puck, with the official Nous Research Hermes logo. Direct Hermes integration is planned.](docs/media/zentari-voice-overview.png)

> **Experimental preview.** The browser app works. The puck firmware is an
> experiment. Connecting to the [Hermes agent](docs/HERMES-INTEGRATION.md) is
> the next step and is **not** in this release. See [release status](docs/RELEASE-STATUS.md).

## Names used in this project

- **Hermes Voice Agent** is the project and the browser app, built by Zentari Systems.
  It was previously called Zentari Voice.
- **The puck** is the optional round touch-screen device.
- **Zenti** and **Orbit** are the two on-screen characters you can choose.
- **Hermes** is the [Nous Research agent](https://github.com/NousResearch/hermes-agent)
  this project plans to connect to. The repository name refers to that goal.

## What works today

| Status | What |
| --- | --- |
| **Works** | Browser app: talk, type, Stop, mute, new conversation, save a transcript, install as an app. Password-protected local gateway with your chosen chat model. |
| **Experimental** | Puck firmware and touch controls. Signed approvals and over-the-air updates exist in source but are off by default. |
| **Planned** | Direct Hermes agent connection, moving a conversation between phone and puck, approving agent actions on screen, guided setup. |

Browser and puck conversations are separate today; they do not sync.

## Watch the puck

<a href="docs/media/video/puck-demo-2026-11-07.mp4"><img src="docs/media/puck-demo-still.png" width="270" alt="Frame from the physical puck video, showing its Speaking state and touch controls."></a>

[14-second video (MP4)](docs/media/video/puck-demo-2026-11-07.mp4) of a puck
showing touch-to-talk and its Listening, Thinking and Speaking states. It was
recorded on an earlier firmware build, so it shows the idea, not proof that this
exact source has passed hardware testing. [About the media](docs/media/README.md).

## Try the demo in one minute

No account, model or hardware needed. You need Python 3.11 or newer.

```bash
python3 tools/build_site.py
python3 -m http.server 8080 --bind 127.0.0.1 --directory dist/site
```

Open <http://localhost:8080>. The answers are scripted: nothing listens to your
microphone and nothing leaves the page. **Stop** cancels the pending reply.

## Run the real browser client on Linux

### Before you start

- Linux (Ubuntu 24.04 or newer) on an **x86-64 (Intel/AMD)** machine. The
  container and its locked Python packages are built for `linux/amd64` only, so
  ARM machines such as Raspberry Pi or Apple Silicon are not supported yet.
  On Windows, use WSL2 with the checkout inside the Linux filesystem.
- At least 8 GB of memory. The gateway container is capped at 4 GB and four CPUs.
- Docker with Compose v2, Python 3.11 or newer, and OpenSSL.
- [Ollama](https://ollama.com) or another OpenAI-compatible chat server.

### 1. Get a chat model and let the container reach it

Download a model. Setup defaults to `qwen3.5:4b`; this preview was last checked
end to end with `qwen2.5:7b`. Use whichever you pass to setup with `--model`.

```bash
ollama pull qwen3.5:4b
```

Ollama only listens on `127.0.0.1` by default, which the container cannot
reach. Make it listen on Docker's host-side bridge address instead, so it is
reachable from containers but **not** from your network:

```bash
# Print Docker's host-side bridge address (the docker0 interface):
ip -4 -o addr show docker0 | awk '{print $4}' | cut -d/ -f1

# Tell Ollama to listen there. In the editor, add under [Service]:
#   Environment="OLLAMA_HOST=<address printed above>:11434"
sudo systemctl edit ollama.service
sudo systemctl daemon-reload
sudo systemctl restart ollama
```

After this change, run host-side `ollama` commands with
`OLLAMA_HOST=<that address>:11434` in front. Do not set `OLLAMA_HOST=0.0.0.0`
on a machine others can reach, and never expose Ollama to the internet.

### 2. Download the speech models

Weights are not included. These are the speech models this preview was
checked with; review each model card and license before you use them.

```bash
python3 -m venv .venv
.venv/bin/python -m pip install huggingface_hub
mkdir -p models/stt
.venv/bin/hf download Systran/faster-whisper-tiny.en --local-dir models/stt
curl -L -o models/voice.onnx \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/ljspeech/medium/en_US-ljspeech-medium.onnx
curl -L -o models/voice.onnx.json \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/ljspeech/medium/en_US-ljspeech-medium.onnx.json
```

You can use any faster-whisper (CTranslate2) model and any Piper voice instead.
`models/stt/` needs `model.bin`, `config.json`, `tokenizer.json` and
`vocabulary.txt` (or `vocabulary.json`). For checksums and provenance, see
[model integrity](docs/INSTALL.md#model-integrity).

### 3. Create your private settings and start

Run this in an ordinary (non-root) Linux terminal:

```bash
.venv/bin/python -m pip install -r pwa/requirements-bridge.txt
.venv/bin/python tools/setup.py
printf 'ZENTI_UID=%s\nZENTI_GID=%s\n' "$(id -u)" "$(id -g)" > .env
.venv/bin/python tools/doctor.py
openssl x509 -in .runtime/ca.pem -noout -fingerprint -sha256
docker compose up --build -d --wait
.venv/bin/python tools/doctor.py --live
```

`tools/setup.py` asks for a password (16 characters or more) and creates a
private `.runtime/` folder with your sign-in settings and a local TLS
certificate that lasts 30 days. If setup fails, it tells you why and you can
run it again.

### 4. Trust the certificate and sign in

1. Check that the SHA-256 fingerprint printed above matches `.runtime/ca.pem`,
   then import **only** `.runtime/ca.pem` into your browser or OS trust store.
   Never import `server-key.pem`, and never click past a certificate warning.
2. Open <https://localhost:8443>, open **Settings** and sign in.

[docs/INSTALL.md](docs/INSTALL.md) covers installing the app on a phone or
desktop, troubleshooting and updates.

### Privacy and network defaults

- The gateway only listens on this computer (`127.0.0.1`). It opens no public
  port and needs no cloud account.
- Your words go only to the chat backend you configured. There is no automatic
  paid fallback. To use a remote HTTPS backend, pass `--allow-remote --backend`
  to setup and put its API key in `.runtime/backend-key` (mode 600). Read that
  provider's data-retention terms first.
- Conversations live in memory only: up to 16 messages per conversation, and
  they are forgotten after 30 minutes of quiet or when you start a new one.
  Audio and transcripts are not logged. **Export** saves a Markdown copy only
  when you ask.

## Daily controls

- **Hold to talk:** records while held. The microphone is only requested after you press it.
- **Mute microphone:** stops listening; a reply that is already playing continues.
- **Cancel response / Stop audio:** stops thinking and speaking.
- **Hands-free:** takes turns automatically and pauses listening while it speaks.
  Mute the microphone to stop. Wake words are not supported.
- **New conversation:** clears the chat on screen and on the server.
- **Settings:** microphone, volume, Zenti or Orbit avatar, spoken replies and
  reduced motion. These stay in that browser.

## Hardware and development

The reference puck is a **Waveshare ESP32-S3-Touch-LCD-1.85C BOX Rev2**. See
[puck parts](docs/PUCK-MATERIALS.md) and [hardware notes](docs/HARDWARE.md).
Pins and vendor details are in [hardware/pinmap.json](hardware/pinmap.json) and
[third_party/waveshare](third_party/waveshare/UPSTREAM.md); a board with a
similar name may have different pins. Firmware targets ESP-IDF 6.1. Do not
flash a working puck with this preview without a full backup.

Run the tests:

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

Browser tests use simulated microphones and models, so they do not prove phone
audio, a real model or charging.

## More

- [How it works](docs/ARCHITECTURE.md) · [Hermes plan](docs/HERMES-INTEGRATION.md)
- [Security](SECURITY.md) · [Threat model](docs/THREAT-MODEL.md)
- [Contributing](CONTRIBUTING.md) · [Support](SUPPORT.md) · [Changelog](CHANGELOG.md)
- [Release status and remaining checks](docs/RELEASE-STATUS.md) · [Release and rollback](docs/RELEASE.md)

## License and credits

Zentari-owned source is under the [Apache-2.0 license](LICENSE); third-party
parts keep their own licenses (see [license inventory](docs/LICENSES.md) and
[NOTICE](NOTICE)). No model weights are included.

The Hermes / Nous Girl logo is © 2025 Nous Research, used under its
[MIT notice](third_party/nous-research/LICENSE). This is an independent project;
it is not affiliated with or endorsed by Nous Research.

Private security or conduct reports: [info@zentarisystems.io](mailto:info@zentarisystems.io).
