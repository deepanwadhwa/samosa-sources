# Image OCR v1 (S17a in progress)

`register_image` accepts an authorized relative `path` and optional inventory
`expected_file_identity`. Original bytes are snapshotted and SHA256-addressed;
the owner receives an `image/raster` claim. Registration performs signature
screening for PNG, JPEG, TIFF, BMP, GIF, WebP and PNM, without requiring OCR installed.
Signature screening does not prove decodability. Reads perform bounded native
header validation and decoding. Originals are limited to 64MiB.
For multi-image containers the current raster reader recognizes the decoder's
first image only; it does not establish complete frame/page coverage.

`read_image` requires source_id, version and exclusive start_byte/end_byte in
the **recognized UTF-8 text**, not encoded image bytes. Recognized output is at
most 64KiB; headers and decoded dimensions must agree, with at most4096 per axis
and4Mi pixels. Range validation includes UTF-8 boundaries. Coverage always stays
partial with reason `ocr_recognition_uncertain`, including full recognized text.
Provenance reports original pixel dimensions, Tesseract confidence0–100, owning
locator and engine/decoder/model/contract fingerprint. Confidence is the native
engine's score, not a calibrated probability or guarantee of accuracy.

Host configuration explicitly supplies regular artifact files through
SourcesOcrConfig or --tesseract-library, --leptonica-library, --ocr-traineddata
and optional --ocr-language (default eng). Requests cannot supply those paths.
No model download or environment fallback occurs. OCR absent leaves other
readers usable. Engine/decoder files are capped128MiB each; traineddata32MiB.
Fingerprints cover those explicitly configured artifacts, not transitive libraries.

Reads share owner-scoped evidence caching and durable operation_id,
parent_operation_id and timeout_ms semantics. Original snapshot integrity and
configured artifact fingerprints are checked before cache reuse. Failed reads
publish no successful evidence. OCR uses an owning request monitor and an owned
native helper with a30s maximum, shortened by an earlier deadline. Public new
extraction requires the helper; native init/decode may be killed without waiting
for a cooperative return. See docs/native-worker-v1.md for process/protocol
ownership and remaining resource qualification.

Image/PDF acceptance, real recognition cancellation/deadlines, artifact changes
and helper-failure cache preservation pass in bounded native checks. Resource,
broader raster corpus and independent releases remain required to close S17a.
