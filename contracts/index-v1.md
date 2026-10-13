# Sources index contract v1

All operations accept contract_version1 and use the host-configured owner, root,
state and endpoint. Clients use public envelopes and never open the index database.

- `index_create`: relative directory `path`, `operation_id` (1..128 ASCII identity
  characters). Captures bounded inventory and pending items. Returns durable job
  and operation/result envelopes. No content reading during creation.
- `index_step`: `job_id`, unique `operation_id`, `max_items`1..4. Captures only
  pending items in manifest order; atomically commits each item and cursor.
- `index_get`: `job_id`. Returns cursor, indexed/failed counts, inventory/content
  coverage and captured_snapshot freshness. Audio links additionally return
  `audio_jobs` (ordinal plus the public audio job status), `queued_audio_items`
  and `completed_audio_items`. They reuse audio_index_get validation of retained
  snapshots, model/decoder identity, captures and publication receipts. The audio
  root/dependency configuration must remain valid; no original file read or
  model call is needed. Corrupt retained audio refuses the status response.
- `index_audio_history`: `job_id`, optional nonnegative `after` cursor (default0).
  Returns at most16 retained superseded audio links, their index ordinal,
  observation revision, cursor and validated public audio job status. Use
  `next_after` when `has_more` is true. The cursor orders retained transitions;
  equal observation revisions on different locators do not skip rows.
- `index_audio_read`: index `job_id`, linked `audio_job_id`, window `ordinal`.
  Authorizes membership in that owner's current or retained index links and
  returns the exact public derived transcript capture. It preserves artifact_id,
  source/version, source-time range, coverage and model/input provenance, without
  inventing literal evidence. Root scope and model/decoder configuration must
  match; no original file, model transport or publication worker is needed.
  Saved captures/receipts are validated by the existing audio queue reader.
- `index_audio_search`: index `job_id`, nonempty `query` <=128 bytes. Searches
  current and superseded retained transcript captures using ASCII case-insensitive
  substring matching. Returns at most10 derived candidates, each with audio job
  and window selectors, locator, artifact/source/version, source-time range,
  coverage, provenance, <=512-byte derived preview and committed live observation.
  Use `index_audio_read` to retrieve the exact selected capture. Reused queue jobs
  are scanned once even when many history transitions reference them. All retained
  captures/receipts are validated, including those beyond the result limit;
  failures return one typed error without partial candidates. Reports
  `scanned_captures`, `uncaptured_windows` and `candidate_coverage` (partial when
  matching captures exceed10, otherwise complete_for_retained_captures).
  This coverage excludes uncaptured windows and the current filesystem. Root and
  model/decoder configuration must match; original files and model transport are
  unnecessary. This endpoint does not implement semantic retrieval.
- `index_pending`: `job_id`. Returns durable reindex intent for indexed items whose
  latest committed observation differs from their indexed version or reports
  missing/unsupported/uncertain. Reads retained state without a root grant.
- `index_reindex_step`: `job_id`, unique `operation_id`, `max_items`1..4. Captures
  pending observed text into retained immutable index versions and atomically
  clears each completed intent. Earlier literal versions remain searchable.
  Audio intents register the observed identity, measure the owned timeline and
  admit a new queue job. Commit rechecks the exact observation revision, content
  hash, mechanical identity and root scope; queue admission, retained previous
  audio linkage, current linkage and intent removal share one transaction.
  Earlier audio jobs/captures remain available through their public queue IDs.
- `index_queue_step`: no selectors or operation ID. Selects at most one pending
  item across the configured owner's captured jobs in the granted root scope.
  Returns an item result or idle:true; intended for bounded automatic polling.
- `index_search`: `job_id`, nonempty UTF-8 `query`<=128 bytes. ASCII case-insensitive
  literal substring candidates<=10, with locator/mechanical identity, immutable
  source/version, evidence/range/coverage and <=512-byte literal preview. Candidate
  coverage qualifies the indexed snapshot only; exhaustive_current_filesystem is
  always false. Caller must inspect/verify/select candidates before acting.

Registration accepts optional `expected_file_identity` (64 lowercase hex metadata
identity from inventory), refusing stale_source before reading mismatched files.
Index supports .txt/.md/.csv/.json/.log, .html/.htm, .docx and PDF text.
Each original snapshot is at most64MiB; indexed text is at most64KiB. HTML/DOCX
reuse the public bounded readable-text parsers and retain extracted_utf8 selectors
and partial/static-readable coverage. PDF requires the host PDFium adapter and
captures up to the first4 absolute pages, retaining blank text-layer status and
partial/text_layer_only coverage. With an OCR configuration, indexing additionally
uses the existing live-bound `read_pdf_ocr` reader for selected pages whose text
layer is empty or whitespace-only. Pages with usable text retain their exact
text-layer extraction. A combined `pdfium-index-fallback-v1` capture preserves
absolute page selectors and the full text-layer and individual OCR captures in
provenance, including extractor identities, recognition confidence and raster
coordinates. Coverage stays partial (`text_layer_with_ocr_fallback`); OCR does not
prove exact recognition or complete document coverage. Without OCR configuration,
the existing text-layer-only behavior remains qualified as partial. Configured
OCR failures fail the item without committing a partial combined capture.
The existing four-page and64KiB indexed-text bounds apply. Chutni publication
regenerates the same combined projection from owned snapshot bytes before
accepting its digest, identity and page selector; it needs the configured PDF/OCR
dependencies. Regeneration uses the owned snapshot; first SDK source registration
still requires the matching live file under the existing publication contract.
Retained index search needs neither the original file nor OCR configuration.
PDF OCR reindex retains previous immutable captures when live bytes change during
OCR, a configured model is unavailable, cancellation is requested or a deadline
expires. Pending intent remains for a later observation/retry; a successful
reindex adds a distinct captured version. Combined-reader publication preserves
the existing native typed dependency/control errors and durable publication
backoff. Recomputed digests cannot make altered recognized text pass the worker's
owned-snapshot regeneration check. SDK artifact reads retain the exact Sources
evidence, page selectors and OCR provenance. Exact durable read retries remain
available after the original, root grant and OCR/SDK worker are absent.
Initial audio discovery admits WAV, MP3, M4A,
AAC, FLAC, OGG, Opus and AIFF through the existing readers and measured timeline.
It reports waiting_audio until all linked jobs have validated captures and, when
required, committed publication receipts. Completion changes the derived audio
count; it never increments literal indexed_items or implies complete recognition
coverage. Watches and bounded refresh now schedule audio observation changes;
the existing manual/background reindex paths execute audio intents. Pending
audio intents remain readable without a root or model grant. Retained derived
audio reads/history now expose current and superseded captured windows. Derived
candidate search uses the retained queue validation above; semantic retrieval
remains in progress. Native compressed discovery/reindex checks qualify nine
MP3/AAC/FLAC/Vorbis/Opus/AIFF/stereo-WAV cases across two overlapping windows,
changed versions, original-absent capture and retained old jobs on macOS/Linux
in normal and sanitizer modes. They use fixture model output, not accuracy
measurement. Native WAV
failure checks exercise SIGKILL inside queue admission and after commit, newer
observation/root replacement races, cancellation/deadline before commit, all
eight audio-job slots and256 real retained linkage transitions. Refusal preserves
the current link and pending intent; post-commit restart does not duplicate work.
Other formats fail unsupported_reader;
directories and special files are skipped.
Incomplete inventory, failed/skipped items or partial text keep content partial.
Captured metadata identities are mechanical checks, not content hashes; versions
and literal evidence come from actual immutable registration and reading.

Bounded capacity:8 jobs,256 items/job,512 durable operation records,8MiB combined
stored text/evidence. Before reading,524288 bytes of headroom are required; at
capacity pending items become qualified failures and earlier results stay readable.
Audio reindex also preserves the existing eight-job audio queue admission cap
and permits at most256 retained audio linkage transitions. Capacity refusal
keeps the previous linkage and pending intent; no retained capture is evicted.
Other snapshot/evidence quotas also apply. No eviction. Database page cap160MiB.
One active index operation, zero queued; competing work returns busy. Get/search
remain readable during indexing through consistent transactions. Responses<=512KiB.

Exact normalized operation retry returns persisted result before source access;
parameter changes conflict. Owner mismatch hides jobs. Running operations recover
as interrupted only after obtaining the exclusive kernel lease. SIGKILL retains
committed items; a new step operation resumes pending items. Interrupted operations
never replay automatically. Errors after admission leave the operation recoverable
as interrupted and preserve prior checkpoints. Root/scope replacement refuses.

Index integrity checks verify evidence digest, contract version, literal kind and
agreement with stored literal text before candidate publication. Failures return a
single structured error, without partial candidates. Native tests use real process
SIGKILL and public admissions for capacity; only owner-local corruption fixtures
open private storage. PDF text indexing is covered by the actual reader/publication integration checks.
Bounded image OCR also reuses the public reader for indexing and publication.
PDF OCR fallback now uses the existing readers as described above. Video/non-text
indexing and neural enrichment remain pending; audio status and retained derived
retrieval use the separate typed queue paths above.

Indexing performs immutable extraction through Sources locator_read.
After inventory-identity-bound registration, live bytes must match the captured
version before reading; file/parent/root bindings must remain stable through the
final extraction check. A freshness failure commits a qualified failed item,
without publishing stale candidate text/evidence. Registration may retain an
immutable snapshot even when later live verification fails. Already committed
index items and historical searches remain captured snapshots, not continuous
freshness claims. Watch-driven invalidation and reindex queues remain required.

Each search candidate also has live_observation, derived from the latest committed
locator record for this owner, captured root scope and relative locator. Status is
unobserved, available, changed, missing, unsupported or uncertain. Available is
qualified against the candidate's immutable version; an observation whose actual
hash belongs to a different version reports changed. Known observations include
locator_revision and observed_at_ms. semantics is committed_observation_history:
search performs no new filesystem observation and never claims current liveness.

New jobs retain their root scope independently of the selected index subdirectory.
Legacy jobs lacking a captured root scope report unobserved rather than inferring
one from the current grant. Different owners, hosts and replaced roots do not
cross-update indexed freshness. Historical candidates remain retrievable with
qualified changed/missing status; current live use still requires locator_read.
Continuous monitoring, invalidation/reindex scheduling and fresh-only retrieval
policy remain required.

## Observation-driven reindex intent

Reconcile and watch checkpoint commits atomically update pending intent for all
matching indexed items in the same owner/root scope. One row per job/item
coalesces repeated changes to the latest locator revision; an available observation
whose bytes match the indexed version clears the row. The retained eight jobs and
256 items/job bound this derived queue to 2048 rows globally, 256 per public reply.
Observation cancellation or failure rolls back both freshness and pending intent.
Different owners, hosts and root generations do not schedule each other's work.
Content observations are qualified against each indexed version independently of
the observer's expected version. An identity mismatch without a verified content
hash remains changed; it cannot clear pending work.

Pending replies include ordinal, locator, indexed_version and validated
live_observation. Evidence digests and observation revisions are checked before
publication. Historical candidates and immutable evidence remain retained.
Intent records required work; bounded execution is described below. Missing-file
policy, new-file discovery, legacy observation catch-up, retries and fair queue
draining remain required. Initial failed/skipped items are not scheduled by this
indexed-item path.

## Bounded reindex execution

index_reindex_step uses the shared exclusive index admission lease. It captures
the observed mechanical identity through registration and locator_read, verifies
the extracted version equals the observation's verified content hash, and checks
that the observation revision and configured root scope still match before commit.
Newer observations refuse stale_observation without clearing their pending intent.
Files changed since observation refuse stale_source; unavailable or unverified
observations refuse live_unavailable. Pending work remains available for a later
observation and a new step operation.

Each item commits a retained immutable version and removes its exact pending
revision in one transaction. Host cancellation before commit rolls back both.
SIGKILL after an item commit preserves it; a new operation resumes remaining work.
Operation recovery/retry and the 512-record admission limit match index_step.
Exact completed retry works offline; admitted errors recover as interrupted.

At most 256 additional versions globally, with combined original/reindexed
text/evidence bounded by 8MiB. Refusal preserves earlier evidence and pending work;
no history is evicted. Historical search includes original and reindexed versions,
with the existing ten-candidate response bound and independently qualified
freshness. Original job inventory/cursor/coverage remains its captured snapshot.
Scheduling compares observations against the latest version for each item.

Execution visits the least-attempted pending item within one selected job, with
ordinal ordering for ties. Admission records a durable attempt before content work,
including attempts interrupted by process death. A missing/failed item stops that
step and retains its intent and attempt count; subsequent fresh operations give
less-attempted peers priority. Repeated observations preserve counters while work
remains pending. Success or matching-byte restoration clears intent and counter
atomically. index_pending exposes validated nonnegative integer attempts, bounded
to one row per pending item; counters saturate at INT64_MAX.

Later sections implement the bounded runner, retry/backoff and new-file discovery.
The recovery section below adds supported text-item retries. Complete missing-file
policy, broader reader recovery and legacy catch-up remain required.

## Cross-job automatic step

index_queue_step uses the same exclusive index lease and item commit validation.
Job selection visits the least recently dispatched ready job, using a durable
monotonic turn. Within that job it visits the least-attempted pending item. One
schedule row per retained job, at most eight globally; invalid turns refuse
corrupt_index, and INT64_MAX refuses scheduler_limit before incrementing.
Turns persist across item success, recurring changes, failed attempts and restart.
A continuously changing job cannot repeatedly win the tie over an older ready peer.

Selection requires the configured owner and currently granted root scope. Different
owners, hosts and replacement roots are excluded; idle:true means no matching
pending work was selected, not that other scopes or the current filesystem are
fully indexed. Item/root/revision validation still applies before commit.

Automatic steps create no durable client operation receipts and do not consume
the 512-record index operation quota. Completed item checkpoints, pending intent,
attempts and dispatch turns provide bounded recovery; a lost reply is not replayed
as an exact saved operation response. A repeat selects whatever work remains.
Explicit index_reindex_step retains its existing exact completed retry contract.
Snapshot, evidence/history and database quotas still constrain automatic work.
The continuous local runner and per-item backoff are described below.

## Continuous local runner

CLI --index-run continuously invokes index_queue_step with the host's root/state/
owner/host configuration. --index-interval-ms accepts100..60000, default1000.
The watch and index runtime modes are mutually exclusive within a process; they
can run as separate processes sharing Sources' bounded database/admission leases.
Runtime functions own process signal handlers and are not a threaded service API.

One private reindex-runtime.lock file admits one index runner per state directory;
duplicates refuse queue_busy. The runner checks its held lease and state identity,
and the captured root scope between polls. Detected replacement stops the process.
Each attempt has a five-second deadline bounded by any earlier host deadline;
parent host cancellation remains connected. SIGTERM/SIGINT cancel content work
and the paced wait. SIGKILL releases the kernel lease for restart; committed item
versions and remaining queue intent stay durable. The one retained lock file is
reused, without per-cycle files or client receipts.

Successful, idle and transient-failure attempts wait the configured interval.
Missing/stale observations and busy/store_busy/deadline failures retry; permanent
quota, integrity or binding errors terminate with a typed envelope. Stdout emits
only a terminal stopped/error envelope. Existing durable pending/attempt status
remains available independently of the runner.

The interval is global polling pace; automatic items also use the backoff below. Tests cover
actual replacements between polls; precheck-to-dispatch/store-open races, long-run
resource/fairness and complete service lifecycle remain required.

## Durable automatic backoff

Automatic attempts reserve a per-item cooldown before content work, so process
death retains it. A completed failure receives its full delay measured from failure
finalization. Levels1..7 use1000,2000,4000,8000,16000,32000,60000ms; level7 remains
capped. Automatic selection excludes cooling items while ready peers still proceed.
idle:true can therefore mean retained work is waiting for its next attempt.

Repeated observations of the same content hash/mechanical identity preserve the
cooldown. A changed verified target clears it; matching indexed bytes or successful
version commit delete it with pending intent. Unverified/unavailable observations
share an unavailable target until verified content/identity is observed. Cooldown
metadata is bounded to one row per pending item, at most2048 globally, and survives
runtime restart. index_pending exposes next_attempt_at_ms and backoff_level;
the level represents admitted automatic retry state, including an active or killed
attempt, rather than a completed-failure counter.

Explicit index_reindex_step bypasses automatic cooldown while preserving its
existing identity, root and evidence validation. Its failure does not advance the
automatic level; successful explicit work clears the row atomically.

Schedules use checked wall-clock milliseconds for restart persistence. A deadline
more than60000ms ahead of the current clock is treated as due, bounding delay after
a backward clock shift. Invalid integer/range metadata refuses corrupt_index before
automatic selection or pending publication. Full clock-failure/resource and service
qualification still remains required.

## Host binding pins at operation admission

Local runtime copies retain state_pinned/state_device/state_inode for core store
admission. A different state directory refuses before creating sources.sqlite3;
an absent pinned directory is not recreated. The index runner also retains the
startup root_scope_pin, enforced when root-scope identity is calculated for queue
selection and indexed version commit. It cannot adopt pending jobs created under
a replacement root between the outer loop check and selection.

These are host configuration fields, never public request selectors. Root-scope
pins constrain those live indexing scope calculations; retained immutable reads
remain available independently of a live root grant. State pins apply to core
store admission. The separate SQLite filename-open race inside store_open and
remaining helper/control-store/race qualification are still required; this does
not claim a complete filesystem capability or SQLite VFS boundary.

## Explicit discovery of additional files

`index_discover` accepts job_id and operation_id. It scans the captured directory
under its original root/directory bindings and appends unseen locators to the
existing job's pending items. A later index_step captures them. Existing items,
cursor, original inventory and historical versions remain retained; it does not
refresh changed successful items. Failed/skipped supported text items enter the
recovery queue below. The accumulated job retains the 256-item
cap, and discovery plus its exact-retry receipt commit together. A saved retry
requires no live root. A different request using the same operation ID conflicts.

After appending items, original inventory coverage still refers to the original
capture; combined content coverage is partial and freshness remains captured_snapshot.
This does not imply complete current directory coverage. Explicit discovery and
the opt-in periodic runner below share the same bounded scan implementation.
Supported failed/skipped text items can be queued for recovery as described below;
full new-file/freshness policies remain under development.

Job responses include `latest_discovery`: null before the first discovery, otherwise
an object with a monotonically increasing `cycle`, `observed_items`, the latest
scan's `inventory_coverage`, and `freshness: captured_snapshot`. This describes that
bounded scan, including when it observes fewer files than the accumulated job.
It does not assert current filesystem completeness. The original `inventory_coverage`
remains separate. One scan manifest per job (at most 524288 bytes) and its checksum
commit atomically with appended items and the operation receipt. Offline index_get
validates retained metadata; exact operation retries retain their original result.

Discovery samples host cancellation/deadline and revalidates the original root and
directory after saving its scan and operation receipt, immediately before commit.
A refusal rolls back all appended items, totals, latest-scan metadata and the new
receipt. A process killed before commit likewise retains the prior committed job;
a fresh request can retry the uncommitted operation. The accumulated item limit
includes previously indexed or missing locators, so replacement filenames cannot
bypass the 256-item bound.

## Periodic discovery and initial indexing

Host SourcesConfig.discovery_interval_ms enables periodic refresh at100..60000ms;
zero disables it. CLI --index-run --discovery-interval-ms1000 enables the same
work in the owned index runtime. The normal --index-interval-ms polling pace still
applies, so each job's interval is a minimum cooldown rather than a deadline.
This setting is a host grant, not a public request selector.

index_refresh_step accepts only contract_version and op. It attempts at most one
bounded directory scan, one initial pending item, one queued recovery item and
one indexed-locator freshness observation,
across jobs belonging to
the configured owner and original root scope. Independent last-turn counters
give ready peers a turn; per-phase cooldowns reserve an attempt before work.
Missing or stale directories and job-level limits return discovery_error or
initial_error while healthy jobs can proceed. Failure cooldown doubles from
1s to60s. Success resets that phase's level; scan success waits the configured
interval, and successful item progress can continue on the next tick.

The response reports scanned_jobs and processed_items (each0..1), plus nullable
per-phase errors. processed_items counts advanced checkpoints, including skipped
or failed content items, rather than claiming every item was indexed. Schema,
resource, corruption, clock and scheduler failures remain top-level errors.
Refresh requires an enabled host configuration; zero returns discovery_disabled.

One private index_refresh_schedule row per job (at most8) retains scan and initial
next times, failure levels and turns. Selection validates integer bounds and
refuses turn overflow; a wall-clock jump beyond the60s horizon does not strand
ready work indefinitely. Reservations can survive process death before content
commit, and work resumes after their bounded cooldown. Initial item commits and
scan commits are separate checkpoints; a later phase's error does not roll back
an earlier phase's completed work. Repeated background refresh creates no new
client operation receipts. Exact receipts remain for explicit client operations.

The existing single index-runtime lease, state/root pins, bounded attempt deadline
and TERM/INT handling apply. With refresh enabled, each worker tick runs this
refresh before observation-driven index_queue_step reindexing. Newly discovered
items enter the existing immutable text/evidence pipeline. Existing changed indexed files receive periodic locator observations through the
phase below, and existing failed/skipped items can enter the recovery queue below. Full freshness policy and service
qualification remain open.

## Recovery of failed and skipped text items

Discovery queues an existing failed/skipped locator when it is now a regular file
with a supported plain-text extension. It retains the initial cursor and all
successfully indexed items/versions. index_refresh_step attempts at most one
queued recovery after its scan/initial phases, reporting recovered_items0..1 and
a nullable recovery_error. This is an additional bounded content attempt.

At most2048 index_recovery rows (one per job/item) retain the scanned identity,
next retry time, failure level and fair turn. Attempts reserve an exponentially
increasing1..60s cooldown before capture. Repeated scans of an unchanged target
preserve that cooldown; a changed metadata identity resets its level/next time.
Absent files are queued when a later bounded scan observes them again. Unsupported
extensions remain refused; neural/readers beyond the existing text pipeline are
not silently added.

Capture uses the scanned identity and existing immutable registration/live-read
validation. Acceptance revalidates the original root/directory, evidence quota and
host controls. Updating the formerly failed item, removing its queued intent and
recording its recovery marker commit together; the initial cursor does not change.
Successful indexed items remain on the existing observation/reindex path.
Job replies expose recovered_items_total; combined content coverage is partial
after recovery, because accepted evidence differs from the original inventory.
Per-item recovery markers are also bounded by the job/item limits.

Scan, initial indexing and recovery remain independent checkpoints. Cancellation
and deadline failures are top-level errors; recoverable target/limit failures leave
the queued cooldown available for a later retry. Recovery-specific crash/control/
Additional race/corruption qualification and complete freshness policy remain open.

Job replies include recovery_queue with pending_items, highest_failure_level and
earliest_retry_unix_ms (null when empty; zero means an immediately eligible target).
These are retained retry metadata, readable without a live root grant; they do not
assert current filesystem availability. The initial job status may be completed
while recovery remains queued. Metadata types, identity format, ordinal bounds
and the referenced failed/skipped item are validated before a job response is
rendered. Discovery also validates retained queue metadata before replacing a
changed target, so a rescan cannot silently normalize corrupt retry state.

Recovery-specific tests pause after evidence update, intent deletion and recovery
marker insertion, before COMMIT. They qualify cancellation/deadline refusal,
actual SIGKILL recovery, root/directory replacement rollback, preserved cursor
and retry reservation, and later successful retry on macOS/Linux. These checks
cover that commit boundary, not every filesystem race or resource interleaving.

## Periodic observations of indexed locators

An enabled refresh now attempts one indexed-locator observation after its scan,
initial and recovery phases. observed_items is0..1 and freshness_error is nullable.
At most8 index_freshness_schedule rows retain per-job last ordinal, next time and
fair turn. Ready jobs receive turns; each selected job rotates through its indexed
items, including missing files. Each job waits at least the configured discovery
interval between observations. Full sweep latency also depends on polling pace,
job/item count and admitted work; this is checkpoint freshness, not a current
filesystem completeness guarantee.

The phase validates the latest retained evidence checksum/version and original
root/directory bindings, then uses the existing bounded locator hash observation
and physical revalidation. It creates no client locator operation receipt. A
committed changed/missing/restored observation updates the existing reindex intent
transactionally. The index runtime subsequently invokes index_queue_step to
accept a new immutable version; old evidence remains searchable with its own
qualified observation status. Full hashing detects changed bytes even when inode,
size and modification time match prior inventory metadata.

Observation admission still honors owned source claims and the256 live-locator
cap, with per-target refusal rather than silent eviction. Locator revisions now
refuse malformed values or signed-integer overflow. Host cancellation/deadlines,
locator writer leases and the normal worker root/state/shutdown pins apply.
Further periodic-observation race/resource/corruption qualification and complete
service policy remain required.

Periodic observation acceptance now revalidates held file/parent identities and
the named root after staging locator history and index intent, immediately before
COMMIT. Final host controls are sampled around these checks. A mismatch rolls
back observation history and derived intent together, retaining the earlier
committed checkpoint. Shared locator commit logic applies this final check to
explicit reconciliation and watch checkpoints as well.

The native periodic-observation fixture pauses after staging these writes.
Cancellation/deadline, actual SIGKILL, file mutation/deletion and root/directory
replacement preserve the old observation revision and produce no new reindex
intent. A later successful observation/reindex preserves historical evidence
and accepts the changed version. Signed-integer locator revision overflow is
explicitly refused. These are commit-boundary tests, not an exhaustive filesystem
race proof.


Reader integration retains the exact public reader envelope and its digest;
source/version still identify original bytes, not decoded text. PDF search text
is a mechanical newline join of selected page text for candidate matching and
preview; it does not replace the per-page evidence or create a new evidence ID.
Search candidates now include original reader provenance. HTML/DOCX use an empty
selected-range probe to learn bounded decoded length, then a verified live read;
PDF uses page1 to learn total pages, then a verified range through min(total,4).
Every live read retains the existing before/after file/root/parent identity checks.
Oversized aggregate page text refuses indexing; no partial item is committed.
The same capture path serves initial indexing, recovery and retained-version
reindexing. Discovery recognizes the same suffixes, so prior unsupported items
can be recovered after the reader becomes available.

Indexed text publication revalidates original owned media claims, snapshot SHA,
evidence digest and flattened text agreement. The isolated publication worker
reuses HTML/DOCX parsers to prove the original extracted byte selection and
fingerprint, or reuses the configured PDFium reader to reproduce the exact
selected-page envelope. Chutni selectors preserve extracted_utf8 byte coordinates
or absolute pages, and parameters retain the complete original Sources evidence.
The SDK remains the artifact/catalog writer. Later changed-file reindexing retains
old evidence and publishes the new version independently.

With CHUTNI_LIBRARY and actual PDFIUM_LIBRARY configured, test-publication and
sanitize-publication run HTML/DOCX/PDF index/reindex/publication/artifact-read
fixtures in addition to existing text, automatic runtime and crash-recovery cases.
Without PDFIUM_LIBRARY, only the prior text fixture runs; that is not a reader
integration pass. PDF/DOCX fixture generators are test tools, not production
reader dependencies. Sources area4 and full independent release remain open.

Image indexing recognizes PNG, JPEG, BMP, TIFF, GIF, WebP and PNM-family suffixes
and delegates decoding/recognition to the existing bounded image reader. The host
must configure Tesseract, Leptonica and trained data. An empty selected-range
probe determines the recognized UTF-8 length; capture uses the same verified
locator read and immutable snapshot path as other readers. Evidence stays
partial/ocr_recognition_uncertain, with confidence, original raster dimensions,
extractor fingerprint and extracted_utf8 coordinates. There is no automatic
PDF OCR fallback: PDF indexing still selects native text from its first four pages.

Image publication reruns the existing OCR adapter inside the isolated worker and
checks text, fingerprint, confidence and raster identity before calling Chutni.
Recovery compares the complete retained evidence structure as well as artifact
text and identifiers; altered recognition metadata refuses chutni_publication_conflict.
Later encoded-image changes
produce independent retained versions even when their recognized text is equal.

Configure OCR_FIXTURE, TESSERACT_LIBRARY, LEPTONICA_LIBRARY and OCR_TRAINEDDATA
alongside CHUTNI_LIBRARY to enable the actual image integration fixture in
test-publication/sanitize-publication. An omitted fixture skips image qualification.
The fixture tests one English PPM raster; it does not establish OCR accuracy for
every supported format, language, model or multi-image input.

## Derived visual indexing

Video discovery and changed-file reindexing use the same owned registration,
index lease, committed observation revision and atomic queue association paths
as audio. index_get adds queued_visual_items, completed_visual_items and
visual_jobs; waiting_visual means the inventory has been visited but current
visual jobs are unfinished. Captions do not count as complete literal text.
index_visual_history/read/search provide retained association pagination,
validated selected-frame reads and bounded lexical candidates. See
[visual-v1.md](visual-v1.md) for arguments, coverage and validation. Audio and
visual retained association history share the existing256-entry bound.
Enrichment and semantic retrieval remain separate requirements.
