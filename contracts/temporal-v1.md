# Sources temporal observation contract, version1

`analyze_video` implements native prepared-frame sampling, public Inference
validation and retained observation/cache replay. Actual macOS model integration
passes at16 frames and selected late ranges; native lifecycle and regression
gates pass normal/sanitized on macOS/Linux. Area5 closure and platform availability
are recorded in the architecture verification document; release acceptance is7.

Public input: `contract_version=1`, `op=analyze_video`, matching immutable
`source_id`/`version`, `start_ms`, exclusive `end_ms`, and optional UTF-8 `prompt`.
Tracked requests support operation/parent identities and timeout up to120000ms.
The source must be owned encoded media; the selected range must fit its declared
video duration. The default prompt asks for changes across the sampled frames.

Sources retains the native uniform endpoint policy, at most16 frames and bounded
2fps sampling density. Interior selections decode the first available frame at
or after their target. The final selection decodes the final presentation frame
inside the last30seconds of the selected range. Measured duplicate timestamps
collapse; decreasing timestamps refuse. The final-frame preparation has its own
`visual-media-last-frame-v1` identity and remains distinguishable in the receipt.

The host selects `SourcesConfig.temporal_inference` independently of audio and
single-frame visual providers. A native process transport uses the explicit
`temporal_frames=1` profile. No Python runtime is required or shipped.
CLI hosts configure `--temporal-inference-executable`, an owner-only arguments
file, provider identity and runtime/model pins, using the corresponding
`--temporal-inference-*` flags. The host-private temporal transport alone permits
120CPU seconds and8GiB Linux virtual address space for declared model residency;
other native workers retain32CPU seconds/2GiB. Worker RSS, file, core, response,
deadline and requester-death controls remain bounded.

`sources_temporal_input` validates1–16 prepared visual bodies using the existing
media frame validator, including pixel/P6 hashes, immutable source identity,
decoder fingerprint, preparation recipe and actual source timestamp. Frames
must belong to the same source, have strictly increasing timestamps, and remain
inside the selected source range (at most4hours). Each prepared selection keeps
the existing30second bound. Alpha is composited on white by the declared public
model adapter. Source paths and decoder-private metadata do not cross the public
Inference boundary.

The resulting `analyze_video_frames` DTO carries absolute source timestamps in
microseconds, dimensions and canonical RGBA base64, a bounded UTF-8 prompt,
128output tokens and a120second timeout. Source independently computes the
public `molmo2-temporal-rgba8-v1` canonical input hash and ordered prepared-P6
hash. Public transport bounds are24MiB request and256KiB response; this client
accepts a temporal response of at most64KiB.

`sources_temporal_response` checks every returned frame's measured timestamp,
dimensions and pixel/P6 hashes, selected range, model/runtime pins, manifest
compound identity (`SHA256("molmo2-package-v1:" + manifest_sha256)`), group hash,
bounded text/token usage, alpha/timestamp policies and partial uncertain sample
coverage. A mismatch refuses the entire response. An observation remains derived
model output; sampled frames do not establish exhaustive video coverage.

Captures retain source range, prompt, model output and exact public Inference
receipt, plus ordered per-frame hashes, decoder metadata, preparation identities
and measured timestamps. Raw frames/base64 are not persisted in the observation.
The cache binds owner, source, range, recipe, prompt and model/runtime pins;
tracked operation identity also binds the temporal provider/pins. Offline cache
reuse validates the retained owned snapshot, capture digest, decoder metadata,
frame preparation identities, compound input/prepared hashes and public model
receipt without reopening the origin or loading a model/decoder. Captures share
the existing128entry/8MiB Source artifact budget and transaction conflict checks.

The declared actual model dependency is native Molmo2 MLX on macOS. Linux can
build and qualify Sources/Inference C transports with controlled fixtures; that
does not constitute an actual Linux MLX model run.
