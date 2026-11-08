# Experimental reference hardware

The **puck** is the experimental physical companion, separate from the
[installable browser PWA](INSTALL.md#install-the-browser-pwa). The reference
target is **Waveshare ESP32-S3-Touch-LCD-1.85C BOX Rev2**, with a circular
360×360 display. Start with the [parts and materials overview](PUCK-MATERIALS.md)
for what is integrated, what you need separately, and package differences.

Use the exact revision, pin mapping and source provenance in
[`hardware/pinmap.json`](../hardware/pinmap.json) and
[`third_party/waveshare/`](../third_party/waveshare/UPSTREAM.md).
The BOX name alone does not establish the revision. ESP-IDF 6.1, the checked-in
component lock and reviewed board configuration are required. Do not substitute
a similarly named board without independently checking voltage, pins and buses.

Build with an installed ESP-IDF 6.1 environment:

```bash
idf.py -B build/firmware -D SDKCONFIG=build/firmware/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.defaults set-target esp32s3 build
```

This builds the default mode; it neither enrolls nor flashes a device. Voice mode
requires separately reviewed local TLS/enrollment configuration. Do not put Wi-Fi
passwords, identity keys, signing keys or enrollment files into defaults or Git.
The public preview does not provide an accepted factory provisioning flow.

Approval and OTA modes remain experimental and disabled in the default mode.
The OTA acceptance mode takes precedence over its required WSS mode and runs
the physical display/touch challenge before calling the update path. Compiled
startup tests cover that dispatch and both touch/update failure paths; they do
not execute an update. Hardware boot confirmation, measured eFuse anti-rollback
state and an attended restore drill remain unaccepted.
Do not burn eFuses, change secure boot, overwrite a factory dump or flash an
accepted device as part of trying the web demo. Keep a verified full backup and
the known-good exact image before any separately scoped flash/recovery exercise.

The new procedural artwork changes firmware bytes. Earlier physical acceptance
does not validate this new image. Remaining attended acceptance includes:

1. Record exact board, power source, firmware SHA-256, microphone/speaker path.
2. Verify cold boot, hold-to-talk, audible reply, Stop at each phase, mute, menu,
   volume persistence, missing Wi-Fi, TLS failure, restart and session renewal.
3. Measure speech-to-first-audio/cancel latency with p50/p95 and sample count.
4. Measure charging/discharging, low battery and temperature against board limits.
5. Exercise signed approval rejection, expiry, replay and physical confirmation.
6. Exercise corrupt/wrong-board/untrusted/interrupted updates, failed boot,
   rollback and attended recovery with the actual release artifacts.
7. Freeze the candidate for 24-hour then 72-hour soak and retain all failures.

Host tests, synthetic browser audio and a firmware build cannot close these gates.
