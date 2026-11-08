# Distribution inventory

The existing root Apache-2.0 LICENSE/NOTICE and original source copyright/SPDX
headers are retained. Do not assume this overrides a component's own license.

| Component | Source/license | Distribution decision |
| --- | --- | --- |
| Existing Puck/PWA code | Root Apache-2.0 and per-file notices | Retain notices and license |
| Extracted Zentari-owned gateway | Apache-2.0 under this repository's root LICENSE/NOTICE | Owner directed Apache-2.0 source publication on 2026-11-04; retain notices |
| New public tooling, demo and geometric art | Original code-generated assets, Apache-2.0 with this project | No reference portrait, downloaded font, model, or third-party image used |
| Waveshare reference values/init data | Pinned upstream Apache-2.0; third_party/waveshare/LICENSE | Retain NOTICE, provenance and source headers |
| ESP-IDF/component-manager dependencies | dependencies.lock; upstream per-component licenses | Source build downloads separately; firmware binary distribution review pending |
| Python runtime | Python Software Foundation license; pinned official image | Preserve runtime notices in any distributed image |
| Python dependencies | Exact Linux wheel lock and SBOM.cdx.json | Review full wheel license files and transitive obligations before binary/image distribution |
| Piper runtime | Upstream Piper licensing must be reviewed, including GPL obligations | Do not treat root Apache license as covering this runtime or ship images without review |
| Chat/STT/TTS models | Separately acquired; individual model-card licenses | No weights bundled; operator verifies use/redistribution and checksums |
| Earlier standalone reference portraits | Redistribution rights not established for those exact files | Omitted as standalone assets, including their derived RGB565/RLE data; procedural assets replace them |
| Official Hermes / Nous Girl white SVG and its presentation derivatives | NousResearch/hermes-agent at pinned revision; MIT, copyright 2025 Nous Research | Exact logo and full MIT notice included; attribution and independent-project labeling retained; see media provenance |
| Owner-supplied hardware demo video and extracted still | Supplied by the owner with express permission for this repository presentation | Depicts an older installed puck, including its displayed portrait; not proof of current-public-source acceptance or a blanket license to separately reuse the portrayed artwork |

The SBOM describes resolved Python packages, not all OS packages, firmware
components or model terms. It is an inventory, not a legal opinion. Add OS and
firmware SBOMs before distributing those binary artifacts. Source availability
alone does not establish rights to an image, voice, model or trademark.

The owner's source-license direction covers Zentari-owned Puck, browser and
extracted gateway code. It does not relicense third-party dependencies, voices,
models or vendor material. Those retain their own terms. This preview distributes
source and a scripted static demo; it does not distribute a container image,
firmware binary or model weights. A source-license decision is not a legal audit.

When redistributing Apache-2.0 source, include the license, retain applicable
copyright and attribution notices and NOTICE material, and identify modified
files. Commercial use and modification are permitted; the license does not
require recipients to publish their own source changes. Trademark rights are
separate. The full [license text](../LICENSE) controls.
