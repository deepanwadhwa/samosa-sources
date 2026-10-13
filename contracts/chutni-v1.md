# Chutni retrieval and indexed-text publication, version 1

Sources delegates lexical and semantic ranking to the existing external Chutni SDK. This
adapter migrates the monolith's storage boundary; it implements no alternate
Chutni catalog, FTS index or ranking algorithm. The retrieval worker uses only
the documented chutni_open, chutni_call, chutni_free and chutni_close public ABI.
It dynamically loads a host-selected shared library and opens a host-granted
existing store read-only for retrieval. Publication separately requires the host write grant described below. No registry discovery or store creation occurs.

Build `make chutni-worker`. Host configuration supplies absolute paths through
SourcesConfig.chutni_library/chutni_store/chutni_helper or CLI options
--chutni-library, --chutni-store and --chutni-helper. The host must grant the
selected store for the selected owner; possession of a public request never
grants a store. Public callers cannot select library, executable or store paths.
The core remains buildable without the SDK or its headers.

```json
{"contract_version":1,"op":"memory_search","query":"Coriander","operation_id":"memory-1","timeout_ms":10000}
```

Only query and the existing operation controls are accepted. Query is nonempty
UTF-8, at most 512 bytes. Chutni search uses its documented query syntax, limit8
and include_stale=false. Sources does not reinterpret BM25 scores as semantic
similarity. No fallback searches run on failures. Prepared-vector semantic retrieval uses
[semantic-v1.md](semantic-v1.md) with a fixed host profile and a distinct score type.

The worker queries store_info for the public store identity and search for the
results. Returned items retain external_source_id, external_artifact_id,
artifact_kind, snippet, freshness, score and score_type. Public results contain
no private store path. Each snippet is at most 4096 UTF-8 bytes; over-limit or
malformed replies are refused. The parent independently verifies query identity,
UUIDs, exact envelope/field shapes, eight-result limit, current freshness,
finite scores and the bm25_fts5_negated score type. Normalized private responses
are bounded to 64 KiB; raw SDK JSON parsing is bounded to 1 MiB/8192 tokens.
SDK work runs in the existing isolated Sources execution protocol with scoped
cancellation, deadlines, CPU/file limits, RSS monitoring and process teardown.

Success includes the ordinary immutable source envelope, evidence_id and a
memory object with provider, capture time, partial/retrieval_metadata coverage,
and the result. Admission uses Sources' existing owner claims, capacity checks
and one transaction with final interruption validation. Saved metadata can be
read with read_text offline; an identical completed operation_id also replays
offline. There is no independent acquisition receipt for interrupted searches.

Store UUID plus artifact/source UUIDs are external Chutni references. They are
distinct from Sources' SHA-256 snapshot identities. Snippets may come from
extracted text, summaries or standalone memory, so retrieval metadata is not
promoted to literal source evidence or exhaustive filesystem coverage. Chutni's
current flag describes its catalog knowledge at capture; Sources does not claim
that every underlying live file was revalidated during this operation.

Optional checks: `make test-chutni sanitize-chutni CHUTNI_LIBRARY=/absolute/sdk`.
They create a real external store through the public SDK, record 12 artifacts,
retrieve eight, verify store counts stay unchanged, and exercise parent refusal,
private SDK errors, deadline expiry, ownership and offline replay/read using the
actual CLI and worker. SQLite/Chutni dependencies are separate SDK artifacts;
owned Sources code and executed processes receive ASan/UBSan instrumentation.

## Artifact content and provenance

```json
{"contract_version":1,"op":"memory_read","store_id":"<store UUID>","external_source_id":"<source UUID>","external_artifact_id":"<artifact UUID>","start_byte":0,"end_byte":65536,"operation_id":"read-artifact-1","timeout_ms":10000}
```

The three external references are required; omitted byte offsets default to
0/65536. Offsets are integers through64MiB, ordered, with a window at most64KiB.
The host-granted store is opened read-only and its UUID must match the reference.
The SDK's source_context selects the artifact belonging to that source and reads
the requested text range with its existing content/provenance logic. This requires
the SDK's artifact_id/start_byte/end_byte extension documented in its API-JSON.md;
older replies without range metadata fail closed. Sources incorporates no SDK
private schema or headers. SDK builds remain independently selectable artifacts.

The normalized response includes store/source/artifact references, artifact kind,
origin, status, freshness, source content hash, selector, producer and derivation
provenance, semantic_validation=not_performed, text, range and coverage. The range
reports start_byte/end_byte/total_bytes. Offsets beyond EOF clamp to EOF; a start
inside a UTF-8 character is refused, and a split final character is excluded.
Later ranges work beyond the SDK's ordinary context prefix limit. Only a range
covering the complete artifact receives complete coverage. Freshness records the
SDK observation; retained stale/superseded artifacts remain readable, with their
status exposed. Content is not relabeled as literal original-file evidence.

The parent independently verifies external references, exact normalized shape,
UTF-8 text, byte count/range/coverage, BLAKE3 hash shape, status, provenance object
shape and the semantic-validation flag. Normalized replies are at most256KiB;
SDK parsing remains bounded to1MiB/8192 tokens. Observed content/provenance uses
Sources-owned immutable metadata snapshots and ordinary owner claims, controls,
completed-operation offline replay and read_text. Captures are read observations,
not a second authoritative Chutni catalog. Store paths are omitted; producer
parameters and inputs are retained as provenance and may identify source inputs.

Text-backed artifacts up to64MiB are supported within worker and response limits;
non-text object payloads and oversized provenance are refused. Full binary-object
retrieval, larger stores and broader replacement/concurrency qualification remain
pending. Actual SDK checks include search-to-read, published-text provenance,
360KB UTF-8 later ranges, split-character refusal, wrong store/artifact references,
ownership, unchanged store counts and offline replay/read.

## Indexed text publication

The host additionally configures `--chutni-store-id UUID --chutni-publish`
(SourcesConfig.chutni_store_id/chutni_publish). Public requests cannot grant
writes, change the target store or select paths. The worker verifies the expected
store UUID before opening for writes and again after opening. Publication also
uses the existing public typed SDK hash_bytes/hash_file/root_add/source_find
functions. It reuses the SDK's `put_file_outputs` transaction and deduplication;
Sources does not implement a replacement catalog or artifact writer. This
requires a current SDK build that provides that operation, beyond its version
label alone.

```json
{"contract_version":1,"op":"index_publish_step","job_id":"<index id>","operation_id":"publish-1","timeout_ms":10000}
{"contract_version":1,"op":"index_publish_queue_step","operation_id":"publish-next"}
{"contract_version":1,"op":"index_publication_status","job_id":"<index id>","after":0}
```

Each step selects at most one owned, root-scoped indexed text version. It
validates the retained evidence digest and immutable snapshot SHA-256, then the
worker proves the literal byte range against that snapshot. Chutni's public
hash function supplies its distinct BLAKE3 identity. Before new publication,
actual live file bytes must match; missing catalog entries are registered and
changed entries refreshed through the SDK. Existing applicable Chutni root
policies are retained; otherwise the host-granted Sources root is registered.

A pending intent commits in Sources before SDK work. Its publication identity
binds owner, root scope, store UUID, source version and evidence digest, so
identical evidence across jobs reuses the same Chutni artifact. SDK outputs
include deterministic parser provenance, the original Sources evidence envelope,
byte selector and version/evidence references. Before accepting either a new or
recovered artifact, the worker checks these fields and its actual untruncated
text. Conflicting content or duplicate matching operations fail closed.

Recovery looks for the retained artifact before refreshing the live catalog;
thus an SDK commit followed by a lost Sources acknowledgment can be recovered
even if the live file changes again. Sources subsequently commits a response
and its digest. A successful response reports published_items=1,
receipt_state=committed, job/ordinal/generation and a publication object with
store/source/artifact identities, publication/version/evidence identities,
Chutni content hash, artifact status and recovered flag. Completed controlled
operations replay offline. This is recoverable publication across two stores,
not a shared atomic transaction.

Pending retries preserve last_error and rotate by persisted scheduling turn.
Initial reservation cooldown is 1 second; failed attempts back off from 2 to
60 seconds. Up to 4096 publication checkpoints are retained without eviction.
Status requires the owning job and pages 16 retained entries; it works offline
and while an indexing writer is running. Empty queue responses explicitly say
no_due_publications, not that every file has been indexed or published.

With the host publication grant, --index-run adds one bounded publication phase
after discovery/freshness/reindex work. It uses its own 5-second phase budget,
honors earlier host deadlines and stops with the runtime's existing signals.
SDK errors remain visible in durable checkpoints and retry independently.

Publication covers indexed plain text, HTML readable text, DOCX readable text
bounded PDF text and image OCR, from original snapshots up to64MiB and indexed text up to
64KiB. The worker reuses the existing readers to verify converted text; PDF
publication requires the host's configured PDFium library. HTML/DOCX selectors
identify extracted_utf8 coordinates; PDF selectors identify absolute pages.
Complete original reader evidence remains in Sources parameters. The parent
requires the corresponding owned media claim rather than treating every snapshot
as plain text. Partial opening/page coverage remains explicit. Worker SDK
JSON remains bounded to 1 MiB/8192 tokens and at most 512 context artifacts;
large histories may refuse rather than claim unverified success. Broader store
replacement races, capacity/concurrency qualification, PDF OCR/audio/video artifacts,
non-text artifact retrieval, enrichment and semantic retrieval remain release work.

Image OCR publication uses the host's existing Tesseract/Leptonica/model adapter
inside the isolated worker, preserving extracted_utf8 selectors and explicit
recognition uncertainty, confidence, raster dimensions and extractor fingerprint.
Recovery compares the complete Sources evidence structure against saved SDK
parameters, including recognition metadata, before accepting an existing artifact.
The fixture's deliberate SDK catalog mutation is test-only; production Sources
continues to use the public SDK for catalog access.

`make test-publication sanitize-publication CHUTNI_LIBRARY=/absolute/sdk`
exercises the actual SDK/CLI, registration, refresh, retrieval, grant/UUID
refusals, offline checkpoints and replay, corruption of artifact text with
intact provenance, post-SDK SIGKILL recovery without duplicates and automatic
publication of bounded UTF-8 text. Fixture catalog mutation is test-only;
production accesses Chutni exclusively through its public SDK.

Sources work area4 and the complete service/release gates remain open.

The private publication worker now also accepts validated transcript captures.
This adapts the monolith's chutni_store_model_text output construction onto the
same existing put_file_outputs transaction and recovery path. It writes transcript
artifacts with model_generated origin, provider/model-revision identity, complete
Sources capture parameters and source_time_ms time_range selectors. Recognition
uncertainty, cue timestamps, prepared-input identities and timing policy remain
in the retained capture. It does not rerun or semantically verify the model.

Capture shape and identity, original snapshot SHA256, capture digest and selected
ranges are validated before writing. Existing-artifact recovery compares the
complete saved capture, text, model identity and selector. Conflicting captures
refuse rather than overwrite an already committed artifact. Transcript receipts
use artifact_id; literal publication continues to use evidence_id. Native tests
exercise the real SDK and both worker/parent validation in normal and sanitizer
modes on macOS/Linux, using clearly labeled fixture model output.

Sources exposes `publish_transcript` with `source_id`, `version`, `start_ms` and
`end_ms`, plus the existing optional `operation_id`, `parent_operation_id` and
`timeout_ms` controls. It selects an owner-authorized cached transcript; it never
invokes Inference. Call `transcribe_audio` first. Missing capture returns
`inference_unavailable`. Provider/engine/model configuration must match the saved
capture. The host must grant Chutni publication and the registered source's root;
the source claim must have a relative file locator. Upload-only snapshots have no
file publication locator and return `publication_locator_unavailable`.

The response contains `published_items:1`, `receipt_state:"committed"` and the
worker's `publication` receipt. The stable SDK operation key binds owner, root
scope, store UUID, immutable source and complete capture digest. Sources saves a
digest-checked receipt under that key and revalidates the owner, snapshot, prepared
audio and capture before bare receipt replay. The SDK recovers a committed artifact
if the local receipt is missing, including after original file removal. Durable
operation replay preserves the original envelope. Local transcript receipts share
the4096-receipt publication bound with index receipts. Committed receipts are
historical; use memory_read for current catalog status and artifact contents.

Selected-range durable audio jobs and their existing scheduler integration are
implemented; see [audio indexing](audio-index-v1.md). Automatic filesystem audio
discovery/reindex/freshness remains pending. The existing public
memory_read SDK adapter preserves resulting model provenance. Actual Whisper
capture-to-publication/readback now passes for the qualified English WAV and MP3
fixtures on macOS and native ARM64 Linux, in normal and ASan/UBSan modes. The
native public-CLI fixture checks the complete retained capture against readback,
exact artifact identity/text and original/provider/worker-absent receipt replay.
It also qualifies cold-state actual-model audio queue capture, separate SDK
publication, completion and offline saved-capture readback for those short fixtures.
Broader speech quality, full-duration audio indexing and upload/acquisition
publication remain open.

Visual caption publication uses the same bounded derived-artifact writer and
SDK receipt/recovery path. See [visual-v1.md](visual-v1.md) for publish_visual,
exact sampled-time selectors and retained uncertainty/provenance. A caption is
kept distinct from transcript and literal extracted-text artifacts.
