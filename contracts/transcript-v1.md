# Derived audio transcript v1

`transcribe_audio` accepts source_id, version, start_ms and end_ms, plus the existing
optional operation_id, parent_operation_id and timeout_ms controls. It first uses
the existing `prepare_audio` path: immutable owner-authorized bytes, a nonempty
window of at most30000ms, and the original source timeline bounded to4h.

The library host supplies SourcesInferenceConfig with provider_id, engine_sha256,
model_sha256 and an optional public Inference call callback. Configuration is
trusted host input, not request parameters. The callback receives public JSON
`transcribe`, canonical WAV base64, provider_id and timeout_ms. It must obey the
supplied monotonic deadline/cancellation control and own transport/process cleanup.
Its returned response is malloc-owned, NUL-terminated and length excludes the NUL.
Sources does not load model files or call an engine directly. With matching
identity configuration, qualified cache hits work with the callback absent.
The supplied SourcesInferenceProcess adapter uses the existing isolated Sources
worker to launch a host-selected public Inference executable. It sends one public
JSON request on stdin and reads one response on stdout, without a shell. Arguments
are an array of at most64 strings, each at most4095 bytes. Response bytes are
bounded262144; embedded NUL, oversized output and abnormal exits refuse capture.
Provider exit0 or1 permits a public success/error envelope. Cancellation and the
earlier deadline stop work; the enclosing worker supervises parent death and kills
its process group. Inference owns cleanup of its separately supervised engines.

CLI configuration uses --inference-executable, --inference-arguments-file,
--inference-provider-id, --inference-engine-sha256 and --inference-model-sha256.
The arguments file is an owner-private regular file, at most8192 bytes, containing
the argument JSON array (excluding argv0). It may configure Inference's engine,
model and admission state; those settings are not Source request fields. With
only the three identity flags and no executable/arguments file, the CLI can serve
qualified cached transcripts without a provider transport.

The current consumer supports English whisper.cpp results. It checks contract
version, provider, engine/model pins, exact prepared input SHA256 and duration,
derived transcript type, partial/speech_recognition_uncertain coverage, and every
cue. Responses are bounded262144 bytes, text16384 bytes and128 cues. Cue text must
be valid nonempty UTF-8; cue ranges must be nonempty, ordered by start and inside
the prepared duration. Joined cue text must equal the response text. Relative
cue timestamps are shifted by start_ms into absolute source_time_ms. No literal
evidence_id is created. Output records source/version, selected range, prepared
artifact/input identities, provider/engine/model pins and recognition derivation.
The optional recognized timestamp_policy is validated and preserved in capture
and cache provenance. Currently the accepted policy is
whisper_20ms_end_clipped_to_input; it permits Inference to clip only Whisper's
first20ms grid boundary at the input end. Sources still requires every returned
cue to lie entirely within the prepared duration and original selected range.

Artifact identity binds source, prepared artifact, range and provider/engine/model
pins. Cache keys additionally bind owner. Transcript entries share the existing
128-entry/8MiB response budget. Original snapshot and prepared payload integrity
are checked before cache reuse. Digest and full saved transcript semantics are
revalidated; altered entries return corrupt_cache. Model changes select a distinct
artifact. Concurrent duplicate capture compares qualified bytes rather than
overwriting them. Failed/cancelled capture publishes no transcript, although its
successfully completed prepared-audio dependency may already be cached.

Durable operation retries preserve the original result, including offline replay.
Bare cache lookup requires the same configured provider/engine/model identities.
The default transcription deadline is60s; an earlier caller deadline wins.
Provider busy, cancellation and deadline errors remain typed; malformed or
identity-mismatched results refuse publication. The bounded JSON parser admits
2048 tokens so the complete128-cue result fits its durable operation envelope;
the public request byte bound remains16KiB.

Native C fixture tests qualify capture, absolute timing, all128 cues, model-change
identity, offline ownership/replay, quota, cancellation and both digest and
recomputed-digest cache tampering on macOS/Linux with ASan/UBSan. These tests use
a fixture provider. Actual public Sources preparation -> installed Whisper was
qualified separately on macOS. Full Sources capture -> process transport -> actual
Whisper, exact input digest and transport-absent offline cache reuse now also pass
for WAV and MP3 on macOS/native ARM64 Linux in normal and sanitizer modes. The
MP3 pipeline compares prepared PCM against an independent platform-native decode;
its media helper remains configured during cache lookup. Broader speech/media
quality remains open.
`publish_transcript` publishes an already captured window through the existing
Chutni SDK without rerunning Inference; see [the Chutni contract](chutni-v1.md).
Actual Whisper-to-publication/readback now passes for the qualified WAV and MP3
fixtures on both platforms, including sanitizers. Automatic audio publication
queues, broader speech qualification and visual analysis remain pending.

The private queue window path now reuses the same prepared-audio validation,
public Inference transport and transcript projection with bounded working buffers.
It may read qualified existing caches, but inserts neither prepared PCM nor the
new capture into them. The queue caller owns the returned capture and must commit
it to its own durable checkpoint before advancing. Public prepare_audio and
transcribe_audio keep their existing persistent cache and quota behavior.

The publication core also accepts a Sources-owned validated capture, sharing the
public cached-window writer's owner/snapshot checks, SDK transaction, recovery and
durable receipt handling. This is a private component entry point; public requests
cannot submit captured text or metadata. Window planning adapts the monolith's
one-second overlap and ceiling-stride tail accounting to the30s public limit.
Durable selected-range audio job state and integration with the existing queue
runtime are now implemented; see [audio indexing](audio-index-v1.md). Broader
filesystem discovery/freshness remains open. Actual Whisper WAV/MP3 queue capture,
publication and offline saved-capture readback pass normal and ASan/UBSan on both
platforms for the short English fixture; broader media/recognition coverage remains open.
