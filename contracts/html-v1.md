# Native HTML source contract version1

register_html: op,contract_version=1,path and optional expected_file_identity.
Authorized root and trusted owner are host configuration. Validate strict UTF-8
HTML document bytes before immutable registration, at most64MiB. Source/version
SHA256 binds original bytes; claim media is text/html. Separate owner claims
protect access and provenance even when bytes deduplicate.

read_html: op,contract_version=1,source_id,version,start_byte,end_byte. Optional
operation_id,parent_operation_id,timeout_ms follow the existing durable read
control contract. The exclusive range refers to extracted_utf8 coordinates,
not original markup offsets. Full deterministic readable output is bounded64KiB;
excess refuses html_output_limit even for a small requested range. UTF-8 codepoint
boundaries are required. No renderer, network, JavaScript or model is invoked.

Evidence kind=literal includes selected readable text, immutable original
source/version, reader html-readable-v1, fingerprint, original locator and exact
range identity. Coverage is always partial/static_readable_text_only: embedded
images, CSS rendering and dynamic content are not inspected. This is extracted
literal text, never a generated summary. Script/style/head/template/SVG/iframe/
object content is suppressed and supported entities decode deterministically.

Private cache identity includes owner, original version, selector, reader and
fingerprint. Hits revalidate original snapshot digest/UTF-8 and cached response
identity. A cache miss validates the decoded selector after bounded extraction.
Cancellation/expiry refuses publication. Exact tracked retries return saved
public results, including parent reference, independently of the live root.
Existing snapshot/cache/operation capacities apply; no separate unbounded store.

Focused qualification: make test-html sanitize-html. Native parser tests and
public library acceptance require no Agent/Inference/App or legacy checkout.
Public CLI, stronger corruption/control/platform/release acceptance remain in
S16a until recorded as passed; these initial tests do not close all Sources.
