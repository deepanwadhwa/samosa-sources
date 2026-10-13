# Retained evidence summaries v1

`summarize_evidence` uses an existing owned, cached literal evidence capture as
its input. First use the qualified public reader for the desired immutable source
and selector, then pass its `evidence_id`, `source_id` and equal `version`:

```json
{"contract_version":1,"op":"summarize_evidence","source_id":"SOURCE_SHA256","version":"SOURCE_SHA256","evidence_id":"EVIDENCE_SHA256","operation_id":"summary-1","timeout_ms":60000}
```

The host configures a separate `summary_inference` transport with fixed provider,
engine and model pins. CLI equivalents are `--summary-inference-executable`,
`--summary-inference-arguments-file`, `--summary-inference-provider-id`,
`--summary-inference-engine-sha256` and `--summary-inference-model-sha256`. The
arguments file uses the existing private host JSON-array rules. A host may supply
pins without a transport for retained-cache reads. Public requests cannot supply
text, change prompts, budgets, models, provider pins or executable arguments.
Existing Sources owner scoping, operation correlation, cancellation, ingress
budgets and durable replay apply. Operation replay returns its original captured
outcome, including original provenance, rather than rerunning a changed model.

Before model use or cache reuse, Sources verifies the owned immutable snapshot,
literal cache digest, source/version, reader/fingerprint and evidence selector
identity. It does not reopen the origin or load the extractor. Captures use the
existing qualified text, HTML, DOCX, image-OCR, PDF-text and PDF-OCR envelope
shapes. Text is sampled from the opening of the selected capture. PDF page text
is joined in retained order with newlines. The scalar sample is at most 1,400
UTF-8 bytes, trimmed at codepoint boundaries, with 128 output tokens. Empty input
refuses. This reuses the monolith engine and its bounded opening/chunk approach;
it does not reproduce the monolith's larger batched enrichment recipe.

The public Inference response must bind the exact sampled bytes and their hash,
configured provider and engine/model pins, scalar recipe, output budget, encoder
context and partial/uncertain coverage. Empty, malformed, literal-labelled,
wrong-input/model/recipe and unsupported consumption claims refuse. Provider
busy, cancellation and deadlines remain observable. Unavailable transport does
not cause a fallback to another model.

A summary is a `derived` artifact, never literal evidence. Its artifact identity
binds source, evidence ID, the full evidence-envelope digest, sampled-input digest
and summary recipe/model pins. `input_selector` and `input_coverage` retain the
original evidence coordinates and coverage. Provenance records sampled bytes,
reader/extractor, evidence ID and hashes, engine/model and provider pins. Coverage
is always partial with reason `opening_sample_and_generated_summary_uncertain`;
processed input token count remains null because the inherited protocol supplies
no consumption receipt. The recipe is
`samosa-native-summary-opening-chunk-v1`, deliberately distinct from the
monolith's multi-chunk `samosa-native-summary-opening-sample-v3`.

Summaries use the existing owner-bound prepared-artifact cache and its shared
literal/prepared count and byte quotas. Retained entries require their stored
digest and canonical full envelope to match the current input/model recipe.
Model changes create new artifact identities. Cache reuse works without the
origin and with the model transport disabled. No new eviction policy is added.

`make test-summary sanitize-summary` checks Sources response validation,
UTF-8/empty sampling, ownership, quotas, replay/cache, cancellation/deadline and
snapshot/evidence/summary corruption. The fixture model does not qualify model
quality. `build/test-summary-real` and its sanitizer variant take absolute paths
to the Sources native worker, public Inference summary adapter, native T5 engine
and private T5 model. They exercise actual Sources -> public Inference -> the
inherited model, provenance and retained cache. The actual model engine remains
separately packaged and is not sanitizer-built by these targets.

`summarize_evidence_opening` accepts the same evidence identity fields and uses
up to 60,000 opening UTF-8 bytes through public Inference `summarize_opening`.
It reuses the monolith native tokenizer partitioning and shrinking map/reduce
policy. Sources validates and retains the map byte count, reduction rounds,
64-token chunk budget, 1,800-byte output cap and explicit truncation flag.
Its recipe is `samosa-native-summary-opening-map-reduce-v1`; scalar and opening
artifacts have separate identities and caches.

`publish_summary` accepts the same source/version/evidence fields. It requires
an already cached opening summary and never invokes the model. The host must
enable Chutni publication and pin its SDK, store identity and worker. Publication
uses the existing derived-artifact receipt machinery and public SDK
`put_file_outputs`, with artifact kind `summary_short`, producer
`samosa-sources-summary-v1`, original byte/page selector, model pins and complete
Sources capture in parameters. Parent and worker validate the capture. Stable
SDK operations recover a committed artifact when the local receipt is lost,
including after origin deletion. Initial publication still needs a root-relative
file locator; uploaded-only sources refuse without such a locator.

`make test-summary-publication sanitize-summary-publication CHUTNI_LIBRARY=...`
exercises the actual SDK with a fixture model, cache-only publication, repeated
receipt reads and SDK recovery after receipt loss and origin deletion.
Automatic indexing/enrichment scheduling, embeddings and representation/query
generation remain area 4 integration work. These APIs do not close area 4.
