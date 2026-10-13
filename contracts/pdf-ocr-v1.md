# PDF OCR v1 (S17a qualification in progress)

Use `register_pdf` for original immutable bytes, then explicit `read_pdf_ocr`
with source_id, version and inclusive absolute start_page/end_page. At most four
pages are selected; original source bytes remain capped64MiB. `read_pdf` remains
the separate text-layer operation. OCR neither silently replaces text reading nor
claims that recognized text exhausts page content.

Automatic indexing may combine text-layer pages with OCR for empty selected
pages through the existing public readers. See [index-v1](index-v1.md) for that
capture's page/provenance, reindex and publication contract; the standalone
read_pdf/read_pdf_ocr selectors and coverage above remain separate.

Each selected page renders at150dpi in grayscale. Render dimensions must be
finite and positive, at most4096 per axis and4Mi pixels per page, validated before
raster allocation. Oversized pages are refused rather than silently downscaled.
Raster buffers are released per page. The owned renderer supplies encoded PGM
from bounded pixels to OCR; this internal render representation has a4MiB+64
limit and does not raise the public original-image64MiB limit. At most64KiB of
recognized UTF-8 text is retained across all selected pages.

The result has `pages` with original absolute page numbers, recognized literal
text, native confidence0–100 and rendered-pixel dimensions. Provenance includes
150dpi, `pdfium-ocr-v1`, owning locator and a combined fingerprint of PDFium,
OCR engine/decoder/model, language and render/reader contract. Empty recognition
does not establish absence. Coverage always remains partial with
`ocr_recognition_uncertain`. Generated descriptions are not OCR text.

Configured PDFium and OCR runtime are optional trusted host artifacts. Owner
claims, immutable snapshot validation, fingerprinted cache and durable
operation_id/parent_operation_id/timeout_ms apply. All selected pages must succeed
before successful evidence or cache publication. Cancellation, deadline, page
failure or artifact change discards the whole in-flight result and preserves
existing evidence. The request shares one cooperative30s maximum across all
pages, shortened by an earlier host deadline. Native render/init/decode execute
in an owned helper whose parent can kill/reap it on cancellation/deadline without
waiting for those calls to return. See docs/native-worker-v1.md; remaining
resource acceptance is required before declaring the reader fully qualified.

Acceptance uses a native image-only PDF fixture: page1 blank, page2 a known
numeric sentinel raster, with no text layer. Qualification checks OCR versus
empty text-layer reading, absolute pages, cache, owner refusal, optional failure,
actual monitor cancellation/deadline, no partial cache, durable retry, and
pre-allocation oversized-page refusal. Platform/release gates remain open until
the evidence log records their terminal outcomes.
