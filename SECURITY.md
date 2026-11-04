# Security

This is an experimental preview. Automated tests and a static AI source review
have run; there has been no independent human security audit or stable supported
release. Authentication, updates and physical safety
must not be described as certified or production-ready.

## Report privately

Email [info@zentarisystems.io](mailto:info@zentarisystems.io) for private security
reports. This is the repository owner's designated monitored contact.
When available, use [GitHub's private vulnerability report form](https://github.com/mike-zentarisystems/hermes-voice-puck-zentari/security/advisories/new).
The form is private to the reporter and repository security maintainers; a
public issue is not a private reporting channel.

Include the source revision, affected component, impact and safe reproduction
steps. Omit live credentials, raw audio, personal transcripts and private device
dumps. Use the subject **Hermes Voice Puck security report** for email.

If the GitHub form is unavailable, use the email contact. No guaranteed response
time is promised. Channel configuration and notification delivery are separate
checks; the [maintainer procedure](docs/MAINTAINER.md) records how to verify them.

Maintainers acknowledge privately when available, reproduce in an isolated fixture,
coordinate a patch and advisory, and disclose after a remediation path exists.
Track affected versions and credit the reporter with consent. Auth, enrollment,
privacy and OTA fixes require independent review and release verification.

## User controls

Keep the stack loopback-only unless a separate HTTPS/network review is completed.
Trust only the verified local public certificate. Keep `.runtime`, passwords,
provider keys, models and backups outside Git. Revoke compromised device
generations in the registry and stop/recreate the stack to close every session.
Preserve sanitized evidence privately. Disable an affected service before
investigating; do not expose a debugging server or disable TLS verification.

Read [the threat model](docs/THREAT-MODEL.md) for the implemented boundaries and
remaining independent, hardware and operating-channel gates.
