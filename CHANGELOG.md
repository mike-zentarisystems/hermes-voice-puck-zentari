# Changelog

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
