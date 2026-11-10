# Changelog

## 0.1.0-preview plain-English docs and setup fixes — 2026-11-10

- Add `eli10.md`, a plain-English overview of what the project is, what works
  today and how to try it.
- Rewrite the README around a short quick start: x86-64 requirement, making
  Ollama reachable from the container without exposing it to the network, and
  example speech-model downloads.
- `tools/setup.py` now prints the reason when setup fails and removes a
  half-created configuration directory, so setup can be run again.
- Rename the project to Hermes Voice Agent in docs, report subjects, the
  browser app title and install name, and the overview image; replace private
  infrastructure names in comments, tests and the demo with neutral wording.
- Fix two missing-file references in NOTICE and three help links in the
  install guide.

## 0.1.0-preview reporting setup — 2026-11-04

- Record Apache-2.0 publication direction for Zentari-owned extracted gateway
  source while preserving separate third-party licenses and notices.
- Designate info@zentarisystems.io for private security/conduct reports, add a
  private vulnerability form and route sensitive reports away from public issues.
- Document administrator checks, private triage, delivery verification and the
  experimental source/demo publication boundary. Delivery receipt remains a
  separate observed check; no message is sent by these setup changes.

## 0.1.0-preview startup review — 2026-11-04

- Select the experimental OTA acceptance path before its required WSS mode.
  Previously the combined build ran only the connection test. The physical
  display/touch gate still runs before OTA, and failures remain gated.
- Add compiled startup regressions for OTA, WSS, voice and default modes.
  This repair does not establish signed update, rollback or recovery acceptance.
- Preserve the display's in-flight fill buffer on a completion timeout and
  latch the existing display fault. Further writes require restart, preventing
  the allocator from reusing storage while the display may still read it.

## 0.1.0-preview review follow-up — 2026-10-29

- Resolve speech subprocesses relative to their installed module, fixing real
  STT/TTS startup in the standalone image.
- Bound the FFmpeg resampling graph to one thread; repeated 22.05 kHz voice
  conversion no longer exhausts the container's task budget before cyclic GC.
- Require the CTranslate2 vocabulary in speech model readiness and setup checks.
- Issue a server-only TLS leaf and discard the temporary authority signing key.
  Fresh setup now trusts `ca.pem`; retain matched configuration/image backups
  when upgrading from the earlier preview.
- Reject nested environment-file variants from release archives and preserve
  existing checksum evidence. Add a transcript-free real-model acceptance tool.

## 0.1.0-preview — unreleased

- Standalone authenticated browser/gateway package with a local OpenAI-compatible
  chat adapter and bounded local speech workers.
- Credential-free static scripted demo suitable for GitHub Pages subdirectories.
- Original procedural Zenti/Orbit artwork; reference portraits and weights omitted.
- Clean export tooling, secret/private-content checks, exact dependency and source
  manifests, contribution and security documentation.
- Experimental ESP32-S3 source retained with vendor provenance. New public artwork
  and packaging have not inherited the private installation's physical acceptance.

This is not a stable release. See the explicit open gates in docs/RELEASE-STATUS.md.
