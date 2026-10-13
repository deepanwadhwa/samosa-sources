# Sources 0.1.0 release acceptance

Status: qualified standalone source release, 2026-10-12.

The release is the standalone Sources component: source, native build and test
tooling, public contracts, dependency manifests and preserved licenses. Its host
supplies authorized roots, private state, credentials and optional public SDK,
reader, decoder and Inference artifacts. Replacement App release is a separate
architecture milestone.

This standalone repository was extracted from the Sources component of
`deepanwadhwa/samosa-chat`. The seven qualified areas and their platform/model
scope are recorded below. Build and runtime require no sibling repository.
The detailed original development record is retained in that project as
`docs/ARCHITECTURE_REBUILD_VERIFICATION.md`.
Final qualification includes the following independent archive acceptance.

| Requirement | Required evidence | Current archive status |
| --- | --- | --- |
| Independent core, uploads, freshness/watch, queues and service | Extracted macOS and ARM64 Linux `make test sanitize`, terminal success; explicit 64MiB socket gates | Passed on both platforms, including socket64 |
| Acquisition and search | Extracted native HTTP/TLS/public fetch and search/all-provider mapping normal/SAN targets | Passed on both platforms |
| Readers, OCR and public Chutni | Extracted publication/readback, Chutni, transcript/visual publication, semantic, summary, enrichment and embedding normal/SAN targets using actual public SDK/readers | Passed on both platforms |
| Media preparation and reindex | Extracted native codec/color/orientation/late-range/audio corpora, visual capture/queue/index normal/SAN targets | Passed on both platforms |
| Actual semantic model | Twelve document/query rankings, automatic capture/publication, CLI query and exact offline replay on both platforms, normal/SAN | Passed on both platforms |
| Full-duration speech | Three overlapping windows of actual 66-second speech, exact PCM/source coordinates, all SDK publications and model/origin-free readback using extracted Sources executables | WAV normal and encoded SAN passed on both platforms |
| Temporal video | Actual native Molmo2 16-frame input/pixel/time/model bindings and retained offline replay, normal/SAN on macOS; Linux adapter/lifecycle qualification | Passed: macOS actual model normal/SAN at the unchanged 120-second budget; Linux native adapters/lifecycle |
| Package identity and dependencies | Exact file contents, complete native tooling/contracts/licenses, no Python/AppleDouble/build state; declared optional dependencies and minimal binary linkage | Passed: qualified runtime/build/test identity, complete native source set and minimal linkage |
| Final deliverable | Final source archive and SHA256 manifest match qualified implementation; release notes identify platform availability and replacement scope | Source archive and external SHA256 manifest in dist/ |

Independent builds may use public dependency executables/libraries explicitly
configured by the host. They must not compile sibling private implementation or
read sibling private state. Sanitizers instrument Sources, its owned helpers and
native clients; external SDKs/model engines retain their declared qualification.
Actual macOS MLX temporal execution is not claimed on Linux. Paid search-provider
availability is distinct from native request/reply mapping acceptance.

Replacement retirement within Sources covers removed pathname-based SQLite
reopen paths and the former Python publication driver. The production gateway
continues serving the existing application until its assembled replacement gates
pass; deleting that application is not part of independent Sources acceptance.

Failed development runs are excluded. A live process or partial PASS line is
insufficient: each required suite must terminate successfully. Candidate archive
identity alone does not establish qualification or release completion.

The final archive uses the qualified candidate runtime/vendor files and compilation rules and
the separately qualified OCR enrichment fixture lifetime fix. A block-local OCR
configuration previously outlived its scope during background queue assertions;
the test now retains it for the full fixture lifetime. Actual reader enrichment
passes normally and under ASan/UBSan on both platforms. No production behavior,
assertion or deadline was relaxed. The packaging rule excludes generated
bytecode caches and macOS metadata; no Python source or runtime is shipped.

Both platforms pass independent core and optional dependency acceptance. Actual
semantic acceptance checks twelve documents and twelve paraphrase queries
(normal/SAN: 468 checks each); actual full-duration speech checks three overlapping
windows. Actual macOS Molmo2 acceptance checks sixteen 512px frames, independent
RGBA/P6/timestamps, ordered observations and model/origin-free replay
(normal/SAN: 4,194,476 checks each, including pixel writes). Linux temporal
acceptance qualifies native adapters and controls with fixture models.
Independent summary protocol/publication acceptance is distinct from the earlier
actual macOS T5 component qualification; actual Linux T5 execution is not claimed.

Failed concurrent development runs remain excluded. Dedicated sequential reruns
passed unchanged visual inspection, strict document-call counts and temporal
deadlines. This acceptance covers the declared bounded contracts and supported
formats; generated speech, OCR, summaries and visual observations retain their
partial/uncertain coverage.

To verify the delivered archive, run from dist/:

    shasum -a 256 -c samosa-sources-0.1.0.tar.gz.sha256
