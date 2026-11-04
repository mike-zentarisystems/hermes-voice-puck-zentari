# Public readiness — 2026-10-26

**Source/web preview candidate prepared. Stable product release remains open.**
The acceptance below applies to this new export, not an older installed puck.
Private source history, infrastructure records, credentials, reference portraits
and model weights are excluded. The preview is not published from this document.

## Real-model review follow-up — 2026-10-29

An isolated Linux container installation built from the source ZIP passed real
faster-whisper tiny.en → local Qwen2.5 7B → Piper LJSpeech medium inference over
verified HTTPS/WSS. Text, context recall/reset, cancellation/recovery, restart,
and rollback from a deliberately failed image back to the exact known-good image
passed. Speech input was a locally synthesized public phrase, not a physical
microphone; no audio or transcript was retained. Ollama cloud routing was disabled.

Three warm runs produced 12 text turns and 3 speech turns: first-audio p50/p95
was 138/434 ms for text and 1309/1387 ms for speech. This small synthetic-input
sample on an RTX 3060 is not a latency SLA, cold-start or physical audio result.
The initial cold chat-model load exceeded the gateway's 28-second backend budget;
warm the selected model before the initial test. The limit remains fail-closed.

The review fixed speech-worker paths, resampling thread accumulation, missing
model-vocabulary readiness, TLS signing-key exposure, and packaging gaps.
This is an engineering review with executable regression tests, not independent
security or legal approval. Hardware, unfamiliar-machine/operator acceptance,
public reporting, distribution rights and public-release authorization remain open.

## Executed software evidence

| Check | Evidence tier and result |
| --- | --- |
| Source/private-content scan | Static: exported tree passes fixed private-host/path/key checks |
| Gitleaks | Static: no leaks with one exact published synthetic JWT plus its exact vector path allowlisted; no fixture directory excluded |
| Dependencies | Linux CPython 3.13 amd64: 34 exact wheel hashes resolved; pinned image builds; pip-audit reports no known Python dependency vulnerabilities |
| Portable firmware core | Native GCC: 22/22 suites pass, including regenerated public RGB565/RLE artwork |
| Gateway | Linux: 76 tests pass, including protocol rejection, context/reset/isolation, cancellation and subprocess lifecycle |
| Browser unit coverage | Native Node: 42 tests pass, including auth, late audio, permission races, bounded queues and cache failure |
| Accessibility | Automated axe WCAG 2/2.1/2.2 A/AA checks pass for phone/desktop PWA states, sign-in dialog and the static demo; actual assistive-technology review remains open |
| Full PWA | Synthetic Chromium and Firefox: sign-in, text/context, cancellation, microphone fixtures, reconnect, reduced motion, avatar persistence, offline shell, logout and 320px layouts pass |
| Static demo | Synthetic Chromium, Firefox and WebKit: scripted interaction, cancellation/reset, keyboard, narrow layouts and no external requests/storage pass |
| Standalone setup/TLS | Linux integration: fresh private setup, verified real HTTPS/WSS, cookie login, local HTTP chat fixture, bounded PCM output and logout pass; inference is synthetic |
| Firmware builds | ESP-IDF 6.1: default and M3a voice-mode builds pass with the procedural public assets; no flash or physical acceptance |
| Compose | Static: configuration parses; host ports are loopback-only, non-root UID, read-only mounts/root, capabilities dropped and resource limits |

No hosted GitHub CI, phone microphone, screen reader, new physical firmware,
hardware charging, signed update, firmware rollback or soak pass is implied.
Python vulnerability results exclude OS/firmware/model analysis. SBOM inventory
and checksums do not constitute legal clearance, artifact signatures or an
independent security audit. Workflow and packaging tools require final-tree checks.

## The original 1.0 gates, with the actual remaining acceptance

| Gate | Engineering progress | Still required for the full gate |
| --- | --- | --- |
| OSS-01 baseline | Prior voice phase accepted; new public art/core tested | Exact new public firmware/web/model/board physical acceptance |
| OSS-02 product/brand | Local-first browser + reference hardware boundary documented | Owner repository/name/trademark decision |
| OSS-03 code/vendor rights | Root/vendor notices retained; 34-package inventory | Gateway ownership/license authorization and dependency/binary obligations review |
| OSS-04 artwork/models | Unclear artwork and weights omitted; original geometric art generated | Confirm final distribution inventory contains only cleared assets |
| OSS-05 standalone gateway | Extracted gateway plus local OpenAI-compatible adapter; real TLS fixture round trip | Independent replication of real local STT/chat/TTS acceptance |
| OSS-06 clean export | Explicit source/overlay allowlist; private provenance outside export; scans | Independent exact final tree and new-history review |
| OSS-07 protocol | Existing schemas/vectors, limits, compatible-client and negative tests retained | Final published compatibility review |
| OSS-08 installation | Demo, private setup, doctor, hardened Compose and synthetic TLS integration | Unfamiliar clean-machine real-model install, measured time |
| OSS-09 accessibility | Automated keyboard/reduced-motion/layout coverage | Actual screen reader/mobile keyboard/contrast review |
| OSS-10 security | Threat model and bounded adapter errors/egress | Independent auth/privacy/update review and findings closure |
| OSS-11 hardening | Origin/CSRF/logout/session tests, expiry, bounds, CSP/cache | Measured adversarial load, full independent review |
| OSS-12 privacy | Device/principal context, reset/expiry tests, RAM-only documented | Independent retention and identity-remap review |
| OSS-13 reporting | SECURITY.md and disclosure procedure | Named private contact; enabled reporting; tested receipt/notifications |
| OSS-14 software/models | Native, Linux gateway, locked image, two full browser engines | Actual phone audio, full WebKit microphone path, independent model replication |
| OSS-15 hardware/performance | Reference board/provenance and C tests retained | New physical baseline and p50/p95/resource measurements |
| OSS-16 charging | Experimental boundary documented | Attended charge/discharge/low-battery/thermal measurements |
| OSS-17 signed approval | Source/tests retained; default mode disabled | Signature, expiry, replay, denial and physical confirmation |
| OSS-18 OTA recovery | Source/policy tests retained; default mode disabled | Wrong/corrupt/untrusted/interrupted image, failed-boot rollback and attended recovery |
| OSS-19 soak | Required procedure documented | Frozen candidate 24-hour then 72-hour physical soak |
| OSS-20 community/docs | Install, architecture, support, conduct, contribution and maintainer docs written | Two unfamiliar reviewers and tested real contact channels |
| OSS-21 release automation | Action/image digest pins, wheel hashes, SBOM, read-only CI and manual Pages workflow | Hosted clean run, firmware matrix and OS/license/scanner coverage |
| OSS-22 artifacts/recovery | Exact source manifest and deterministic packaging tooling | Second-builder/signature review and independent install/rollback replication |
| OSS-23 beta | Explicit experimental scope and known limitations | Consented external browser/hardware beta and blocker closure |
| OSS-24 public authorization | Candidate source can be inspected before publication | Owner approves exact repository, license, manifest and public visibility |
| OSS-25 maintenance | Governance/recovery requirements documented | Named second release-capable maintainer and accepted triage/support duties |

These gates are not marked passed because a checklist or harness exists.
Battery, recovery, independent review, owner decisions and elapsed soak cannot
be completed by source editing. They remain required before a stable 1.0 claim.
