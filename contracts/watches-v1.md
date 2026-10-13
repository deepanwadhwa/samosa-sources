# Sources watch registry v1

locator_watch_create accepts contract_version1, operation_id and1..8 locators.
Locator fields/validation match locator_reconcile: path, expected_version, optional
expected_device/expected_inode. Every version must belong to the configured owner.
The host supplies root/state/owner/host identity; requests cannot enlarge grants.
Creation retains a root-scoped registration rather than observing content.
Missing current files may be registered against retained immutable versions.

Creation identity includes normalized locator order/fields and host identity.
Exact repeated registration returns the same watch_id/registration before root
access; changed parameters conflict. State in the response is current registry
state, not a replay of an earlier lifecycle state. Root replacement does not
silently move an existing registration to the new root. Different owners are
isolated and cannot retrieve or stop another owner's registration.

locator_watch_get takes watch_id and returns owner-bound durable status without
root access. locator_watch_stop takes watch_id, marks it stopped, and is repeatable
without the original root. Eight retained registrations globally, including stopped
records; no eviction. Shared locator writer lease admits one writer, zero queued.
Registration responses are bounded64KiB and stored with integrity digests under
the existing database ceiling. Foreign/unknown IDs return unknown_watch.

Registry states are registered/stopped. monitoring is not_running until a runtime lease is held; running denotes that
lease, and unknown denotes a lease inspection failure. These registry operations
alone do not start a continuous watcher. Continuous runtime and explicit
stop/removal/replacement are described below. Index reindex execution remains
a separate indexing requirement. Consumers
must use locator_read for present live evidence; registry existence implies no
current freshness, enumeration completeness or successful background work.

## Atomic checkpoints

locator_watch_step accepts watch_id and expected_cycle (unsigned, starts0).
It verifies the registered host/root, observes at most eight files, rechecks
file/parent/root bindings and atomically commits locator rows and the next watch
checkpoint. Existing index freshness observes these same committed locator rows.
Polling replaces one latest checkpoint per watch rather than consuming one of
256 durable locator operation histories on every cycle.

The response includes watch_id/cycle and ordinary locator observations. Get and
repeated create include checkpoint (null before the first successful step).
An exact retry for the immediately preceding cycle returns the saved checkpoint
without accessing the root. Older/future cycles refuse watch_cycle_conflict.
Stopped watches refuse watch_stopped. This bounded retry window is explicit;
checkpoints are not an unbounded event log or automatic re-registration of changes.

Cancellation/deadlines and filesystem races commit neither partial observations
nor an advanced checkpoint. Registered root/host mismatch refuses scope_unavailable.
Latest checkpoint/result integrity is checked on retrieval; corrupt state refuses
without partial JSON/evidence. One-shot steps do not claim continuous monitoring.
Bounded watch runtime admission is described below; global service fairness and
reindex execution retain their separate service/indexing requirements.

## Continuous runtime

Run the Sources executable with its normal host grants plus --watch-id ID and
optional --watch-interval-ms100..60000 (default1000). It does not read stdin or
stream checkpoint events to stdout: public get/search read committed checkpoints.
One kernel lease per watch refuses duplicate workers with watch_busy. Eight
retained registrations bound the number of distinct admitted watch workers.

The worker gets the latest cycle, observes/commits a next checkpoint, then sleeps
the configured interval. Each observation gets at most five seconds, further
bounded by the host deadline/cancellation. Busy/conflict/racing/deadline outcomes
leave prior checkpoints intact and retry after the interval. Permanent scope,
corruption or state errors end the worker with a typed failure; it does not rebind
replaced roots or states. Monitoring reports the lease, not completeness or a
successful current observation; checkpoint timestamp remains authoritative.

SIGTERM/SIGINT cancel active hashing/precommit work and interrupt the wait.
Public watch_stop ends the loop at its next check. SIGKILL releases kernel leases;
a new worker resumes from the durable checkpoint. Explicit removal/replacement
and interrupted cleanup are described below. Automatic process respawn, global
service fairness and reindex scheduling retain service/indexing requirements.
Sleep begins after a completed/failed attempt, so this is bounded
polling with a minimum interval rather than a fixed-rate timing guarantee.

## Runtime health

Watch status includes health: attempts, successes, failures, last_attempt_ms and
last_error (null after a successful attempt). These are durable completed-attempt
reports across runtime restarts, bounded to one row per retained watch. A failure
updates its code/time and cumulative counter without replacing the checkpoint.
A later successful attempt clears last_error and increments successes.

Health recording is component-private; clients cannot submit fabricated counters.
Checkpoint commit and health report are separate: a killed worker can leave a
new checkpoint with an older health report. Lease status and health do not replace
checkpoint observation timestamps or prove current filesystem completeness.
Broad fair scheduling, long-running resource qualification and automatic lifecycle
remain required.
A successful report means the observation completed; per-file checkpoint status
still determines availability. Invalid counter types/ranges or invalid last-error
codes refuse corrupt_locator before status/checkpoint publication. Counters saturate
at INT64_MAX while the latest attempt time/error continue updating.

## Removal and replacement

locator_watch_remove accepts watch_id and optional shared durable controls:
operation_id, parent_operation_id, timeout_ms. The watch must belong to the owner
and be stopped; a held runtime lease refuses watch_busy. Removal atomically deletes
registration, latest checkpoint and health metadata. Immutable sources, caches,
locator observations and indexed historical evidence remain retained.

Use an operation_id for exact saved removal retry after the watch disappears.
Untracked repeat returns unknown_watch. Failed tracked requests retain their own
terminal result; use a new operation_id when retrying after resolving a refusal.
Removed registration no longer consumes one of eight slots. Creation retry identity
is retained while its watch exists; creation after removal produces a fresh opaque
watch_id even for identical operation/locator parameters, preventing stale workers
from attaching to a replacement lifetime. New IDs use OS entropy with owner/root/
normalized request material; snapshots retain their existing content identities.

Successful removal reports lease_cleanup complete/retained. Runtime lease unlink
requires its checked held inode; failed cleanup does not restore removed metadata.
A late admitted worker that discovers removal cleans its own checked lease and
exits. Crash-time leftover private leases and broader garbage collection still
require qualification. A stopped watch runtime exits before creating a new lease.
Replacement is explicit remove/create, with the new host root grant independently
validated. There is no automatic root rebind or destructive source-file operation.
A successful committed removal preserves its completed receipt if cancellation or
its deadline arrives during receipt finalization. Cancellation before commit rolls
back removal. This commit precedence applies to removal; read-result publication
continues to enforce its existing cancellation/deadline behavior.

## Interrupted-removal cleanup

Removal transactionally records owner-bound cleanup intent before deleting watch
metadata. Eight pending intents globally; a full intent queue refuses further
removal with watch_cleanup_limit before mutation. Normal successful inode cleanup
clears its intent. A process killed between metadata commit and filesystem cleanup
leaves an intent available for recovery.

locator_watch_cleanup accepts only contract_version/op. It drains at most eight
owned intents under the shared observer lease, refuses any still-registered identity,
checks/acquires private runtime leases and unlinks only the checked held inode.
Busy leases remain retained. It reports cleaned/retained counts and is repeatable
with no root grant. Foreign owners cannot drain another owner's intents. Immutable
sources, locators and historical index evidence remain retained.

Cleanup metadata and filesystem effects are restartable rather than one global
atomic batch. Failure/cancellation can leave previously cleaned rows completed;
retry safely drains the remainder.

Runtime admission revalidates owner, registered state and captured root under the
same observer lease used by stop, removal and cleanup. It creates and acquires the
private runtime lease before releasing that observer lease. A stale registration
lookup cannot create a lease after removal and cleanup. Busy observer/store
admission retries for at most five seconds; duplicate runtime leases still refuse
watch_busy. Process death before or after private lease acquisition releases the
observer lock; an existing registered lease file remains covered by later removal
intent. Broader resource/fairness and service lifecycle qualification remain open.
