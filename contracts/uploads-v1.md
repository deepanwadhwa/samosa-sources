# Immutable upload ingestion: host binary entry point

`sources_upload(config, format, bytes, length, output)` accepts a stable caller
buffer without a filesystem root grant. The component copies it in64KiB chunks,
checks scoped host cancellation/deadlines, validates its format and admits it
through the shared immutable snapshot transaction. The public format enum covers
text, PDF, HTML, DOCX, image, plain WAV and encoded media. Format acceptance has
the same bounded sniff/UTF8 rules as registration; it does not prove full extraction
or decoding coverage. Byte limit is64MiB, with existing shared snapshot/byte/claim
quotas and no eviction. The caller keeps its input stable until the call returns.

Success returns the usual immutable source_id/version/bytes/media_type envelope.
Owner-bound ordinary read/preparation operations can consume that version without
a live root. Deduplication shares immutable bytes; each owner independently obtains
its claim. New upload claims use `upload:` as provenance, not a live filesystem
locator. Existing primary owner/id/media provenance is retained on deduplicated
claims, matching registration's existing first-claim behavior.

This is a host C ingestion entry point. Durable JSON upload sessions are described
below. Authenticated multipart transport and complete service admission remain
part of Sources completion. Scoped cancellation before snapshot commit rolls back
admission; direct caller retry deduplicates bytes without an operation receipt.

## Host streaming entry point

`sources_upload_stream(config, format, declared_length, reader, context, output)`
requests at most64KiB per callback. The host reader returns zero on success,
reports no more bytes than requested, and reports zero bytes only at EOF. Sources
requires the exact declared length, samples scoped controls before and after each
callback, and accepts no snapshot after a read/length/chunk error. It probes one
byte beyond the declared length to reject excess input. Typed errors are
upload_read_failed, invalid_upload_chunk and upload_length_mismatch.

The reader/context belongs to the host; Sources does not choose sockets, paths
or providers. The host must bound callback blocking using its transport controls.
Sources retains an owned buffer up to the existing64MiB limit; streaming callbacks
do not yet provide disk staging, resumability or a memory-independent large-file
store. `sources_upload` delegates through the same pipeline with a buffer reader.

## Durable resumable sessions

`upload_begin` takes operation_id, format (`text`, `pdf`, `html`, `docx`, `image`,
`wav`, `media`) and declared bytes. `upload_status` and `upload_abort` take
upload_id. These initial JSON operations persist owner-bound metadata and return
upload_id/status/bytes/received_bytes. Identical begin retries retain their original
lifetime; changed parameters conflict. Abort is repeatable and releases capacity;
an exact begin retry after abort reports the aborted lifetime.

Limits are eight open sessions,64MiB aggregate reserved bytes and128 retained
session identities, with no eviction. Session reservations also count against
snapshot/byte limits so live and direct uploads cannot consume reserved capacity.
`upload_append` takes upload_id, offset and canonical base64 data. Nonfinal
chunks contain exactly8192 decoded bytes; the final chunk contains the exact
remaining bytes. Chunks and received_bytes advance in one transaction. An exact
retry at an earlier chunk offset validates the stored bytes and SHA256 digest and
returns current session progress; conflicting bytes report idempotency_conflict.
Gaps, unaligned offsets and incorrect chunk sizes report upload_offset. Invalid
base64, including nonzero padding bits, reports invalid_upload_chunk. Staging has
a global8200 chunk-row cap. Status and append reject inconsistent staged layout;
retry additionally verifies its stored digest. Abort deletes staged chunks and
resets received_bytes to zero in the same transaction.

`upload_commit` takes upload_id. It requires all declared bytes and validates
ordered staged chunks and their SHA256 digests. Publication buffers at most64MiB,
uses shared format/owner/snapshot admission, and atomically replaces staging with
an immutable snapshot claim and a digest-protected receipt. An exact commit retry
returns the original source envelope. Format rejection restores the open session
and staged bytes. Abort after completion leaves the accepted source intact.

Completed-session operations validate receipt digest, JSON identity/version,
format and length against the accepted snapshot metadata and owner claim, and
require no retained staged chunks. Fresh executable processes qualify resume,
read and exact retry; two processes starting together qualify identical append
and commit retries with one snapshot/claim/receipt.

Scoped cancellation at the final precommit control sample preserves the open
session, staged chunks and zero accepted snapshots/receipts. Killing a publication
process with SIGKILL at that same sample recovers the rollback journal in a fresh
process and permits an exact commit retry. This covers process death before
COMMIT, not machine power loss or every SQLite commit phase. `describe` advertises
the five upload JSON operations and session/reservation/chunk bounds.

Actual64MiB staging/publication/retry and tail reads pass both-platform normal
and sanitizer gates. A covering expression index serves layout checks from chunk
metadata without rereading payload pages.

Append, status, abort and commit accept optional operation_id,
parent_operation_id and timeout_ms (1..60000) through the existing durable request
control interface. Successful mutations retain their committed result if a late
cancellation arrives. upload_begin's operation_id remains its lifetime identity;
it is not a generic request-control receipt. The public operation_cancel API at
the final precommit sample and timeout_ms expiry at that sample preserve staging
and save typed cancelled/deadline_exceeded operation results. Exact cancelled
request retries retain that result; a new operation identity can publish the upload.
A separate exec publishing worker also pauses at the final precommit sample;
the parent verifies running, cancels through the public API and confirms rollback
and successful publication by a fresh process. Broader timing races, crash phases
and authenticated transport remain open at the full service boundary. A process
killed after the upload transaction commits but before any response retains one
snapshot/claim/receipt and no staging; an untracked fresh-process commit retry
recovers the accepted envelope. Generic operation receipts are a separate lifecycle:
if their process dies before saving a terminal receipt, status reports interrupted
rather than inventing a completed response. The upload's own receipt still permits
recovery using the upload identity and a new request operation identity.
An upload_id is not an accepted source_id or an authorization credential.

Reservation admission validates every retained session's integer bounds, format,
status and received offset before trusting aggregate capacity. New sessions and
new snapshot bytes both refuse corrupt_upload if malformed reservations could
mask each other in the total. This does not provide authenticated transport or
replace full database-integrity qualification.
