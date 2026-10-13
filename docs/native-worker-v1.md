# Owned native worker v1 — qualification in progress

Optional media worker request kind13 measures a complete usable audio timeline
through the existing decode/resample scan without retaining output PCM. Its nine
field private response includes fingerprint, stream, source_rate/source_channels,
origin_us, frames, duration_ms, sample_rate and duration_policy. The parent checks
shape, bounds, floor-millisecond accounting and the expected decoder fingerprint;
inspect_audio projects this through the owned snapshot API. The normal30s worker
budget and existing cancellation/process cleanup apply. See
[audio timeline inspection](../contracts/audio-timeline-v1.md).

Public uncached image OCR, PDF OCR and PDF text reads execute native libraries in
an owned Sources worker. The CLI resolves its executable (literal /proc/self/exe
on Linux, including when the original file was unlinked; canonical executable
path on macOS). A library host configures SourcesConfig.native_helper to a
compatible Sources executable with the worker entry point. Missing helper refuses
new native extraction; qualified old cache hits still work. Direct low-level
adapters without a helper are internal primitive calls and do not provide this
process boundary.

The parent uses posix_spawn, a new process group, reset signal masks and a private
socket on child fd3. Other inherited host descriptors are closed: Linux uses
glibc closefrom spawn actions (>=2.34); macOS uses CLOEXEC_DEFAULT plus explicit
actions. Standard streams go to /dev/null. Original bytes are at most64MiB; owned
PDF rendering may internally supply at most4MiB+64 of generated PGM. Responses
are capped2MiB, framed, parsed and validated. Successful output is released only
after EOF, exit0 and the expected result identity/ranges/provenance; failure
publishes no new evidence. The private same-architecture protocol is not a public
network endpoint or a sibling's database interface.

The parent polls its owning request's cancellation/deadline during transfer,
native execution, output and reaping. The native request has at most30s, shortened
by an earlier deadline. Cancellation/expiry kills only the owning worker group
and reaps the worker. Cooperative callbacks retain real recognition-monitor
acceptance, but native initialization, decoding and rendering no longer need to
return before the parent can terminate them.

Worker watchdog checks peer EOF without consuming control replies. Caller death
causes its own group to receive SIGKILL. Linux additionally registers a
parent-death SIGTERM handler that kills that same group, rather than leaving
descendants alive after a direct worker kill. Test-only worker and descendant
stalls verify caller SIGKILL teardown; Linux test subreaper collects adopted
descendants. The worker never inherits a Sources SQLite handle or claims root.
OpenMP thread limits are set to1 in child environment only; core dumps are disabled.
This is process ownership/isolation, not an arbitrary native-code security sandbox.

SourcesConfig.native_memory_limit configures a worker RSS threshold between
16MiB and512MiB; zero selects512MiB. Parent polling samples the owned PID and
terminates/reaps it with native_helper_memory_limit when exceeded. Sampling can
overshoot between polls. Worker descriptor limit is64, CPU limit32 seconds and
core limit0. Linux production also limits address space to2GiB; ASan builds omit
that virtual-address limit for shadow memory while retaining RSS enforcement.
The private socket becomes close-on-exec before loading native libraries.

Fault hooks, process abort/stall/invalid frames/DTOs and readiness notifications
exist only in SAMOSA_SOURCES_TEST_FAULTS builds. Production worker rejects a
nonempty fault argument. Acceptance checks include errors, invalid output,
deadlines/cancellation, reaping, caller/descendant death, cache preservation and
healthy survivors. RSS/descriptor/CPU/core setup and durable cancellation with
cached/non-native peers pass normal/sanitizer checks on both platforms. Remaining
gates include simultaneous native-peer Linux acceptance, broader decoder corpus,
full independent releases and
bounded authenticated-service admission/shutdown. Do not claim Sources or S17a
completion from this boundary alone.
