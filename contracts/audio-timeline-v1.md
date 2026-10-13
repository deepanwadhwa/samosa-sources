# Audio timeline inspection, contract1

```json
{"contract_version":1,"op":"inspect_audio","source_id":"<sha256>","version":"<same sha256>"}
```

Inspect an owner-authorized immutable register_audio/register_media or upload
snapshot. The original file/root is not needed. Unknown owners/sources refuse;
version disagreement returns stale_source. Extra fields refuse. Existing optional
operation_id, parent_operation_id and timeout_ms controls apply, including exact
durable replay without the media worker. Bare encoded inspection needs the
host-configured media worker each time; it has no persistent timeline cache.

The response has kind:derived, artifact_type:audio_timeline, source_id/version,
frames, sample_rate:16000, duration_ms, coordinate_space:source_time_ms,
duration_policy and reader/decoder fingerprint provenance. It contains no waveform,
transcript, literal evidence_id or recognition claim. Frames count the usable
mono16k output timeline. Duration is floor(frames/16); any sub-millisecond tail is
reported by frames but cannot be selected through integer-millisecond preparation.

Plain WAV uses the existing validated PCM16/mono16k parser and
pcm_samples_floor_ms. Encoded audio uses the existing isolated FFmpeg decoder,
stream selection, timestamp continuity checks and resampler. It scans to EOF and
flushes the resampler, discards output PCM, and reports decoded_samples_floor_ms.
The nested decoder DTO records stream, source rate/channels, origin_us, frames,
duration, sample_rate, duration policy and configured library fingerprint. The
parent validates its fields/ranges and binds it to the expected fingerprint.

This adapts the monolith attachment_audio_decode_probe_path/native --probe
workflow to Sources-owned snapshots and the already extracted platform decoder.
A metadata-only duration is insufficient for exact window admission because
compressed containers can include encoder padding. No new media engine is added.
Prepared/transcript cache retention and quota behavior are unchanged.

Existing bounds apply:64MiB input, at most8 streams, supported WAV/MP3/MOV audio
codecs,8–192kHz and1–8 channels,1000000 inspection packets,65536 samples per decoded frame,
1MiB packet payload,4-hour timeline and a30s worker deadline (earlier caller budget
wins). Secondary media/network references refuse. Timeline gaps, unsupported
format changes, malformed data, capacity/deadline and cancellation remain typed.
The four-hour bound is an admission ceiling, not a promise that every four-hour
file completes within packet/deadline limits. This full scan is not a seek index.
Selected preparation keeps its50000-packet bound and seeks over unrelated earlier
media. Full inspection necessarily reads the entire relevant timeline; its separate
finite packet allowance supports qualified long sources without retaining PCM or
relaxing the worker's deadline, cancellation or memory limits.
Full-scan workers refresh parent cancellation at25ms intervals instead of making
multiple synchronous control/store calls per packet. Local deadline checks still
run at each interruption point; parent death retains the existing watchdog.

Automatic filesystem audio discovery/reindex/freshness, durable duration reuse,
job/history removal and complete component release remain pending.
Inspection does not itself create an audio job or perform Inference/publication.

Qualified subset: native WAV/MP3/AAC/48kHz stereo fixtures and public WAV/MP3
2672ms endpoint/owner/immutable/durable-replay checks pass normal and ASan/UBSan
on macOS/native ARM64 Linux. Existing worker tests also exercise the new probe
request through faults, active cancellation/deadline, RSS bounds, peer survival,
reaping and requester SIGKILL. External codec libraries remain configured
dependencies. The short fixtures are not a speech accuracy benchmark. A compact
nearly-four-hour AAC source additionally qualifies full usable sample counts
against native decoding, late public preparation/cache and exact offline durable
inspection/preparation replay. It does not benchmark recognition or every bitrate/
sample-rate combination.
AAC padding retained by a decoder is counted in its decoded timeline; the policy
does not assert an unpadded recording duration or require identical sample counts
across different decoder versions.
