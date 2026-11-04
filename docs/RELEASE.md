# Build, verify and release

Public source and the scripted static demo are distinct from a stable hardware
product release. Only publish a new history created from the reviewed export;
never change visibility of the operational repositories or copy their `.git`.

Run the README checks and record toolchain/OS/browser and exact source revision.
Regenerate art and compare it byte-for-byte. Resolve Python dependencies in the
pinned Linux amd64 CPython 3.13 builder with `pip install --dry-run
--ignore-installed --report`, then run `tools/lock_dependencies.py` against that
report. The resulting lock is platform-specific and includes wheel hashes.
Re-resolving a dependency changes the candidate and requires regression checks.

Run `python3 tools/check_release.py --write-manifest` after all changes, then
`--verify-manifest` on the exact final tree. It reports filenames/categories,
never matched secret values. Also run an independent secret scanner against
the final source and new Git history. Review findings manually; allowlisting and
pattern scans cannot prove the absence of every credential or personal record.
The source manifest excludes local output, models and runtime credentials.

The Pages workflow is manually dispatched and deploys only `dist/site`, built
from `site/` and four generated images. It cannot publish the authenticated
client, server configuration, gateway, model weights or private receipts.
Enable Pages and review the target environment in the new public repository.
Do not claim a deployed website until the anonymous public URL is checked.

Before a stable tag: close the ledger, finish independent security/license and
artifact reviews, verify private reporting and maintainer recovery, execute
real-model/hardware/rollback/soak acceptance, and obtain owner authorization
for the exact manifest. Do not replace those gates with this documentation.

For an image release retain its immutable digest, complete OS/runtime/license
SBOM, provenance, signatures and instructions for verification. For firmware,
retain exact board/mode, toolchain, SHA-256, notices and rollback image. Build
twice in clean builders before calling anything reproducible; source hashes
alone do not establish reproducible firmware or containers.

No release workflow in this candidate has access to signing keys, deployment
secrets or production routes. Untrusted pull requests run read-only checks.
