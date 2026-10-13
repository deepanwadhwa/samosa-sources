# Natural-language retrieval through owned Inference, version 1

`memory_search_text` accepts a nonempty UTF-8 `query` of at most 512 bytes and
ordinary Sources operation controls:

```json
{"contract_version":1,"op":"memory_search_text","query":"How are coastal birds protected?","operation_id":"text-search-1","timeout_ms":60000}
```

Sources calls public Inference `embed_text` with the query role, validates the
response, then delegates ranking to the existing public Chutni semantic search.
Public requests cannot choose the model, runtime, profile or store. Generated
vectors require an `l2` host semantic profile; dimensions remain bounded to 1024.
No Python runtime is required or shipped for this path.

The host provides `SourcesConfig.embedding`, the existing Chutni host settings,
and an Inference callback. The CLI uses these additional flags:

* `--embedding-inference-config-file ABS_PRIVATE_JSON`
* `--embedding-inference-executable ABS_EXECUTABLE`
* `--embedding-inference-arguments-file PRIVATE_JSON_ARGUMENT_ARRAY`

The configuration has exactly seven fields: `provider_id`, `engine_sha256`,
`model_sha256`, `recipe`, `query_prefix`, `document_prefix`, and `context_tokens`.
Digests are lowercase SHA-256. Context is an integer from 128 through 131072.
The private arguments file selects the owned Inference managed CLI with
`["--config","/absolute/private/managed.json","--single"]`.
The existing `--chutni-semantic-profile-file` supplies the complete representation
profile. The model recipe and prefixes must match that owned runtime's setup.

Sources independently verifies provider, runtime/model pins, complete profile,
role/title, operation identity, original and prepared input digests/byte counts,
token budget, finite f32 values and L2 norm. Changed or malformed responses
refuse before SDK search. Cancellation and deadlines propagate through the
native process transport; there is no alternate-model fallback.

The retained retrieval metadata includes the text query and validated embedding
receipt alongside the existing SDK query/profile/vector and ranked results.
Tracked operation identity binds the owner, model recipe/profile and store UUID.
Offline replay requires the same host binding but needs neither a callable
runtime nor the SDK. Changed binding refuses idempotent replay. Retained results
remain partial retrieval metadata rather than literal source evidence.

The internal document path now supports saved-capture validation and native
payload-checked publication. `sources_embedding_capture_arguments` revalidates
the complete document receipt against text, title and host binding without a
model call. `sources_native_chutni_embedding` verifies the selected SDK artifact's
complete text and payload hash before generation. A publication call requires
the host write grant and exact host semantic profile, checks the text and saved
payload binding again, then calls `put_representation_bound`. It uses only the
SDK's documented four-function ABI. Older SDKs refuse; there is no unchecked
write fallback. Document input is bounded to 60000 UTF-8 bytes and the configured
model token budget, with refusal instead of truncation.

`index_embed_step` selects an owned index job; `index_embed_queue_step` discovers
due retained generations after literal SDK publication. The host must grant
publication and configure the embedding provider/profile. The existing background
queue runs these steps automatically. New work selects the latest generation;
already admitted captures from older generations remain recoverable. Captures
and publication receipts commit as separate durable phases. Publication uses the
saved vector, rechecks its SDK text/payload binding, and never regenerates it.

`index_embedding_status` accepts `job_id` and an optional `after` cursor, returning
up to16 retained rows with generation, model binding, artifact/representation,
phase, receipt digests and retry state. Status requires the owned root grant,
but no callable model or loaded SDK. Archived rows validate their source
generation, scope/owner, operation identity, finite normalized vector and exact
SDK publication arguments even when the active profile has been retired.

Admission reserves capture/receipt space before model execution:4096 rows and
8MiB, including pending reservations. Retry delays are bounded to60 seconds.
Whole-document inputs over60000 bytes refuse explicitly; no silent clipping or
alternate model is selected. Chunking beyond the declared input bound is a
separate extension, not an implied capability.

`test-embedding` and `sanitize-embedding` cover SDK ranking with fixture vectors,
response drift, controls, owner isolation, native document binding/publication,
offline replay, retained reindex history, background execution and SIGKILL after
local capture or SDK commit. Rehashed cross-artifact/text/vector corruption is
refused. The native `test-embedding-real` QA gate starts with12 source files and
an empty SDK store, runs the actual background queue with pinned owned model
weights and an unchanged managed configuration, and checks12 paraphrases against
related distractors. It also exercises CLI queries, retained provenance and exact
offline replay. This qualifies that declared corpus and path, not arbitrary
retrieval accuracy or final release acceptance. All execution is native C/C++;
Samosa does not ship a Python runtime.
