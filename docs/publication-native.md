# Native publication acceptance

`make test-publication sanitize-publication` uses a standalone native C client,
actual Sources CLI processes and the public Chutni SDK shared library. Configure
CHUTNI_LIBRARY explicitly. PDFIUM_LIBRARY enables the HTML/DOCX/PDF matrix;
OCR_FIXTURE, TESSERACT_LIBRARY, LEPTONICA_LIBRARY and OCR_TRAINEDDATA enable actual
OCR. Missing optional dependencies do not establish those matrix passes.

The native replacement preserves the former publication driver's cases:

- Index/publication, committed and pending checkpoints, exact model-free receipt
  replay, idle publication without duplicates, SDK literal provenance, lexical
  retrieval and artifact readback.
- Foreign owner, disabled publication and changed-store refusal.
- Real post-SDK/pre-receipt SIGKILL; changed live bytes; cold interrupted-operation
  recovery; forged committed content refusal, repair and retained recovery without
  another artifact; subsequent version publication.
- Headless index runtime publication of a bounded UTF-8 prefix, exact original
  byte comparison, partial coverage and graceful runtime stop.
- Actual HTML/deflated DOCX/PDF indexing, extracted coordinates, snapshot versions,
  script exclusion, absolute pages and empty text layer; publication and memory
  readback with preserved evidence; reconciliation/reindex, retained search history
  and generation-one publication.
- Actual image OCR, method/confidence/original raster and uncertain coverage;
  publication/readback and offline replay; mutated SDK recognition provenance
  refusal, repaired retained recovery and encoded-image reindex history.

The current PDF reader adds a distinct text-layer-with-OCR-fallback artifact when
OCR becomes available. The matrix verifies exactly one fallback artifact with one
OCR page, its partial coverage and the two retained text-only versions. Artifact
counts include that explicitly verified profile; repeated publication still must
leave counts unchanged. The older Python count predates this implemented reader.

Catalog corruption is fixture-only and uses the catalog SDK's own SQLite symbols.
Production Sources accesses Chutni through its public SDK boundary. Native fixture
programs generate DOCX/PDF; the client resolves OCR dependency symlinks, bounds
subprocess waits and retains ownership of children for failure cleanup. Neither
build commands nor this test require a Python runtime.
