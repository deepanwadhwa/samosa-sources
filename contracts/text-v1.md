# Public JSON contract version 1

One request/result per CLI process. Unknown fields/operations fail explicitly.
Host configuration (--root/--state) is never accepted from request JSON.
Host --owner defaults to local-owner and binds registration/read authority. Content
SHA identifies immutable bytes; knowing it does not grant another owner's claim.
Registration creates an owner/id/media claim with that owner's first admitted
locator. Read requires the same owner and registered media. Identical content may
share private bytes while each owner receives their own provenance. Claims are
bounded to256 globally without eviction; claim_limit refuses a new authority.
Invalid owner configuration refuses before work. Unclaimed old prerelease
snapshots need explicit re-registration; their bytes are retained.

- describe: contract_version, op. Returns component/version/operations/limits.
- register_text: contract_version, op, path. Returns source_id, version, bytes,
  media_type=text/plain, identity=immutable_sha256. Source/version IDs are the
  lowercase SHA-256 of captured bytes. No policy relies on live file contents.
- read_text: contract_version, op, source_id, version, start_byte, end_byte.
  Returns evidence_id (SHA-256 over reader/version/exact range), literal text,
  range, coverage(status complete|partial,total_bytes,inspected_bytes), and
  provenance(reader,locator). Empty selected ranges are legal. End is exclusive.
  Complete means exactly all registered bytes, not all meanings/visual regions.

Failure: {contract_version:1,error:{code:...}} and nonzero process exit status.
Errors include invalid_json, unsupported_contract, invalid_request, unknown_field,
unsupported_operation, unauthorized_path, unsupported_source, source_too_large,
source_changed, invalid_utf8, resource_limit, storage_limit, state_unavailable,
store_busy, store_failed, unknown_source, stale_source, invalid_range,
utf8_boundary, corrupt_snapshot. Limits are published by describe. No automatic
full read or eviction fallback. A caller must preserve qualified provenance.
Private cache keys include owner; public source/version/evidence SHA identities
retain their documented semantics. Owner claims are checked before cached access.

When describe advertises request_correlation/request_budgets, read_text,
read_pdf and inventory also accept optional operation_id (nonempty opaque ASCII
identity<=128), parent_operation_id (same bounds, requires a different operation
ID), and timeout_ms (integer1..60000, relative to request receipt). Qualified
results and typed failures echo valid references. References grant no authority,
and these reads do not advertise durable operation status/cancellation or request
deduplication. Evidence identity/cache provenance excludes transient references.
Mutating index/locator operations retain their separate strict contracts.

Host configuration may supply an earlier monotonic deadline and scoped cancellation
callback. Public budgets shorten it. Expired/cancelled work returns
deadline_exceeded/cancelled, preserves prior snapshots/cache and does not publish
a fresh interrupted evidence-cache entry. Checks precede work, occur between
inventory entries and precede cache commit. Native extractor/kernel calls require
owned helper isolation for a hard process deadline; this cooperative library
contract does not claim to interrupt every native syscall or extractor call.
