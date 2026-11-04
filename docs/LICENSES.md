# Distribution inventory

The existing root Apache-2.0 LICENSE/NOTICE and original source copyright/SPDX
headers are retained. Do not assume this overrides a component's own license.

| Component | Source/license | Distribution decision |
| --- | --- | --- |
| Existing Puck/PWA code | Root Apache-2.0 and per-file notices | Retain notices and license |
| Extracted gateway | Owner-controlled source, no root license in source repository | Owner authorization required before public distribution |
| New public tooling, demo and geometric art | Original code-generated assets, Apache-2.0 with this project | No reference portrait, downloaded font, model, or third-party image used |
| Waveshare reference values/init data | Pinned upstream Apache-2.0; third_party/waveshare/LICENSE | Retain NOTICE, provenance and source headers |
| ESP-IDF/component-manager dependencies | dependencies.lock; upstream per-component licenses | Source build downloads separately; firmware binary distribution review pending |
| Python runtime | Python Software Foundation license; pinned official image | Preserve runtime notices in any distributed image |
| Python dependencies | Exact Linux wheel lock and SBOM.cdx.json | Review full wheel license files and transitive obligations before binary/image distribution |
| Piper runtime | Upstream Piper licensing must be reviewed, including GPL obligations | Do not treat root Apache license as covering this runtime or ship images without review |
| Chat/STT/TTS models | Separately acquired; individual model-card licenses | No weights bundled; operator verifies use/redistribution and checksums |
| Original portraits | Redistribution rights not established | Omitted, including derived RGB565/RLE data; new procedural assets replace them |

The SBOM describes resolved Python packages, not all OS packages, firmware
components or model terms. It is an inventory, not a legal opinion. Add OS and
firmware SBOMs before distributing those binary artifacts. Source availability
alone does not establish rights to an image, voice, model or trademark.
