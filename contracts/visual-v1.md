# Sources visual observation contract v1

`analyze_visual` reuses `prepare_visual` and a host-selected public Inference
provider. It analyzes one selected frame from an immutable owned encoded-media
snapshot. It does not scan every frame in the requested window. Full visual
index discovery/reindex/publication remains separate, in progress.

Requests require contract_version=1, source_id, version, start_ms and end_ms.
Version must equal the immutable source identity. Windows are nonempty, at most
30000ms, within the existing 14400000ms media ceiling. Optional prompt is nonempty
UTF-8, at most4096 bytes; default is “Describe the scene and visible text in this
frame.” The public request still has the16384-byte transport ceiling. Unknown
fields refuse. Engine paths, arguments, hashes and provider configuration belong
to the host. Optional operation_id/parent_operation_id/timeout_ms use the existing
durable control contract; parents require an operation ID.

The library's separate `SourcesConfig.visual_inference` uses the existing
SourcesInferenceConfig callback/control type. Audio and visual providers may
coexist. The CLI accepts --visual-inference-executable,
--visual-inference-arguments-file, --visual-inference-provider-id,
--visual-inference-engine-sha256 and --visual-inference-model-sha256. The argument
file has the existing owner-private regular-file/8192-byte bound and contains a
JSON argv array. The supplied transport uses the existing isolated native worker
and public Inference executable; it invokes no shell and reads no private
Inference database. Provider and engine/model pins are mandatory. Omitting the
executable/arguments pair permits reading captures with the three identity flags.

Preparation validates the owned snapshot, decoder identity, dimensions, color,
orientation, selected source timestamp and canonical packed RGBA payload through
the existing media worker and parent validator. The same validator computes
SHA256 of the exact RGBA and of the public Inference P6/white-alpha preparation.
The Inference request is analyze_image, with those pixels/dimensions, the selected
prompt and an output budget of128 tokens. One scoped deadline/cancellation
control spans preparation, transport, response validation and cache publication;
default attempt budget is60s, and public timeout_ms can reduce it.

The response must match contract, provider, input hash, dimensions, derived
visual_observation type, engine/model pins, independently computed PPM hash,
white-alpha policy and input_pixels coordinate space. Text is nonempty UTF-8,
at most16384 bytes; total accepted response/capture is at most65536 bytes. Usage
is bounded131072 input/128 output tokens. Coverage must remain
partial/visual_analysis_uncertain. Provider failures map to typed
inference_unavailable/inference_failed/inference_busy or the owning request's
cancelled/deadline_exceeded. Failures return no derived artifact.

Success retains source_id/version, artifact_id, kind=derived,
artifact_type=visual_observation and text. `frame` records the actual selected
timestamp_us in source_time_us, width and height. `range` remains the original
source_time_ms selection; it is not a claim of continuous observation.
Coverage explicitly includes sampled_frames=1 and
temporal_scope=selected_frame_only. Provenance retains prepared_artifact_id,
input/PPM hashes, provider/engine/model identities, prompt, complete validated
decoder metadata and the original preparation provenance/locator. No literal
evidence_id is generated, and packed pixel data is not duplicated in the capture.

The artifact identity binds source, prepared artifact, pixels, PPM, provider,
engine/model, prompt and exact decoder/preparation metadata. The owner-scoped
cache reuses the existing prepared_artifacts table and shared128-entry/8MiB
budget. Hash and capture validation precede cache reuse; model/prompt or decoder
changes select a different identity. Quota is checked before a model call.
Publication rechecks under the existing immediate transaction, refuses competing
different output and rolls back on control failure. Capture validation also
reuses the media metadata validator, including timing, scale, orientation and
HDR/color invariants. This is integrity validation, not model accuracy proof.

Cached calls still validate the snapshot and current prepared frame/decoder.
They can work with the original live file and model transport absent while the
configured media worker remains available. Exact completed tracked retries
replay before preparation, provider configuration or original-file access, even
with all adapters absent. Unknown/foreign owners refuse before model execution.
The private queue-window entry point uses transient frame/capture buffers and
can reuse prior cache entries, but inserts neither new pixels nor new captures.

Native `make test-visual sanitize-visual MEDIA_HELPER=... MEDIA_FIXTURE=...`
checks use actual decoded video frames and a controlled public Inference callback.
The independent test-visual-real tools accept Sources CLI, media worker, public
Inference CLI, native VisionPsy engine, model directory and an encoded heading
fixture. The installed macOS BF16 checkpoint recognizes the established heading
through this full pipeline, with exact source timing and offline durable replay.
It repeats the heading: this bounded runtime gate does not establish general
captioning/VQA accuracy. The sanitized gate instruments Sources, worker transport,
public Inference and test harness; the reused native MLX engine itself is not a
sanitized rebuild. Linux callback/native-frame gates do not claim MLX availability.

## Video timeline inspection

inspect_visual accepts contract_version, op, source_id and version, plus the
existing optional tracked-operation controls. It requires an owned encoded-media
snapshot and the configured isolated media worker, but no visual model. The same
bounded native FFmpeg container/stream probe used by timed frame decoding reports
the first supported video stream, source dimensions, origin_us and duration_ms.
The parent validates the exact seven-field worker DTO, four-hour duration bound,
eight-stream bound, 4096-axis/four-million-pixel bounds, signed origin and current
extractor fingerprint before returning visual_timeline. Duration uses
container_floor_ms; it is container metadata, not decoded-sample coverage.
Inspection reports partial/media_inspection_only and contains no pixel payload.
A tracked completed inspection can replay with its worker absent. Malformed,
unsupported or unknown timelines refuse; cancellation/deadlines remain scoped.
Durable visual indexing/admission and SDK publication remain separate open work.

## Publishing retained visual observations

publish_visual accepts the same source/version, selected start_ms/end_ms and
optional prompt as analyze_visual, plus the existing durable operation controls.
It requires the host's Chutni write grant and configured store/library/helper,
root and visual provider identity. It never invokes a model: the requested
capture must already exist for that owner/frame/prompt/model identity. A miss
returns inference_unavailable. Preparation and cache validation still require
the media worker for an untracked call; completed tracked retries replay before
those dependencies are consulted.

The shared derived-artifact writer validates the entire capture and owned source
snapshot again, then uses the existing isolated public Chutni SDK publication,
stable operation identity, bounded4096 shared receipts and commit recovery.
Visual operation keys use sources-visual-v1 and Chutni stores image_caption,
model_generated, producer samosa-sources-visual-v1, with public model identity
and the complete Sources visual_observation in sources_capture parameters.
The SDK selector identifies the selected frame as a one-microsecond time_range
[timestamp_us,timestamp_us+1), coordinate_space source_time_us. This denotes a
sampled instant, not inspected motion or the complete preparation window; the
original window, decoder/preparation provenance and explicit uncertain
single-frame coverage remain in the retained capture.

SDK conflict checks require the exact caption kind, producer/model, source hash,
selected timestamp and capture. SDK-committed artifacts recover when the local
receipt is absent; a later saved receipt replays without another SDK write.
Owner claims, model/prompt identities and cache/capture corruption checks apply.
Public memory_read can read the retained artifact and its full provenance with
the original file and visual adapters absent, using the host-granted SDK store.
The private source_visual_publish_capture entry uses the same writer for owned
queue captures, including transient frames. Durable visual queue admission,
discovery/reindex and their complete lifecycle qualification remain open.

## Durable sampled-frame jobs

visual_index_create accepts source_id/version and an exclusive start_ms/end_ms
range within the owned video timeline. visual_index_get/visual_index_step accept
job_id; visual_index_queue_step has no selector; visual_index_read accepts job_id
and captured ordinal. Existing tracked-operation/parent/timeout controls apply.
Unknown operations/fields refuse. The shared queue engine validates the selected
video stream and actual bounded container duration before admission. Identity
binds owner, root scope, source snapshot, decoder fingerprint, visual provider/
engine/model pins, default prompt,128-token budget,512-axis preparation and the
30-second sampling recipe. Changing configuration cannot silently resume a job
with different derivation semantics.

Each job plans one selected native frame per contiguous30-second window, up to
480 windows in a four-hour range. A frame is an instant: status always reports
partial/sampled_visual_frames and each observation retains selected_frame_only
coverage. This sampling does not qualify continuous motion or whole-video visual
understanding. Unavailable windows are durable errors with bounded retry backoff,
not fabricated observations. The default prompt is fixed by this recipe.

Audio/visual queues instantiate one media_index_impl.h implementation with fixed
modality callbacks/namespaces. Their existing audio identities remain stable.
Both share eight retained job slots and8MiB retained captures, preserving the
component's bounded database budget. Visual preparation/capture uses private
transient buffers and does not insert or evict public prepared/capture caches.
Capture and publication commit separately under the existing index lease; when
the host grants publication, a saved capture must publish before the next capture.
Publication uses the distinct caption writer and shared bounded SDK receipts.
A peer/model failure preserves committed observations, records durable failure/
retry state and backs off up to60s. The runtime executes at most one visual step
per cycle using scoped cancellation/deadlines, alongside the existing audio and
index paths. It honors TERM through the existing stop control.

A crash after SDK commit recovers the stable external artifact without re-running
the model, even with the origin absent. Retained reads validate owner, scope,
snapshot, frozen recipe, complete capture and receipt digest/proof; model transport
can be absent, but identity/decoder validation still requires host configuration.
SDK memory_read remains the fully adapter-independent retained artifact path.
Automatic visual discovery/reindex association, version history and derived
candidate retrieval use the shared association implementation below. Platform
qualification is recorded in the architecture rebuild verification log.

## Discovery, replacement and retained candidates

Index discovery recognizes the supported video container extensions and prepares
an owned encoded snapshot through the same register_media path as public callers.
inspect_visual supplies the bounded container duration. Admission into
index_visual and the visual queue commits with the index item; missing model
configuration rolls back admission, leaving the item pending for retry. Captions
remain derived observations and never count as complete literal text coverage.
index_get reports queued_visual_items, completed_visual_items and visual_jobs;
a fully visited inventory waits with waiting_visual until its current jobs finish.

Committed freshness observations include queued_visual items. Reindexing registers
and probes the changed owned snapshot, validates the observation again inside the
replacement transaction, and retains the previous association in
index_visual_history before switching to the new job. Audio and visual history
share the existing 256-association bound. A failed or killed replacement preserves
the last committed association and capture. The global index lease, observation
revision checks, operation recovery and queue retries are shared with audio.

index_visual_history accepts job_id and optional after; each page returns at most
16 previous associations with a row cursor and has_more. index_visual_read accepts
job_id, visual_job_id and ordinal. Owner/scope checks precede the complete retained
snapshot/capture validation. The origin and model transport may be absent.
index_visual_search accepts job_id and query and searches current and historical
retained captions using the existing bounded ASCII case-insensitive substring
matcher. It returns at most ten candidates, preserves derived provenance and
selected-frame coverage, and reports uncaptured windows and live observations.
This is retained-caption lexical retrieval; semantic retrieval and temporal video
analysis remain separate required work.
