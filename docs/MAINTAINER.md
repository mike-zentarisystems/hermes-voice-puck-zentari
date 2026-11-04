# Preview maintenance and private reporting

Repository: [hermes-voice-puck-zentari](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari).
Initial maintainer: the `mike-zentarisystems` repository owner.
Private security and conduct contact: [info@zentarisystems.io](mailto:info@zentarisystems.io).
The owner selected this monitored address on November 4, 2026. No additional
mailbox, forwarding rule, signing key or production credential is created by
this repository.

## Reporting channels

- Security: prefer [GitHub private reporting](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari/security/advisories/new),
  or email the designated contact if that form is unavailable.
- Conduct: email the designated contact; never use a public issue.
- Ordinary non-sensitive defects: use the repository's bug-report form.

GitHub private vulnerability reporting is available for public repositories.
An administrator enables it under Security settings after source-preview
publication, then checks the setting with the API and the report form with a
signed-in account. The exact reporting URL must remain bound to this repository.

## Configuration and receipt checks

An administrator can inspect GitHub's setting without exposing credentials:

```sh
gh api repos/mike-zentarisystems/hermes-voice-puck-zentari/private-vulnerability-reporting
```

An enabled response is `{"enabled":true}`. An authentication redirect from an
anonymous browser is expected for submitting a report; it is not proof of email
delivery or maintainer notification receipt.

For an end-to-end delivery check, send a benign email with subject
**Hermes Voice Puck reporting-channel check** to the designated inbox, and have
the inbox owner confirm receipt. A different GitHub account can submit a clearly
marked non-vulnerability channel test; the owner verifies the private receipt
and notification, then closes the test privately. Do not create a public advisory,
invent a vulnerability, or include secrets. No email or advisory is sent by the
setup scripts. Retain timestamp, channel and receipt result privately.

Configuration checks do not establish delivery. Until receipt is observed,
record notification delivery as unverified. Do not promise an acknowledgement
deadline or count this as stable-release security acceptance.

## Triage

The owner reviews private reports, acknowledges when available, and reproduces
issues in a disposable test installation. Keep real transcripts, audio, identity
keys and production settings out of reports and public logs. Track affected
versions, coordinate remediation and disclose after a tested update exists.
Security-sensitive changes need review independent of their author.

## Release boundary and recovery

The selected license is Apache-2.0 for Zentari-owned source; retain all applicable
vendor/dependency terms and notices. Publish source and the scripted demo as an
experimental preview. Firmware, container binaries and model weights need their
own distribution and acceptance review.

Before replacing a source package, verify its manifest and scanner results and
retain the previous archive/checksum. Before a runtime update, retain its exact
image and matching private configuration. Before a firmware update, retain the
accepted device image and component-specific recovery record. Publishing a new
source preview never authorizes overwriting a working Puck, changing eFuses or
reconfiguring production services. Charging, signed approval, firmware recovery,
human/phone accessibility and physical soak remain stable-release gates.
