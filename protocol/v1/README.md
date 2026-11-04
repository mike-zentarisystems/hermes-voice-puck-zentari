# M3a voice event extension to `zentari.voice.v1`

The M2 `device.hello`, `auth.challenge`, `auth.prove`, and `session.accepted`
messages are unchanged. Only a validated `session.accepted` enables voice
frames. Every subsequent event carries `v`, `type`, `device_id`, `session_id`,
`correlation_id`, and a sequence increasing by exactly one per direction. The
server binds device/session to the accepted connection; neither peer accepts a
stale correlation ID. [Schema](schemas/voice.turn.schema.json) and
[vectors](test-vectors/voice-turn.json) describe the fields.

One half-duplex turn: `audio.input.start`, one or more `audio.input.chunk`,
`audio.input.stop`, `transcript.final`, `assistant.thinking`, `assistant.text`,
`audio.output.start`, one or more `audio.output.chunk`, `audio.output.stop`.
`cancel` and categorized `error` end a turn. Audio format on both sides is
signed 16-bit little-endian PCM, mono, 16 kHz. Each JSON/base64 chunk decodes
to 1–1024 bytes, with an even decoded byte count. Whole WebSocket messages
are at most 4096 bytes. Input is at most 15 seconds/480000 bytes; output is
at most 60 seconds/1920000 bytes. Each stage has 30 seconds except speaking,
which streams up to 60 seconds; the whole turn 150 seconds. (Until 2026-10-15:
30 seconds/960000 bytes of output and a 120-second turn.) Text is at most 4096 UTF-8 bytes. The server holds one bounded
input buffer and one worker; the puck streams capture and has at most eight
queued output messages. Both sides reject malformed, stale, extra, or
out-of-order data. The schema describes the JSON shape; dynamic identity,
sequence, byte counts, time budgets and exact field sets are enforced by code.

Destructive actions require an `approval.request` whose canonical fields are
signed by a configured gateway approval key. The puck checks key id, signature,
action digest, nonce replay, issue time, maximum 90-second TTL, and expiry before
displaying the exact action. Approval needs a fresh 1.5-second hold after a
400-ms arming delay; denial and timeout fail closed.

## Device commands

`device.command` lets the gateway invoke the puck instead of only speaking to it.
The trust tier is a property of the command name, fixed in an allowlist on the
device; the sender's `mutates` hint is diagnostic only and is never used for
enforcement. `read` and `push` need only the authenticated session. Anything that
changes local device state is `mutating` and requires the signed, single-use
approval above plus the hold gesture, with `risk:"mutating"` — a device command may
never claim `risk:"destructive"`, which remains reserved for Hermes-originated work.

Exactly one mutating command may be held at a time. A second is rejected as `busy`
rather than queued, so a stale command cannot fire minutes after the operator has
moved on. Rejections always answer with `device.command.result`, whose
`reason_code` is a bounded enum that never echoes arguments, summary, or digest.

A local touch is not a remote command: the hold constrains a remote or
unauthenticated actor, and a finger on the panel is the operator's own presence.

### Device command digest and physical confirmation

A device-command approval's action must equal its command. Its action_digest is
SHA-256 of sorted-key, compact UTF-8 JSON with this exact shape:

```json
{"action":"device.volume.set","args":{"level":40},"command_id":"5a97be68-cfbe-4772-84e7-0ea48c1a0fc1"}
```

Level is an integer 0..100; muted is a JSON boolean. Other allowlisted commands
currently accept no arguments (omitted args and {} normalize to {}). Duplicate
keys, fractional numbers, token prefixes and unknown arguments are rejected.
Configure the trusted digest_verify adapter on the command context; absence
rejects remote mutations. The firmware adapter is zv_hermes_command_digest_verify.

ingest() retains one verified mutating command and returns ZV_OK with result_len
zero. This is pending work, not an applied result. The UI displays that retained
snapshot and feeds fresh physical input to confirm(). Arming is 400 ms and an
uninterrupted hold is 1500 ms. Completion rechecks session state and approval
expiry, applies only the snapshot, emits its result and clears it exactly once.
Denial, disconnection and expiry cannot apply an effect. READ/PUSH replies remain
immediate and do not cancel pending work. Local touchscreen effects still use
apply_local() directly. No network dispatcher is enabled by these changes.
The bottom-left microphone icon is push-to-talk: hold for 200 ms to start,
keep pressing while speaking, then release to send. Tap the microphone again
while THINKING or SPEAKING to cancel. The visible bottom-right menu control
works in every voice state. Opening it during a turn cancels audio first;
after dismissal the link reconnects and authenticates afresh so old queued
frames cannot enter another turn. Microphone and menu use the same circular
touch predicates as the reference painter.
