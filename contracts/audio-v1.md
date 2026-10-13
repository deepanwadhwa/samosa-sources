# Prepared audio v1 — S18a in progress

`register_audio` snapshots an authorized relative `path`, with optional inventory
`expected_file_identity`. The current native reader validates RIFF/WAVE PCM16
little endian, mono,16000Hz. Originals are bounded64MiB. `register_media` separately
admits encoded candidates; configured optional native conversion/resampling is
described in media-v1.md. Sources-owned transcription through the public Inference
callback is described in transcript-v1.md; production transport remains open.

`prepare_audio` takes source_id, version, inclusive start_ms and exclusive end_ms
in the original source timeline. A window is nonempty, at most30000ms, entirely
inside the original PCM frames. Millisecond boundaries map exactly to16 frames.
The public timeline bound is4h; out-of-bound selections refuse before decoder
artifact queries. For encoded sources, selected audio must have verified timestamp
coverage; unsupported gaps refuse instead of padding the prepared output.
The reader validates RIFF/chunk lengths, at most128 chunks, a single supported
format and a single nonempty aligned data chunk, including odd-chunk padding.
Unknown metadata chunks are skipped; duplicate format/data chunks are refused.

Response kind is `derived`, artifact_type `prepared_audio`, media_type `audio/wav`
and encoding `base64`. The prepared WAV retains exact selected PCM samples with
a canonical header and starts its local timeline at zero. Source range retains
original absolute milliseconds. Artifact identity binds source version, selected
time range and the versioned reader fingerprint. Owner provenance binds the
original locator. Coverage stays partial/media_preparation_only. There is no
literal text, transcript or evidence_id. `transcribe_audio` separately reports
recognized text, uncertainty and timestamps mapped to this source range.

Owner authority, original snapshot integrity and scoped cancellation/deadline
checks apply before publication and during bounded copying/encoding. Database
read statements are finalized before preparing the copied immutable bytes.
Failed/cancelled preparation publishes no artifact. Optional operation_id,
parent_operation_id and timeout_ms use the existing durable control boundary;
successful and cancelled saved retries work with original/root absent. A changed
time range under the same operation ID is an idempotency conflict.

Prepared artifacts use an owner-scoped durable cache keyed by exact source
version, time range and reader fingerprint. Stored response digest, derived type,
identity, timestamps, provenance and bounded base64 shape are validated on reuse;
original immutable bytes and owner claim are checked first. Hits skip WAV window
copying/encoding, stay byte-identical, and work with the original/root absent.
Cache validation polls the owning cancellation/deadline. Corruption is a typed
corrupt_cache error and does not invalidate other entries. Failed/cancelled work
creates no entry. Saved operation retries remain separate immutable histories.

Prepared artifacts and literal evidence share one128-entry/8MiB response budget,
with2MiB per response, lowered by host cache_limit/cache_byte_limit. Count
admission is checked before preparation; exact byte admission is checked inside
the committing transaction afterward. Concurrent duplicate admission compares
existing qualified output rather than overwriting it. There is no eviction; at
capacity old qualified hits remain usable and new entries return cache_limit.
