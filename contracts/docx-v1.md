# Native DOCX source contract version1

register_docx accepts op,contract_version=1,path and optional mechanical
expected_file_identity. The host controls authorized root and owner. Registration
captures at most64MiB immutable ZIP-shaped bytes; full OOXML/package/text validation
occurs during read. No literal evidence is asserted by registration alone.
Original bytes determine source_id/version and a separate owner/media claim.

read_docx accepts op,contract_version=1,source_id,version,start_byte,end_byte plus
optional durable operation_id,parent_operation_id,timeout_ms. Exclusive selectors
refer to extracted_utf8 coordinates, never compressed ZIP offsets. Full readable
output must fit64KiB before a selected range can be published. UTF-8 boundaries
are required. No script, macro, external entity, filesystem reference or network
is executed. Original immutable versions remain usable after live changes.

Package bounds:2048 entries,8MiB total declared expansion,1MiB per-entry/XML,
64 auxiliary parts, compression ratio200 above64KiB, XML depth64,64 attributes per
element and ASCII qualified name length127. Duplicate attributes refuse.
Only stored and Deflate entries are supported. Validate safe paths/duplicate names,
required package parts/content-type/relationships, XML roots/structure/UTF-8,
DTD/entity refusal and CRC when extracting inspected parts. Uninspected media
CRC/rendering is not claimed. Unsupported or excessive input returns typed error,
never partial literal evidence. Result evidence is bounded readable text from
main document then sorted supported auxiliary parts, with deleted text omitted.
Generated filenames are excluded from literal text.

Evidence kind=literal, reader=docx-readable-v1, original source/version and exact
selector/fingerprint bind identity. Coverage always remains partial with reason
 ooxml_readable_text_only; images, formatting, macros and other package content
are uninspected. Reader fingerprint binds the owned versioned native contract.
Owner-scoped cache hits verify original digest and cached response identity;
misses validate full package and decoded selectors. Existing cache quotas and
durable read worker/history/result quotas apply. Cancellation and expiry prevent
cache/evidence publication. Exact tracked retries return saved envelopes offline.

Focused tests: make test-docx sanitize-docx. Native ZIP fixture generation requires
no Python, external archiver or other component. Full independent platform,
limit/failure/control and release acceptance remain S16b work until recorded.
