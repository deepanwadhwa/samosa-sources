# Durable selected-range audio indexing, contract1

The host grants a Sources root/state/owner and configures public Inference provider,
engine/model SHA256 pins and transport. Register immutable audio/media first.
Jobs refer to that owned source/version and an explicit source-time range. They
reuse the monolith's overlapping-window accounting, existing Sources preparation
and public Inference projection, and the existing Chutni SDK writer. There is no
replacement decoder, model engine, catalog or background scheduler.

```json
{"contract_version":1,"op":"audio_index_create","source_id":"<sha256>","version":"<same sha256>","start_ms":0,"end_ms":233000}
{"contract_version":1,"op":"audio_index_step","job_id":"<job sha256>"}
{"contract_version":1,"op":"audio_index_queue_step"}
{"contract_version":1,"op":"audio_index_get","job_id":"<job sha256>"}
{"contract_version":1,"op":"audio_index_read","job_id":"<job sha256>","ordinal":0}
```

All operations accept existing optional operation_id, parent_operation_id and
timeout_ms controls. Requests cannot select roots, paths, workers, model identity,
SDK stores or write permissions. Unknown fields refuse. Job identity binds owner,
root scope, immutable source, selected range, provider/engine/model and decoder
recipe, and the publication store UUID (or capture-only mode). Repeated creation
returns the existing job without resetting progress. Other owners get
unknown_audio_index. Changed provider/engine/model/decoder configuration returns
audio_index_configuration_changed; a replaced root returns scope_unavailable.

Ranges are integer source milliseconds with0<=start<end<=14400000. Windows are at
most30000ms, with1000ms overlap. Ceiling stride retains the final overlapped tail,
including exact multiples of the window length. A four-hour selection has at most
497 windows. WAV ranges are checked against retained PCM frames at creation and
work time. Encoded ranges are admitted against the configured decoder identity;
actual stream availability/duration is checked by preparation at work time.
The separate [inspect_audio](audio-timeline-v1.md) operation now measures usable
owned audio duration. Initial filesystem indexing now uses this measurement to
admit the full retained audio timeline. Persistent duration reuse remains open.

Internal discovery/reindex admission can now reuse creation within an existing
index transaction and held index lease. Its row joins the observation commit or
rollback; it preserves the same owner/scope/source/range/model/decoder/store
identity, limits and existing retry state. This internal entry does not add a
public request field or run the model. Initial discovery registers the inventory
identity through the public reader, measures the owned snapshot, and commits its
typed audio-job relation and cursor with queue admission. It reports
`queued_audio_items` and `waiting_audio` after enumeration, without inserting
literal text. Index get exposes linked public audio job identities/progress and
counts completed derived items only after the existing queue validates all saved
captures and required publication receipts. Locator watches and bounded index
refresh now create audio reindex intents. Manual/background index reindex
registers and measures the observed version, then atomically admits its job,
retains the previous linkage and clears the matching revision intent. Commit
rechecks live revision/identity/hash/root; failures preserve the previous job.
Native WAV tests exercise interrupted admission/commit, observation/root races,
cancellation/deadline and actual job/history capacity boundaries. Derived
index history/read now expose validated current and superseded derived captures
through `index_audio_history` and `index_audio_read`, preserving the same public
artifact identity, source-time selectors and model/input provenance.
`index_audio_search` scans those validated retained captures with bounded derived
substring candidates, exact read selectors and explicit uncaptured-window coverage.
It needs no model transport; semantic retrieval remains open.
Compressed discovery/reindex has a nine-case native
normal/sanitizer matrix on both platforms, covering measured31–32s timelines,
two overlapping windows, original-absent capture and retained changed versions.
These model-fixture checks qualify integration, not recognition accuracy.

A capture step validates the owner snapshot and recipe, prepares one window in
bounded working buffers, validates public Inference output and atomically stores
its complete derived capture plus SHA256 and cursor. It may reuse qualified public
cache entries; it never adds or evicts prepared PCM or transcript cache entries.
A publication step consumes that saved capture and writes through the existing
SDK transaction/recovery path, then stores its validated receipt independently.
Publication retry never calls the model. A lost local acknowledgment after SDK
commit recovers the same artifact, including with original file and model transport
absent. Original snapshot ownership/integrity and configured identities still apply.

With the host Chutni publication grant enabled at creation, the job binds the
expected store UUID and requires publication of each capture before advancing to
the next window. Upload/acquisition locators cannot use this file-backed writer.
Without that grant the job retains captures locally and has no publication phase;
publish mode/target is immutable, so changing it requires a distinct job.

step/queue_step returns processed_windows:0 or1. Successful work reports job_id,
ordinal and phase captured/published. Zero work reports completed_selected_windows,
retry_backoff or no_due_audio_windows. get reports total/captured/published counts,
publication_required, selected range, retry timestamp, failures and last_error.
Completed means all selected windows have committed required phases; coverage
remains partial/selected_audio_windows and does not assert recognition accuracy,
complete original-file indexing or coverage outside that range. read returns the
stored derived capture with original source-time cues, uncertainty and model
provenance; it adds no literal evidence_id. Saved captures/receipts are deeply
validated, including recomputed-digest structural/timing corruption refusal.

Limits:8 retained jobs,497 windows per job,8MiB aggregate capture bytes. Each capture
is at most256KiB and each receipt at most8192 bytes. Shared existing Chutni/index
publication receipts also retain their4096-entry bound. At capacity, new admission
fails without evicting prior data. Job removal/history reclamation remains pending.
Failed work records its typed error and bounded retry backoff through60000ms;
explicit steps and queue scheduling honor that checkpoint. Provider busy, cancellation
and deadline remain typed. Parent death releases the shared index.lock admission
lease; committed capture/publication phases survive.

The existing --index-run loop services at most one audio phase per cycle when an
Inference configuration is present, using the same queue runtime lease, signal
cancellation and pacing. Audio attempts have a60s maximum; earlier host/caller
budgets win. Text refresh/reindex and literal publication retain their5s attempts.
Fatal store/protocol/corruption errors stop the runtime; known dependency failures
remain pending under job backoff. New audio jobs may be created explicitly or
admitted by the automatic index discovery/reindex integration described above.

Native actual-SDK fixtures cover eight overlapping WAV windows under a1KiB
persistent-cache admission budget, public durable phases and readback, owner/model
configuration refusal, worker failure/backoff/retry with model transport absent,
recomputed-digest corruption, existing runtime progression/TERM shutdown, and
SIGKILL after SDK commit before local acknowledgment followed by recovery without
the original file/model. These longer-window queue tests use labeled fixture model
output. A separate native actual-Whisper CLI fixture now qualifies a2672ms English
WAV and its MP3 encoding through cold-state queue capture, separate publication,
completion and byte-identical saved-capture readback after original removal with
model transport and SDK worker absent. Normal and ASan/UBSan runs pass on macOS
and native ARM64 Linux. Full-duration recognition, broader freshness/discovery and
job/history lifecycle qualification remain open. No complete Sources release is
claimed by this contract subset.
