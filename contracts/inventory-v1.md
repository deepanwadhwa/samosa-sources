# Read-only inventory v1

`{"contract_version":1,"op":"inventory","path":"relative/directory"}`.
Empty path selects host-configured root. Absolute/dot/empty components, symlink
traversal, trailing slash and non-directory selection fail unauthorized_path.

Result: host_id (configured endpoint namespace), scope_identity (host/directory
inode/locator identity), scope, inventory_coverage, content_coverage and entries.
Entries contain locator/kind/eligible/identity/byte_size/content_version:null/
content_coverage:uninspected. Metadata identity hashes device/inode/size/mtime;
it does not certify bytes or authorize actions. File actions must separately bind
qualified content version and recheck identity before effects.

Complete inventory declares exact returned/total entries within traversal policy;
partial inventory declares total_entries:null and cannot support global absence
or counts. Excluded symlinks/specials are listed with eligible:false and counted.
Source text isn't read. Bounds:256 entries/512 names/32 directories/depth4/4096-byte
locator/512KiB maximum output. Invalid UTF-8 names are counted and qualify partial.
source_changed refuses a modified directory capture; no implicit full-walk fallback.
