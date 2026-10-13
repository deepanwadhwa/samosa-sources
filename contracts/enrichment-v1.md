# Retained index enrichment v1

`index_enrich_queue_step` selects one due enrichment phase from owned index jobs
in the current pinned root scope. `index_enrich_step` restricts selection to a
`job_id`. They take the standard contract version, operation correlation and
control fields. No caller-provided text, executable, model or prompt is accepted.

```json
{"contract_version":1,"op":"index_enrich_queue_step","operation_id":"enrichment-turn-1","timeout_ms":60000}
```

The host's separate summary Inference configuration enables enrichment. Missing
configuration returns `processed_items:0` with `enrichment_disabled` and preserves
literal indexing. The queue takes the existing one-at-a-time index lease. Work
comes from retained `index_items` and `index_versions`; new work selects the latest
generation, while already admitted older work retains its original evidence.
Discovery/reindex and model/store profile changes are detected on the next queue
step without a manual evidence read or a separate job-creation call.

Each durable identity binds owner, root scope, index job, ordinal, generation,
full literal-envelope digest, summary recipe and destination store. Snapshot
ownership, hash, reader and selector identities are checked before capture or
publication. The existing public opening-summary implementation captures up to
60,000 UTF-8 bytes through public Inference, with explicit reduction/truncation
and source coverage. Mixed PDF text/OCR captures retain the inherited compound
identity. Retained enrichment avoids duplicating literal/summary entries in the
public evidence/prepared cache; its own retained records are bounded to 4,096
and summary captures to 8 MiB. It reserves room for the maximum response before
model use. Empty input is recorded as terminal `empty` rather than retried.

Capture and publication are separate steps. A successful model result is stored
with its digest before publication. Publication uses the existing derived SDK
writer and receipts, with artifact kind `summary_short`. SDK failures and process
death after capture never require rerunning the model. Stable SDK operations
recover a commit made before its local receipt was recorded. Initial publication
still requires a root-relative file locator whose content matches the retained
source; changed/missing origins remain visible failures unless the SDK commit
already exists. Capturing without publication enabled records a completed summary.

The existing background index runtime advances one enrichment phase per cycle when
summary Inference is configured, using its pinned root/state, cancellation and
bounded deadline. TERM during model work leaves a retryable pending checkpoint.

Failed attempts retain their phase and a bounded retry delay, increasing to
60 seconds. Scope, configuration and corruption errors remain explicit. A failed
model call does not create a summary, and no alternate model is selected.

`index_enrichment_status` takes `job_id` and optional integer `after` (default0).
It returns up to four owner-authorized retained entries, including generation,
recipe, store, status, summary capture, publication receipt and retry metadata.
`next_after` is the last returned sequence for pagination. Old model profiles
remain visible and are validated against their captured pins.

`make test-enrichment sanitize-enrichment CHUTNI_LIBRARY=...` exercises actual
public SDK writes with a fixture model, capture/publication phases, SIGKILL after
SDK commit, origin/model-free recovery, quota independence, watch/reindex,
retained generations, HTML/DOCX extraction and SDK publication, excluded script/deleted
text, model/owner isolation, deadlines and corruption refusal.
Fixture embeddings/model output do not establish model quality. Broader reader
and runtime qualification, public embeddings and representation/query generation
remain area4 work; this API does not close area4.
