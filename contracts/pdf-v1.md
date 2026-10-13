# Optional native PDF text contract version 1

Register_pdf(contract_version=1,op,path) captures authorized bytes <=64 MiB with
PDF signature. Same immutable identity/quota rules as register_text. Format
metadata is Sources-owned. Registering a PDF does not require an installed reader.
Read authority requires the configured owner's PDF media claim. Another owner's
known SHA or cached extraction cannot grant access or locator provenance. Registering
the same bytes as text alone does not grant that owner's PDF media claim.

Read_pdf(contract_version=1,op,source_id,version,start_page,end_page) requires
1-based inclusive range <=4 pages. Host supplies --pdfium-library, never request.
Returned evidence has literal pages with absolute page numbers/text/text_status,
range and coverage {status:partial,reason:text_layer_only,total_pages,
inspected_pages}; provenance reader=pdfium-text-v1/locator. Blank text layers
remain empty_text_layer; no claim of OCR/visual inspection. Limit 65536 UTF-8
bytes per page, refuse oversized output before publishing evidence.

Errors include invalid_range, pdf_unavailable, pdf_adapter_incompatible,
pdf_encrypted, pdf_malformed, pdf_page_failed, pdf_text_failed,
pdf_text_unreadable, extraction_too_large and shared snapshot errors.
Only selected pages/text pages are loaded; document inventory supplies page total.
No automatic OCR/full read/cache fallback. PDF calls are isolated by the CLI
process; no library-thread concurrency or persistent service claim in S03.
