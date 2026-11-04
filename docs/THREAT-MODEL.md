# Threat model and privacy boundaries

Assets: microphone input, conversations, approved profiles, passwords, gateway
HMAC keys, backend keys, trusted TLS roots and firmware signing authority.
Trust domains: browser origin, session bridge, authenticated gateway, local
speech subprocesses, explicitly configured model backend, host filesystem,
reference hardware, dependency/build system and release account.

| Threat | Implemented control | Remaining acceptance |
| --- | --- | --- |
| LAN interception or malicious TLS peer | Loopback bindings; verified WSS/HTTPS; minimum TLS 1.2; exact origin/Host | Fresh unfamiliar install and trust review |
| Password guessing and resource exhaustion | Scrypt; bounded login attempts/hash concurrency; session/socket caps; container CPU/RAM/PID limits | Measured adversarial load and independent review |
| CSRF, session fixation, logout races | Secure HttpOnly Strict cookie, random CSRF, socket retirement, fixed expiry | Independent review; notification/incident drill |
| Malformed/replayed frames | HMAC nonce handshake, session identity and sequence, frame/audio/turn bounds | Extended fuzzing; hardware path negatives |
| Cross-user context | Server-only device/principal binding; RAM bounds; reset/idle expiry | Human review; authenticated browser is single-owner |
| XSS and cache disclosure | Text-safe message rendering; CSP; static-root boundary; shell-only cache allowlist; no API caching | Independent XSS/service-worker review |
| Rogue model or prompt injection | No tools; structured role history; fixed errors; response cap; redirects denied | Model output is untrusted, never action authority |
| Unintended cloud data egress | Local endpoint default; remote HTTPS explicit opt-in; no automatic fallback | Operator review of chosen backend and retention |
| Worker credential theft | Minimal STT/TTS environment, read-only mounts, non-root container | Host root/Docker administrators remain trusted |
| Supply-chain changes | Action/image digest pins; exact wheel hashes, SBOM and source manifest | Independent license/vulnerability review and artifact signature |
| Malicious update or stolen puck | Default source does not enable approval/OTA; no signing keys shipped | Signed update, rollback, recovery and physical approval tests |
| Private source/history leakage | Allowlisted new tree; no copied .git; content scan; private provenance stored outside export | Independent final tree/history review |

No raw audio or transcript telemetry is persisted by this application by default.
Conversation context is in RAM; restarting drops it. Diagnostic IDs/codes can
still reveal activity timing and should be handled privately. Backend retention
depends on the configured provider and is not controlled by this repository.
Browser preference storage holds avatar/volume/motion settings, never passwords
or gateway secrets. Explicit transcript export is a user-controlled local download.

Limits: registry expiry is checked per event, not pushed immediately into idle
sockets; a session expires within its configured TTL. Root/host compromise is
outside the process boundary. The CPU/memory limits do not constitute a network
egress firewall. The owner password is not individual user authentication.
No independent security review, verified disclosure receipt or hardware security
acceptance is claimed by the implementation tests.
