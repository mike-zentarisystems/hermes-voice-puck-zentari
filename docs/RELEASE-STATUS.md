# Public preview status — 2026-11-08

**Experimental source/web preview prepared. Stable product release remains open.**
The acceptance below applies to this new export, not an older installed puck.
Private source history, infrastructure records, credentials and model weights
are excluded. The approved presentation media is identified below; the older
colored portrait is visible only in the owner-supplied footage and its extracted
frame, not distributed as a standalone avatar asset. Actual publication and
channel status are verified separately from this document.

## Current published software evidence — 2026-11-08

The public repository's main revision
[`812adbb59d1320af0bd8bb637e0fe0a4e9efd555`](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari/commit/812adbb59d1320af0bd8bb637e0fe0a4e9efd555)
passed hosted [validation run 42444887109](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari/actions/runs/42444887109).
This evidence applies to that exact revision, not automatically to later changes.
The run used Ubuntu 24.04, CPython 3.13 and Node 24, and completed:

- Source scan: 298 files scanned and fingerprinted.
- Python: 76 gateway/script tests and 46 public/runtime tests.
- Browser unit tests: 47 passed, including unavailable/interrupted audio recovery.
- Portable firmware core: 22 native host suites passed; no device was flashed.
- Chromium: full PWA, scripted demo and automated accessibility tests passed.
- Firefox: full PWA and scripted demo passed with synthetic microphone/model
  fixtures and a virtual PulseAudio sink.
- WebKit: scripted static demo passed; full microphone/voice behavior is untested.
- Container: the Docker image built and `docker compose config --quiet` passed.
  This step did **not** start the Compose service or run a real model in it.

The [presentation update](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari/pull/1)
and [audio-recovery fix](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari/pull/2)
are merged. The [media inventory](media/README.md) records the exact official
Hermes logo with its MIT notice, independent-project graphics, and the approved
owner-recorded physical puck video with unchanged audio and a real frame.
That footage does not identify the installed firmware/backend revision and is
not acceptance of this public source. Direct Hermes integration, cross-device
handoff and visible agent-action approval remain [planned](HERMES-INTEGRATION.md).

Before promoting this source preview, the next software acceptance is a dedicated
clean-machine installation that starts the reviewed container and exercises
real STT/chat/TTS, login/logout, reset, cancellation/recovery and rollback. Record
its exact source/image/model revisions and sanitized evidence. The historical
real-model result below does not validate this newer revision. Physical phone
microphone, puck, battery/charging and elapsed-soak gates remain separate.

## Owner-directed publication setup — 2026-11-04

The owner selected [hermes-voice-puck-zentari](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari),
directed Apache-2.0 publication of Zentari-owned source including the extracted
gateway, and designated **info@zentarisystems.io** as the monitored private
security/conduct contact. Vendor/dependency licenses remain separate. That initial
distribution was limited to source and the scripted static demo; the later approved
media additions are recorded above. No firmware/container binary, model weight or
standalone reference portrait asset is included.

The private vulnerability form and issue-routing configuration are supplied.
GitHub's reporting feature is activated and checked separately when the new
repository becomes public. Actual email/notification receipt remains unverified
until an end-to-end check is observed; see [the maintainer procedure](MAINTAINER.md).

November 4 engineering checks include 46 Linux public tests, 76 gateway tests,
42 browser unit tests, 22 native suites and an ESP-IDF OTA-mode build. The two
new compiled regressions cover OTA startup priority and timed-out display DMA
lifetime. The connected Puck's accepted firmware passed owner voice checks;
these newer source fixes are not flashed. A static AI baseline review found no
reportable vulnerabilities with explicit generated-pixel exclusions. It does not
replace independent human, legal, binary, hardware or elapsed-soak acceptance.

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
security or legal approval. This historical result does not establish hardware,
unfamiliar-machine/operator, reporting-receipt or third-party binary acceptance.
Current owner source-license and contact decisions are recorded above.

## Executed software evidence (historical checks plus November 8 hosted run)

| Check | Evidence tier and result |
| --- | --- |
| Source/private-content scan | Static: exported tree passes fixed private-host/path/key checks |
| Gitleaks | Static: no leaks with one exact published synthetic JWT plus its exact vector path allowlisted; no fixture directory excluded |
| Dependencies | Linux CPython 3.13 amd64: 34 exact wheel hashes resolved; pinned image builds; pip-audit reports no known Python dependency vulnerabilities |
| Portable firmware core | Native GCC: 22/22 suites pass, including regenerated public RGB565/RLE artwork |
| Gateway | Linux: 76 tests pass, including protocol rejection, context/reset/isolation, cancellation and subprocess lifecycle |
| Browser unit coverage | November 8 hosted Node: 47 tests pass, including auth, late audio, permission races, bounded queues, cache failure and unavailable/interrupted audio recovery |
| Accessibility | Automated axe WCAG 2/2.1/2.2 A/AA checks pass for phone/desktop PWA states, sign-in dialog and the static demo; actual assistive-technology review remains open |
| Full PWA | Synthetic Chromium and Firefox: sign-in, text/context, cancellation, microphone fixtures, reconnect, reduced motion, avatar persistence, offline shell, logout and 320px layouts pass |
| Static demo | Synthetic Chromium, Firefox and WebKit: scripted interaction, cancellation/reset, keyboard, narrow layouts and no external requests/storage pass |
| Standalone setup/TLS | Linux integration: fresh private setup, verified real HTTPS/WSS, cookie login, local HTTP chat fixture, bounded PCM output and logout pass; inference is synthetic |
| Firmware builds | ESP-IDF 6.1: default and M3a voice-mode builds pass with the procedural public assets; no flash or physical acceptance |
| Compose/container | November 8 hosted run: Compose configuration parses and Docker image builds; service startup and real-model execution were not part of this run. Configuration retains loopback-only ports, non-root UID, read-only mounts/root, dropped capabilities and resource limits |

Hosted GitHub CI at the revision above does not imply phone microphone, screen
reader, new physical firmware, hardware charging, signed update, firmware rollback
or soak acceptance.
Python vulnerability results exclude OS/firmware/model analysis. SBOM inventory
and checksums do not constitute legal clearance, artifact signatures or an
independent security audit. Workflow and packaging tools require final-tree checks.

## The original 1.0 gates, with the actual remaining acceptance

| Gate | Engineering progress | Still required for the full gate |
| --- | --- | --- |
| OSS-01 baseline | Prior voice phase accepted; new public art/core tested | Exact new public firmware/web/model/board physical acceptance |
| OSS-02 product/brand | Owner selected the preview repository/name; local-first browser/reference boundary documented | Stable product/brand and trademark review |
| OSS-03 code/vendor rights | Owner directed Apache-2.0 for Zentari-owned gateway; root/vendor notices and package inventory retained | Third-party dependency/binary obligations and independent rights review |
| OSS-04 artwork/models | Procedural client art retained; exact official Hermes logo has pinned MIT provenance; owner-approved video/frame inventoried; model weights omitted | Review rights and notices for the exact final distribution; do not infer rights to other portrait files |
| OSS-05 standalone gateway | Extracted gateway plus local OpenAI-compatible adapter; real TLS fixture round trip | Independent replication of real local STT/chat/TTS acceptance |
| OSS-06 clean export | Explicit source/overlay allowlist; private provenance outside export; scans | Independent exact final tree and new-history review |
| OSS-07 protocol | Existing schemas/vectors, limits, compatible-client and negative tests retained | Final published compatibility review |
| OSS-08 installation | Demo, private setup, doctor, hardened Compose and synthetic TLS integration | Unfamiliar clean-machine real-model install, measured time |
| OSS-09 accessibility | Automated keyboard/reduced-motion/layout coverage | Actual screen reader/mobile keyboard/contrast review |
| OSS-10 security | Threat model and bounded adapter errors/egress | Independent auth/privacy/update review and findings closure |
| OSS-11 hardening | Origin/CSRF/logout/session tests, expiry, bounds, CSP/cache | Measured adversarial load, full independent review |
| OSS-12 privacy | Device/principal context, reset/expiry tests, RAM-only documented | Independent retention and identity-remap review |
| OSS-13 reporting | Owner-designated monitored contact, private form, issue routing and triage procedure | Live feature check and observed end-to-end receipt/notifications |
| OSS-14 software/models | Native, Linux gateway, locked image, two full browser engines | Actual phone audio, full WebKit microphone path, independent model replication |
| OSS-15 hardware/performance | Reference board/provenance and C tests retained | New physical baseline and p50/p95/resource measurements |
| OSS-16 charging | Experimental boundary documented | Attended charge/discharge/low-battery/thermal measurements |
| OSS-17 signed approval | Source/tests retained; default mode disabled | Signature, expiry, replay, denial and physical confirmation |
| OSS-18 OTA recovery | Source/policy tests retained; default mode disabled | Wrong/corrupt/untrusted/interrupted image, failed-boot rollback and attended recovery |
| OSS-19 soak | Required procedure documented | Frozen candidate 24-hour then 72-hour physical soak |
| OSS-20 community/docs | Install, architecture, support, conduct, contribution and maintainer docs written | Two unfamiliar reviewers and tested real contact channels |
| OSS-21 release automation | Action/image digest pins, wheel hashes, SBOM and read-only hosted software CI passed at the revision linked above; Pages workflow is manual | Firmware matrix and OS/license/scanner coverage; rerun required checks for each new candidate |
| OSS-22 artifacts/recovery | Exact source manifest and deterministic packaging tooling | Second-builder/signature review and independent install/rollback replication |
| OSS-23 beta | Explicit experimental scope and known limitations | Consented external browser/hardware beta and blocker closure |
| OSS-24 public authorization | Owner selected the new repository and directed experimental Apache-2.0 source/demo publication | Stable release needs its own final manifest, acceptance and authorization |
| OSS-25 maintenance | Governance/recovery requirements documented | Named second release-capable maintainer and accepted triage/support duties |

These gates are not marked passed because a checklist or harness exists.
Battery, recovery, independent review, owner decisions and elapsed soak cannot
be completed by source editing. They remain required before a stable 1.0 claim.
