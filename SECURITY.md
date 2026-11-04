# Security

This is a preview candidate. It has not received an independent security audit
and has no stable supported release. Authentication, updates and physical safety
must not be described as certified or production-ready.

## Report privately

Do not put vulnerabilities, exploits, credentials or private user data into public
issues. Use this repository's **Security → Report a vulnerability** only after
maintainers enable private vulnerability reporting. The release checklist requires
an end-to-end test of that channel and its notifications before public launch.
That test is still pending; no monitored inbox or response-time SLA is claimed.
If the private-report action is unavailable, keep sensitive details private until
the repository owner provides a working private channel.

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
