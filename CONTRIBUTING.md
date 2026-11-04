# Contributing

Start with the README's hardware-free demo and host tests. Open a focused issue
with expected/observed behavior, source revision, OS/browser, reproduction and
sanitized fixed error categories. Never attach credentials, raw conversation
data, private hostnames, flash dumps or production settings.

Keep each pull request focused. Include its user-visible outcome, tests actually
run and limitations. Changes to authentication, enrollment, context ownership,
updates, signing or privacy require an independent maintainer review. Tests that
use synthetic media must say so. Do not claim physical acceptance from a build.

Contributions are under the existing Apache-2.0 source license unless an individual
file has a different preserved notice. Sign commits with `git commit -s` to certify
the [Developer Certificate of Origin 1.1](https://developercertificate.org/).
Do not contribute proprietary code, unlicensed artwork or model weights. Keep
copyright and vendor notices intact. No contributor license assignment is required.

Useful first tasks: reproduce mobile keyboard behavior on a real phone; review
keyboard and screen-reader flows; test fresh Linux onboarding with measured elapsed
time; add a failing protocol vector for a confirmed parser defect. State browser,
board and evidence tier for each result. Maintainers should triage these tasks
before adding public good-first-issue labels.
