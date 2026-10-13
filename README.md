# samosa-sources 0.1.0

Sources is a component for an LLM app that turns raw content into information
the app can read, search and cite.

## Input and output

**Input:** Files, uploads, URLs, text, images, audio and video, plus a request
telling Sources what to do: save, read, index, search or analyze the content.
The host app supplies authorized folders, private storage and any required
credentials or optional tools.

**Output:** Saved copies of the content and JSON results containing the requested
text, transcript, visual description or search matches. Results include IDs,
versions and references to the original file, page, byte range or timestamp,
so the app can trace an answer back to its source. Generated analysis also
reports its partial or uncertain coverage.

Sources keeps saved versions when the original changes. Optional Inference
integration supplies model work such as transcription, summaries and visual
descriptions; these require configured model services. Readers, web fetching
and Chutni search also require their declared optional dependencies.
See [dependencies](DEPENDENCIES.md).

## How to use Sources

Build from this directory with a C11 compiler and make:

```sh
make
```

This example saves a text file, then reads the saved version. It needs no
optional libraries or model service:

```sh
demo=$(mktemp -d)
mkdir "$demo/input" "$demo/state"
chmod 700 "$demo/state"
printf '%s' 'Hello from Sources.' > "$demo/input/notes.txt"

registration=$(printf '%s' \
  '{"contract_version":1,"op":"register_text","path":"notes.txt"}' |
  build/samosa-sources --root "$demo/input" --state "$demo/state")
printf '%s\n' "$registration"

source_id=$(printf '%s\n' "$registration" |
  sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')

printf '{"contract_version":1,"op":"read_text","source_id":"%s","version":"%s","start_byte":0,"end_byte":19}' \
  "$source_id" "$source_id" |
  build/samosa-sources --state "$demo/state"
```

The first response identifies the saved content. For text, its source ID and
version are both the content's SHA256. The second response contains
`Hello from Sources.`, an evidence ID, the selected byte range and provenance.
Changing `notes.txt` afterward does not change that saved version.

Each CLI invocation takes one JSON request on stdin and returns one JSON result
on stdout. `--root` allows registration from that folder; `--state` holds private
saved content and must remain the same across calls. File paths in requests are
relative to the root. `end_byte` is exclusive.

To see the available operations and limits:

```sh
printf '%s' '{"contract_version":1,"op":"describe"}' | build/samosa-sources
```

For other workflows, use the [operation contracts](contracts/): register the
content, then read or analyze a selected range, or index it and search the index.
An app can also call the native library or use the
[authenticated HTTP service](contracts/service-v1.md).

## Build and technical reference

Independent native C component. `make test` builds the executable and runs C and
shell acceptance checks. `make sanitize` runs AddressSanitizer/UBSan; `make release`
packages this component's source/build/test/contracts. See DEPENDENCIES.md.

`--serve --serve-auth-file /private/auth.json --state /private/source-state`
starts the authenticated loopback HTTP adapter (default port8643;
`--serve-port 0` reports an assigned port). Host credentials map requests to
owners; JSON and single-part streaming uploads use the existing Source contracts.
Bounded workers/queues preserve owner FIFO/round-robin progress and a separate
control path. Native64MiB socket upload, overload/control, shutdown/restart and
normal/sanitizer gates pass on macOS/Linux. Service-owned background queues,
shared admission leases, active provider cleanup and scope replacement also pass.
Sources areas1–7 are qualified for this standalone component release.
See [service contract](contracts/service-v1.md); `make test-service
sanitize-service`, with `SAMOSA_SERVICE_FULL_UPLOAD=1` for the64MiB gate.

The default build includes pinned SQLite source and uses descriptor-bound state
stores. No SQLite runtime installation or network download is needed. Existing
state database names/schema are preserved; pathname-reopen store paths are removed.
The release includes qualified descriptor-bound storage; see the release acceptance record.

Each invocation accepts one JSON body <=16 KiB. Describe needs no state/root.
Text is UTF-8 without embedded NUL. Later live changes require explicit new
registration; identical bytes deduplicate.

Bounded text/PDF/HTML/DOCX/inventory/index/locator subsets are implemented.
Image/PDF OCR and bounded audio/visual preparation are qualified on macOS/Linux.
Original snapshots now accept up to64MiB; see docs/S19a.md for boundary evidence.
Media preparation and upload/freshness/watch lifecycle qualification pass macOS/
Linux. Sources areas1–7 are qualified for the standalone 0.1.0 release.
See [release acceptance](docs/release-acceptance.md) for evidence and platform scope.
Assembled product gates remain open.
Existing production implementation remains
until its replacement gates pass. Runtime, builds and publication acceptance
tooling are native; no Python runtime is required. See
[publication acceptance](docs/publication-native.md).

S18a media work adds register_audio/prepare_audio for bounded PCM16 mono16000Hz
WAV windows, returning base64 prepared media with absolute original time ranges.
Results are derived artifacts, not transcripts or literal evidence. See
contracts/audio-v1.md and docs/S18a.md for the implemented subset and remaining
compressed audio/video, artifact cache and neural extraction work.

The library now supports `transcribe_audio` through a trusted host-provided public
Inference callback. It validates English Whisper results, maps cue times to the
original source timeline and caches owner-scoped derived transcripts. Native
fixture and sanitizer checks pass on macOS/Linux (`make test-transcript
sanitize-transcript`). The supplied process adapter and CLI configuration call an
independent public Inference executable; actual Whisper WAV/MP3 capture and offline
cache reuse pass on macOS and native ARM64 Linux, including sanitizers. Actual
66-second WAV/MP3 qualification also covers overlapping initial/middle/final
windows, exact PCM bindings, publication and offline retained readback.
See contracts/transcript-v1.md and `make test-inference-process`.

Library hosts may set `SourcesInferenceProcess.temporal_frames=1` for the native
temporal adapter's prepared-frame DTO: requests are bounded to24MiB, transport
metadata to48MiB, and supervised execution to120seconds. The default process
profile retains its2MiB request/60second limits. Response bounds and scoped
cancellation/requester-death cleanup remain shared. The temporal CLI
configuration selects this transport profile.

`analyze_video` samples up to16 validated native source frames, includes the
final decoded frame, verifies returned timestamp/pixel/runtime/model/package
bindings, and retains owner-bound uncertain observations for offline cache and
tracked replay. Actual macOS model integration covers initial and late ranges;
native lifecycle and 16-frame adapter fixtures pass on macOS/Linux. See the
architecture verification document for actual-model and release acceptance.
See [temporal contract](contracts/temporal-v1.md).

`analyze_visual` now combines validated native frame preparation with public
Inference and an owner/model/recipe-bound capture cache. It preserves the actual
source timestamp, selection range, pixel hashes and decoder/model provenance;
coverage explicitly describes one selected frame and uncertain model output.
Audio and visual providers have separate host configurations using the same
transport/control interface. See [visual contract](contracts/visual-v1.md).
Native frame/callback tests and the actual macOS VisionPsy heading pipeline are
qualified. Public publish_visual now uses the existing Chutni artifact writer
and receipt recovery, retaining a caption with an exact sampled timestamp and
complete uncertain observation provenance; native SDK gates pass on both
platforms. Durable visual jobs now reuse the audio queue engine and global
eight-job/8MiB capture budget, with transient frames, separate publication
steps, retries and runtime shutdown. Expanded queue runtime sanitizer gates pass on macOS and Linux after
correcting retryable SQLite schema contention. Automatic visual discovery and
reindexing now preserve previous derived jobs in retained history; public
index_visual_history/read/search expose bounded, validated retained captions.
Native normal/sanitizer integration gates pass on both platforms, including
transaction rollback and killed replacement recovery. Enrichment and semantic
retrieval remain required work. Prepared-vector memory_search_semantic now uses
the existing public Chutni cosine search, pinned host profile/store identity and
ordinary owner/control/offline capture paths; native normal/sanitizer SDK gates
pass on both platforms. Natural-language `memory_search_text` now obtains a
validated vector through owned public Inference before SDK ranking. See the
[embedding contract](contracts/embedding-v1.md) for host configuration and replay.
Automatic vector publication remains open. See also the
[semantic contract](contracts/semantic-v1.md).

`publish_transcript` writes an owner-authorized cached transcript window through
the existing Chutni SDK, preserving model provenance, uncertainty and source-time
cues. It uses host publication grants, stable SDK recovery and durable local
receipts; it does not rerun Inference. See contracts/chutni-v1.md and
`make test-transcript-publication sanitize-transcript-publication
CHUTNI_LIBRARY=<qualified SDK>`. Actual Whisper WAV/MP3 capture-to-publication and
complete capture readback pass on both platforms, including sanitizers. Audio
discovery/freshness and broader speech/media qualification remain open.

The [inspect_audio](contracts/audio-timeline-v1.md) operation reports the usable
whole-millisecond timeline from owned WAV/media snapshots, without retaining
full-file PCM or adding prepared cache entries. Encoded inspection uses the
existing worker decoder and resampler instead of trusting container padding.

Selected-range audio jobs now checkpoint capture and publication separately,
retry publication without rerunning the model, and run through the existing
`--index-run` scheduler and indexing lease. Public operations are
`audio_index_create/get/step/queue_step/read`; see
[audio indexing](contracts/audio-index-v1.md). Actual-SDK fixtures qualify WAV
queue progression, worker failure/backoff, TERM shutdown and SDK-commit/SIGKILL
recovery. Actual Whisper WAV/MP3 cold-state queue capture, publication, completion
and offline saved-capture readback now pass on both platforms, including sanitizers,
for the short English fixture. Broader media and lifecycle qualification remain open.

S18b adds optional native compressed audio and `prepare_visual` timed RGBA frames
from immutable `register_media` snapshots. Both use a host-configured media worker,
verified artifact fingerprints, owner-scoped shared prepared caches and durable
operation controls. Visual outputs carry source timestamps, stream, source/output
dimensions and color conversion metadata; they remain derived preparation with
partial coverage. See contracts/media-v1.md for bounds and supported semantics.
PQ/HLG native transfer/tone mapping and orthogonal orientation are qualified on
macOS/Linux, including sanitizers. Native seeking preserves late video selections;
the final second of a four-hour compact source passes through the public API/cache
on both platforms normally and under sanitizers. Encoded audio uses native late
seeking with discarded preroll and source-grid alignment; full inspection keeps
no waveform. Admitted video/PCM/MP3/AAC reference tests cover public preparation,
cache and durable replay. The rebuild checkpoint records current archive and
qualification evidence. Neural extraction remains a separate public Inference
work area. Their Sources integration, lifecycle, acquisition, indexing and
service acceptance is recorded in docs/release-acceptance.md.

S03 optional PDF text-layer reading: register_pdf then read_pdf with explicit
start_page/end_page, at most four inclusive 1-based pages. Host configures
--pdfium-library /installed/libpdfium. Plain text works with it absent. Native
checks: `make test-pdf PDFIUM_LIBRARY=/installed/libpdfium`; the fixture generator,
forwarding spy and tests use native C. Coverage is always partial text_layer_only.

S17a optional image OCR: `register_image` then `read_image` with exclusive byte
ranges in recognized UTF-8. `read_pdf_ocr` uses inclusive absolute page ranges
over registered PDFs, rendering bounded150dpi grayscale pages. Configure
--tesseract-library, --leptonica-library, --ocr-traineddata and optional
--ocr-language through trusted host arguments, plus --pdfium-library for PDF OCR.
These paths must resolve to actual regular artifact files, not symlinks.
Coverage always stays partial/ocr_recognition_uncertain. See the image/PDF OCR
contracts and docs/native-worker-v1.md for owned process teardown. Independent
reader/resource acceptance passes on both platforms. CLI uses its own
worker; library hosts must configure SourcesConfig.native_helper for new native
reads. Old qualified cache hits need no running helper.

S07a evidence identity also binds extractor fingerprint. Text publishes SHA-256
of its owned versioned strict UTF-8 byte-reader contract. PDF publishes SHA-256
of the configured regular PDFium artifact (<=128MiB, 64KiB streaming buffer), with
file identity/timestamps checked before and after hashing/loading/extraction.
Configured library paths must be regular files, not symlinks/FIFOs. Host reader
configuration is trusted; fingerprint covers the configured artifact, not every
transitive operating-system library or arbitrary forwarding adapter dependency.

S07b Sources owns persistent qualified evidence reuse, keyed by exact immutable
version/range/reader/fingerprint. Cache responses are byte-identical; partial PDF
text stays partial. Hits verify pinned source bytes, reader fingerprint and cached
response digest/identity before return and skip page/text extraction. Failed
extractions never enter cache. Capacity128 entries/8MiB response bytes,2MiB/entry;
no eviction. Count capacity is checked before extraction; exact byte admission is
transactional after bounded extraction. Library host may lower cache_limit and
cache_byte_limit. At capacity old hits remain readable; new entries receive typed
cache_limit. Corruption receives corrupt_cache. Clients see no cache paths.

S09a `inventory` takes a relative directory `path`; empty selects configured root.
It requires no state directory/model/action executor. Results have endpoint-scoped
host_id/scope_identity, relative locators, mechanical metadata identity, type/size
and uninspected content. Configure --host-id when addressing multiple hosts.
Default local-endpoint identifies this configured endpoint only, not a global host.
Limits256 returned entries/512 inspected names/32 directories/depth4/path4096/
512KiB output. It never follows symlinks; special files are ineligible. Bounds or
invalid UTF-8 names yield partial inventory/unknown total. Directory changes during
capture refuse without publishing entries. Metadata identity is not a content
hash; content_version stays null until actual registration/reading qualifies it.

S09b resumable text index: index_create captures directory inventory; index_step
commits <=4 items; index_get reports durable progress; index_search returns <=10
qualified literal candidates. Configure --owner for separate principals. Search
and exact completed retries work with root unavailable, from captured evidence.
One active index worker, zero queued. Worker death preserves per-item checkpoints;
new operation resumes pending work. Jobs8/items256/operations512/text+evidence8MiB,
combined SQLite page cap160MiB. See contracts/index-v1.md for coverage and failure
semantics; partial results never establish current filesystem absence.

S11c1 --owner also binds immutable source/media read authority and private cache
reuse. Another owner's known content SHA or warmed extraction does not authorize
reading or expose their locator. Registering identical bytes under a permitted
root retains that owner's first provenance without copying immutable payloads.
Max256 owner/id/media claims, no eviction. Old unclaimed prerelease snapshots
remain stored and require explicit re-registration to establish a claim. Public
content/evidence identities remain stable; private cache keys include owner.

S11c2 locator_reconcile observes <=8 relative locations against an owned immutable
version and optional historical physical tuple. It independently checks actual
content/inode and reports available/missing/changed/unsupported/uncertain at a
recorded checkpoint. Locator IDs bind owner, host/root scope and relative path;
new root inode creates a new scope. locator_get and exact retries return committed
history without filesystem access. No claim of continuous freshness or filesystem-
wide absence; immutable evidence is retained. Capacity256 associations/256 retry
records, zero queued observers, no eviction. See contracts/locators-v1.md.
# Correlated read budgets

read_text/read_pdf/inventory accept optional operation and parent references plus
a relative timeout_ms budget. Qualified replies echo references; expired and
cancelled work return typed failures and preserve prior snapshots/cache. Host
callbacks remain scoped. `make test-control sanitize-control` qualifies read
correlation, authority and delayed inventory. `make test-pdf sanitize-pdf
PDFIUM_LIBRARY=...` also proves that delayed real extraction is not cached and a
healthy read survives. Native image/PDF calls execute in owned helpers for scoped
teardown during initialization, decoding and rendering. Durable operation_get/
operation_cancel, saved retries, owner control and recovery are implemented;
see the operation contracts and docs/native-worker-v1.md for remaining gates.

Continuous registered watches use the same executable with --watch-id ID and
--watch-interval-ms1000 alongside --root/--state/--owner/--host-id grants.
See contracts/watches-v1.md for leases, checkpoint freshness and shutdown.

Continuous reindexing uses --index-run and --index-interval-ms1000 with the same
root/state/owner/host grants. It drains retained observation-driven intent through
index_queue_step, preserves immutable versions and uses one private runtime lease.
Run the watch and index modes in separate owned processes. TERM/INT stop; SIGKILL
allows restart. The interval paces polling; durable per-item backoff doubles from
one second to a60-second cap while ready peers continue. Changed targets wake
work; explicit reindex can retry immediately. Full service qualification remains
open. See contracts/index-v1.md, docs/S19s.md and docs/S19t.md.

To discover and index files added to existing jobs automatically, run
`--index-run --index-interval-ms 1000 --discovery-interval-ms 1000` with the same
host root/state/owner grants. Each tick attempts at most one directory scan and
one pending initial item, one queued failed-item recovery and one indexed-locator
observation before reindexing. Persistent
per-job cooldowns and turns preserve bounded progress across restarts without
growing client receipt history. Discovery is disabled when its host interval is
zero. Indexed files receive periodic bounded hash observations, so changed/missing/
restored files feed the retained reindex queue without a separate watch. Supported
failed/skipped text files can recover after a later scan; broader recovery
qualification and full freshness/service policy remain open. See contracts/index-v1.md.

Host HTTP acquisition now has a bounded C entry point, sources_fetch, and an
optional isolated libcurl worker built with make fetch-worker. Grant one exact
fetch_origin and select fetch_helper in SourcesConfig; validated bytes use
existing immutable admission and URL provenance. Acquisition response limit5MiB,
headers64KiB, no redirects. Optional checks: make test-fetch sanitize-fetch.
Host --fetch-public-only validates every DNS answer using the extracted monolith
address policy, pins libcurl and checks the connected peer. --fetch-offline denies
new network work while allowing accepted acquisition/operation receipts to replay.
Native policy checks: make test-fetch-public sanitize-fetch-public; optional
build/test-fetch-public WORKER --live exercises actual public HTTPS. Bracketed
IPv6 literals are supported; explicit local grants also accept HTTP [::1].
Host --fetch-public-web now adds bounded redirects, final-URL receipts, robots
gating and parent-owned per-host politeness; final actual normal/sanitizer gates
pass on macOS and Linux. All six search preset mappings have native request/reply
fixtures: make test-search-presets sanitize-search-presets. These fixtures do not
assert paid live-provider access.
See [acquisition contract](contracts/fetch-v1.md) for exact-origin/public-web
policies and limits, and [search contract](contracts/search-v1.md) for provider
qualification scope.

Public op=fetch now accepts acquisition_id, url and format, with optional existing
operation_id/parent_operation_id/timeout_ms controls. CLI host options
--fetch-origin and --fetch-helper grant the adapter. Immutable admission and
acquisition receipts commit together; a new execution attempt can recover the
same accepted acquisition offline after an interrupted process. Receipts retain
URL/format and validate snapshot integrity and ownership. No full release claim.

HTTPS now supports host-captured PEM trust (SourcesConfig.fetch_ca_pem, max8KiB)
and CLI --fetch-ca-file without disabling peer/hostname verification. The worker
uses only copied trust bytes, never the host file path. Optional qualification:
make test-fetch-tls sanitize-fetch-tls (OpenSSL3 fixture SDK). Valid/invalid trust,
hostname/expiry and CLI file checks pass on macOS/Linux, along with a real public
HTTPS capture followed by root-free offline HTML evidence. Broader TLS and search
provider qualification remains open.

Sources now exposes `op=search` independently of the gateway. Host declarations
reuse the existing six W1 provider presets and generic JSON templates; the public
request supplies a query and optional operation controls. The real Parallel
provider has returned bounded results on macOS/Linux. `make test-search
sanitize-search` checks actual POST/JSON/SSE worker responses, private CLI
configuration, redaction, owner admission and offline replay/read on both
platforms. Paid provider presets remain declarations awaiting qualification.
See [search contract](contracts/search-v1.md) for configuration and limits.

`op=memory_search` delegates lexical retrieval to an existing host-granted Chutni
store through its public SDK. Build `make chutni-worker` and select
--chutni-library/--chutni-store/--chutni-helper. Results retain external references
and explicit metadata coverage, then use existing owned snapshots and offline
replay/read. Indexed text can also be published to the existing store with the
host's --chutni-store-id UUID --chutni-publish grant, using index_publish_step
or the automatic --index-run phase. Durable intents recover lost SDK
acknowledgments without duplicating artifacts. See
[Chutni contract](contracts/chutni-v1.md) for limits and qualification;
memory_read now returns text ranges and full provenance for referenced artifacts,
including later ranges, with owned offline captures. Non-text artifact retrieval,
enrichment and semantic retrieval remain pending.


Indexing and Chutni publication also reuse the existing HTML, DOCX and PDF text
readers, preserving original snapshots, extracted-text coordinates, absolute
pages and partial coverage. The same path captures later reindexed versions.
Run test-publication/sanitize-publication with both CHUTNI_LIBRARY and
PDFIUM_LIBRARY to exercise actual reader-to-artifact integration. See
[index contract](contracts/index-v1.md) for bounds. Image OCR indexing/publication
also reuses the existing configured OCR reader, preserving recognition uncertainty
and confidence. Add OCR_FIXTURE, TESSERACT_LIBRARY, LEPTONICA_LIBRARY and
OCR_TRAINEDDATA to those test targets to exercise the English raster integration.
Current Sources area closure and remaining integration gates are recorded in
[the release acceptance record](docs/release-acceptance.md).

[Retained evidence summaries](contracts/summary-v1.md) reuse the monolith's native
T5 engine through public Inference. Scalar and opening map/reduce captures retain
owned evidence, bounded sampling and model/reader provenance. Cached opening
summaries can be published through the actual Chutni SDK as `summary_short`.

[Automatic index enrichment](contracts/enrichment-v1.md) discovers retained index
generations and model profiles through `index_enrich_queue_step`. Capture and
publication consume separate durable phases, so publication retries reuse the
saved summary. `index_enrich_step` selects one index job;
`index_enrichment_status` exposes retained summary/receipt history and retries.
These operations reuse index admission and the derived SDK publisher.
[Automatic embedding](contracts/embedding-v1.md) captures vectors and publishes
them through the public Chutni SDK in separate durable phases. The native
background queue and text-query search are qualified with a pinned actual model
on macOS and Linux. Samosa does not ship a Python runtime.
