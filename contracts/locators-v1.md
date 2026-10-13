# Observed locator contract v1

Host grants root/state/owner/host_id. Requests cannot expand them. Physical paths
are relative locators on that configured endpoint. Immutable source/version SHA
and evidence records remain unchanged when locations change.

locator_reconcile: contract_version1,operation_id,locators1..8. Each entry has path
(nonempty relative, <=1024 bytes, no empty/dot/parent components),expected_version
(registered SHA belonging to this owner), optional expected_device/expected_inode
(both unsigned native IDs <=INT64_MAX). Duplicate paths/unknown fields refuse.
A physical tuple constrains the observation to the historical artifact; a
same-byte foreign replacement reports changed with zero content reading.

Each observation includes owner/scope/path locator_id, revision, host_id,
scope_identity, path, identity_constraint, expected version/optional physical IDs,
status/reason, content_version, observed_content_hash, mechanical file_identity,
actual device/inode where the final selected node was observed, checked_bytes,
and observed_at_ms. Status available requires a stable streamed SHA match;
changed distinguishes physical/content mismatch; missing requires a stable
absence observation; unsupported covers unsafe/special/oversize nodes; uncertain
covers unavailable reads. An ancestor inode is never presented as the selected
file's inode. observed_content_hash does not automatically register a new source.

Freshness observed_checkpoint and result_semantics committed_observation_history
are explicit. These are per-locator observations, not continuous monitoring or
an atomic filesystem-wide snapshot. exhaustive_current_filesystem is always false.
Changed file/directory/root while observing refuses source_changed without
committing partial observations. Existing immutable evidence remains readable.

locator_get: contract_version1,locator_id. Returns latest owner-bound observation
with freshness committed_history and requires no root/filesystem read. A new
reconcile operation is required for a new current observation. Completed exact
retry returns the original recorded result before root access, without revision
increment; changed parameters conflict. Host identity is part of retry identity.
Root inode changes produce a different locator scope/id; previous history stays.

Capacity256 associations and256 successful operation histories globally, no
eviction; one active observer writer,zero queued. <=512MiB content read per operation (eight originals of at most64MiB each)
through64KiB buffers, <=64KiB JSON, existing160MiB SQLite ceiling. Capacity checks
precede content reading. Observations/results commit together. Writer death before
commit leaves old history intact; read-only observation may be retried safely.
Public consumers never open private Source tables or treat Agent receipts as
current truth. Sources independently verifies its grant and actual files.

Host-provided cancellation/deadline controls are checked before admission,
between observations, for each64KiB hash chunk and before transaction commit.
An interrupted observation returns cancelled/deadline_exceeded without new
history or revision; exact committed retries still return saved history offline.
These host controls do not yet add public operation_cancel to locator operations.

## Live reads

`locator_read` accepts contract_version1, path, optional expected_device and
expected_inode (both required together), and `read`, a nested ordinary public
reader request. Supported nested operations: read_text/read_html/read_docx/
read_image/read_pdf/read_pdf_ocr/prepare_audio/prepare_visual. Nested source_id and
version must be equal owner-bound immutable hashes. Nested control fields and
recursive locator requests are refused. The host still supplies root and owner.

Sources streams and verifies current live bytes against that version before
reading the immutable snapshot. Physical constraints, when supplied, also pin
historical device/inode. Missing/changed/unsupported files refuse with
live_source_unavailable; races during extraction refuse source_changed. The held
file and parent descriptors, relative directory bindings and root identity are
rechecked after extraction, including warm-cache retrieval. No evidence escapes
on failed freshness validation. Existing immutable read operations remain usable
for historical citations after replacement/deletion or while the root is offline.

Success wraps the ordinary result in evidence, with freshness verified_live_read,
path, file_identity and observed_at_ms. It is an observation through the final
check, not a guarantee about filesystem changes after response. No locator history
is committed and no successful observation is reused to bypass future checks.
At most64MiB is streamed through64KiB buffers; nested output is at most2MiB plus
4096 bytes of wrapper allowance. Host cancellation/deadlines span the observation
and nested reader. locator_read accepts optional operation_id, parent_operation_id and timeout_ms
through the shared durable operation wrapper. operation_get/operation_cancel
apply to the whole live observation and nested reader. Completed exact retry
returns the saved observation/evidence even offline: it is the original verified
read at observed_at_ms, not a new current observation. A new operation identifier
is required to recheck freshness. Nested key order and escaped strings normalize
for retry identity; changed selectors or physical constraints conflict.
