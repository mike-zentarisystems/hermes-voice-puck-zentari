# Install, diagnose and recover

Use the root README for the complete local quick start. Run setup as an ordinary
Linux user; do not run the application as root. `.runtime` is mode 700 and
private JSON/key files are mode 600. Compose uses that same UID/GID. On Windows,
use WSL2 with the checkout in the Linux filesystem so these modes are meaningful.

## TLS trust

The setup certificate is local to this installation and expires with the chosen
credential lifetime (30 days by default). Compare its SHA-256 fingerprint in
your own terminal before trusting the public authority certificate in your local OS/browser.
The service receives a leaf certificate with CA:FALSE. The temporary authority signing key is discarded during setup and is never mounted into the service. The server private key never belongs in the browser, repository, screenshots, or issues.
The browser bridge verifies both the gateway certificate chain and `localhost`.
No production path uses `verify=False`, `curl -k`, or certificate-error bypasses.
Browser test exceptions are confined to generated synthetic fixture certificates.

Your browser may require its own certificate import in addition to OS trust.
After import, verify the connection is trusted before entering the password.
Remove that certificate from your trust store when retiring the installation.
For a remote service, supply a separately reviewed valid TLS/proxy setup; the
loopback quick start does not constitute an internet hosting configuration.

## Model integrity

No model weight, voice, font, or third-party portrait is distributed. Select a
model with a license allowing your intended use. Retain its model card, immutable
upstream revision, source URL, license, and expected SHA-256 hashes obtained from
a trusted upstream channel. Verify the downloaded bytes against those hashes
before mounting them. A checksum computed only after downloading is an inventory,
not independent provenance verification.

Expected paths:

| Path | Format |
| --- | --- |
| `models/stt/model.bin` | faster-whisper/CTranslate2 model |
| `models/stt/config.json` | matching model configuration |
| `models/stt/tokenizer.json` | matching tokenizer |
| `models/stt/vocabulary.txt` or `vocabulary.json` | matching CTranslate2 vocabulary |
| `models/voice.onnx` | Piper voice |
| `models/voice.onnx.json` | matching Piper voice configuration |

Models mount read-only. STT/TTS workers receive a minimal environment without
backend keys, gateway credentials or inherited proxy variables. Worker thread
counts are limited to avoid exhausting the container task limit. The TTS worker
stays warm; STT is bounded to the requested turn. No tool execution is provided.

## Diagnosis

Warm the chosen local chat model before its first turn. Model loading can exceed
the bounded 28-second HTTP stage even when later inference is fast. A cold-start
timeout is reported as a failed turn; it does not trigger a cloud fallback.

For a **dedicated test installation only**, `tools/accept_local.py` exercises real
text/speech, context reset, cancellation and logout. It clears that test owner's
RAM conversation. In a private Linux terminal, store its test password in a
mode-600 file under `.runtime`, then stream the tool into the running image:

```bash
umask 077
read -rsp 'Dedicated test password: ' ZENTI_TEST_PASSWORD
printf '%s' "$ZENTI_TEST_PASSWORD" > .runtime/acceptance-password
unset ZENTI_TEST_PASSWORD
docker compose exec -T -e PYTHONPATH=/app/pwa:/app/services/voice-gateway voice \
  python - --config /run/zenti --password-file /run/zenti/acceptance-password \
  --tts-model /models/voice.onnx < tools/accept_local.py
```

The report contains booleans, timings and waveform hashes; it writes no transcript
or audio. The input is synthesized locally, so a pass is real-model integration
evidence, not physical microphone or subjective listening acceptance. Remove the
private test password file after the test and retire the dedicated test credentials.

`python3 tools/doctor.py` reports prerequisite, private-file, expiry and model
presence checks without values. `--live` additionally verifies the local HTTPS
health endpoint with the configured certificate. `/healthz` checks local model
presence and registry readiness; it does not prove the chat backend is reachable
or that a model can generate sensible speech. Send an actual test turn to prove
that path. Inspect Docker logs privately; share only sanitized fixed error categories.

| Symptom | Check |
| --- | --- |
| Container exits at startup | Private file owner/mode, matching Compose UID, TLS and voice model compatibility |
| Sign-in fails | Password, exact origin, expiry, cookie storage; wait for login rate limit to clear |
| Browser TLS warning | Trust the correct local certificate after fingerprint verification; do not bypass |
| Text works but speech fails | Piper files and format, dependency compatibility, model resource limits |
| Backend unavailable | Exact `/v1` endpoint/model, restricted host reachability, optional key permissions |
| Microphone unavailable | Trusted HTTPS, OS/browser permission, selected input device |
| Unexpected recording | Mute input and exit hands-free; report sanitized reproduction |

## Update and rollback

Build a new image under a distinct tag from its reviewed source and save the
previous immutable image digest and source archive. Back up `.runtime` privately
before a configuration migration. Stop only this stack with `docker compose stop`;
never prune shared Docker volumes or images. Restore the previous image and its
compatible configuration, then sign in and check one text turn, one voice turn,
Stop, and reset. An executed rollback is still a release acceptance gate.

To retire local credentials, stop the stack and preserve the encrypted/private
backup you need. To reset a password without changing identity, use
`tools/provision_pwa_password.py --output` with the absolute path of
`.runtime/password.json` and `--reset` in a private Linux terminal, then recreate
this container. This ends all browser sessions. Regenerate an expired installation
in a NEW directory, review the new certificate and rotate the enrolled identities;
do not silently extend expiry or overwrite the known-good directory.
