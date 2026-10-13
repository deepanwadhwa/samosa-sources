# Encoded media registration v1 — S18b in progress

`register_media` accepts an authorized relative `path` and optional inventory
`expected_file_identity`, using the same immutable snapshot, byte identity,
owner-scoped claim and locator rules as the other registration operations.
Current originals are bounded to 64MiB; resumable upload lifecycle remains required.

Admission recognizes candidate MPEG/ADTS audio headers (ID3 or sync), MP4/
QuickTime initial box types (ftyp, moov, mdat, wide), FLAC/Ogg signatures,
RIFF/WAVE and FORM/AIFF/AIFC headers. This is container candidate
admission, not a claim that the bytes decode, have audio, or cover a selected
timeline. Native decoding later validates actual codec and time coverage.
Registration requires no codec executable, SDK or model. Internal owner claim
type is `application/x-samosa-encoded-media`; the bytes are not literal text or
a prepared artifact. A second owner must independently register its authorized
locator to acquire its own claim.

The audio worker admits WAV, MP3, MOV/MP4, ADTS AAC, FLAC, Ogg and AIFF containers
and the bounded MP3/AAC/FLAC/Vorbis/Opus/declared PCM codec subset. Existing
memory-only input, secondary-open refusal, thread/stream/frame/packet/time bounds
and exact host-selected worker fingerprint validation apply to every container.

Optional worker audio decode, verified fingerprints and parent DTO validation
are implemented. `prepare_audio` accepts the registered encoded-media owner claim,
queries the configured decoder fingerprint before cache lookup, and decodes only
on a miss. Output remains derived PCM16 mono16000Hz WAV with original selected
milliseconds and local zero-based timeline. Additional `decoder` metadata reports
stream index, original sample rate/channels, container origin in microseconds,
selected bounds, output byte count and fingerprint. Provenance uses
`audio-media-window-v1` / `native_decode_resample`; no transcript/evidence is implied.

Owner-scoped cache identity binds source SHA, selection and decoder fingerprint.
Shared literal/derived capacity applies. Cached metadata and canonical base64/WAV
are validated on reuse; newly built responses are validated before publication.
Normal cache access requires the configured worker to query its current artifacts.
Saved durable operation retries can return the exact recorded result with worker
and original absent; a changed range under that operation ID conflicts.
Timed visual preparation is specified below. Complete upload/freshness lifecycle,
Inference transcription/analysis and final Sources release are separate work areas.

## Timed visual preparation

`prepare_visual` accepts `source_id`, equal immutable `version`, and exclusive
`start_ms`/`end_ms` bounds, with the same optional durable `operation_id`,
`parent_operation_id` and `timeout_ms` controls as audio preparation. The owner
must hold an encoded-media claim. A WAV-only claim does not authorize this reader.
Selection is the first decoded video frame whose source timestamp falls inside
the interval. It does not promise interval-wide visual coverage. Current bounds
are 30 seconds per selection and a four-hour source timeline; bounded originals
remain 64MiB. Unsupported/uncovered inputs return typed errors without artifacts.

The result uses `kind: derived`, `artifact_type: prepared_visual`,
`media_type: application/x-samosa-rgba` and canonical base64 data. `decoder`
metadata specifies `pixel_format: rgba8`, width/height, original source dimensions,
packed RGBA byte count, source `timestamp_us`, container `origin_us`, selected
stream, conversion matrix/range, selected bounds and decoder fingerprint. Packed
rows have `width * 4` bytes. The largest axis is at most 512 pixels, with aspect
ratio preserved by integer scaling; the parent validates that exact relationship.
`range` retains the original source milliseconds. There is no local audio timeline.
Coverage remains `partial` / `media_preparation_only`; provenance uses
`visual-media-frame-v1` / `native_decode_scale_rgba` and the owner's registered
locator. Prepared pixels are not literal evidence, transcription or analysis.

Artifact identity binds the visual selection contract, source SHA, selected range
and actual decoder artifact fingerprint, and differs from audio for the same
source/range. Owner-scoped cache keys and the shared 128-entry/8MiB literal/derived
budget apply. Parent DTO validation runs before publication; cached responses
verify digest, identity, provenance and reconstructed frame DTO before reuse.
Failures preserve previous valid entries. Normal cache hits query current worker
artifacts; exact recorded durable retries can work with the worker absent.

The native corpus covers admitted video codecs, SDR/PQ/HLG color, orthogonal
orientation and source timelines through worker/public API/cache. Coverage and
resource limits remain explicit. Complete Sources release additionally requires
the other established upload, Inference, indexing and service work areas.

### Display orientation

The visual worker now applies exact orthogonal display transforms. It reads the
selected frame's display matrix when present, otherwise the selected stream's
matrix, through the platform's public FFmpeg API. Unit rotations/reflections are
supported; shear, non-unit affine scale and perspective return
`media_orientation_unsupported` without prepared pixels. Translation offsets are
normalized when rebasing the transformed frame's bounding box.

Packed RGBA is produced with bounded bilinear scaling and source color conversion
first; orientation then permutes those RGBA pixels without interpolation. This
order avoids rotating subsampled chroma before color conversion and is part of
the declared worker-v6 fingerprint contract. A native FFmpeg reference must use
`-noautorotate`, perform the declared scale/color/RGBA conversion, and then apply
the corresponding presentation filter to compare exact bytes. Default FFmpeg
autorotation can produce different chroma edge pixels because it transforms the
subsampled source first.

Required `decoder.orientation` identifies the applied transform:

| Code | Transform of the decoded image |
| --- | --- |
| 0 | Identity |
| 1 | Clockwise 90 degrees |
| 2 | 180 degrees |
| 3 | Counterclockwise 90 degrees |
| 4 | Horizontal reflection |
| 5 | Vertical reflection |
| 6 | Main-diagonal transpose |
| 7 | Opposite-diagonal transpose |

Codes 1, 3, 6 and 7 swap the scaled width and height. `source_width` and
`source_height` remain the untransformed decoded dimensions. Parent and cache DTO
validation require a valid orientation and the corresponding exact output shape.
Actual executing/loaded artifacts and the updated worker contract invalidate
prior normal cache identities when the configured decoder changes; saved durable
historical results retain their exact retry semantics.

### HDR transfer policy

PQ (transfer16) and HLG (transfer18) with BT.2020 primaries and BT.2020 nonconstant
luminance matrix use native libavfilter conversion to linear BT.709 RGB, Hable
tone mapping with fixed peak10 and desaturation0, then sRGB RGBA8. Other HDR
primaries/matrices return `media_visual_format_unsupported`. Optional mastering
and scene metadata do not change the fixed transformation. This prepares bounded
model input; it is not a calibrated display or photographic rendering service.

The qualified FFmpeg5.1 Linux SDK uses zscale with nominal peak luminance100;
FFmpeg9 macOS uses its native scale transfer/primaries options. Each platform is
compared against its explicitly configured native reference. Pixel equality
across different FFmpeg SDKs is not promised. SDKs without either conversion
path or the native tonemap filter return a typed unsupported error.

Required decoder fields `source_transfer`, `source_primaries`, `output_transfer`,
`output_primaries` and `tone_mapped` record the conversion. Numeric transfer and
primaries values follow FFmpeg's published enums. HDR output reports transfer13
(sRGB), primaries1 (BT.709) and tone_mapped1; SDR reports retained source transfer
and primaries with tone_mapped0. The existing SDR matrix/range scaling policy is
preserved and does not claim general SDR gamut conversion. Parent and cache
validation require consistent fields. Worker-v6 identity includes libavfilter
and mapped libzimg when present, alongside the existing executing artifacts.

### Late visual selections

For start_ms at or beyond30000, the visual decoder uses native backward keyframe
seeking and decodes forward to the first frame inside the requested interval.
Earlier requests retain initial forward decoding. This preserves original source
timestamps, orientation and pixel policies while applying the50000-packet work
bound to the selected region rather than all preceding media. A failed seek is
`media_timeline_unsupported`; the existing frame, packet, interruption and worker
bounds still apply. Worker-v6 binds this selection method.

### Late audio selections

Encoded selections at start_ms>=30000 use native backward seeking with two
seconds of discarded preroll. The existing codec and libswresample perform the
decoding and conversion. At most one input/output sample-grid period of native
resampler silence aligns the original source grid; it occurs before the selected
window. Missing initial source time refuses, and selected samples are never filled
to cover an absent audio interval. Earlier requests retain initial forward decoding.
Selected decoding retains its50000-packet,30s window and existing worker bounds.

Native codec seek state can differ from decoding a whole source. AAC perceptual
noise substitution has decoder noise state; its seek output is qualified against
an independent native seek reference. PCM, MP3 and no-PNS AAC cases additionally
match sequential native-reference samples exactly. This method is part of the
worker-v6 fingerprint. Inspection remains a separate full scan, described in
audio-timeline-v1.md, and does not retain a full waveform.
