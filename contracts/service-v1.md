# Authenticated Sources service1 — implementation in progress

Area6 owns authenticated multipart ingress, global component admission/fairness,
and bounded service shutdown. Source registration, immutable upload ingestion,
durable operation ownership and background queues already exist; the service
must adapt those interfaces rather than duplicate storage or model execution.
App remains responsible for browser sessions and product login. This component
uses host-declared bearer credentials mapped to immutable request owners.

The implemented first boundary (`service_http.c`) consumes only complete HTTP
headers, never body bytes. Headers are bounded to8192bytes/64fields. Host auth
configuration is a<=4096byte JSON array of1..8 distinct owner/token objects.
Owners use the existing bounded ASCII identity alphabet; each token is exactly
64lowercase hex characters, with distinct tokens and owners. The service loader
uses an owner-only descriptor-checked file in the CLI. Tokens are never part of
Source receipts or operation input. Keep host credentials outside granted roots.

Implemented loopback ingress uses HTTP/1.1, one request per connection and close responses:
`POST /v1/sources` carries the existing<=16384byte public JSON contract;
`POST /v1/uploads` carries a single native streaming multipart upload with
<=64MiB source bytes and<=8192bytes multipart framing overhead. A required
Content-Length is checked before accepting a body or sending100Continue.
Authentication maps owner from host configuration; request JSON cannot choose
another owner. Browser Origin requests are refused at this component boundary;
App/proxy supplies its authenticated component request without forwarding Origin.
Duplicate headers, transfer encoding, folded/malformed headers and unsupported
expectation/connection semantics refuse. Optional X-Request-ID is a bounded ASCII
correlation identity. It does not substitute for durable operation_id.

Start with `samosa-sources --serve --serve-auth-file /private/auth.json
--root /granted/root --state /private/source-state`. `--serve-port` defaults to
8643; zero selects an OS-assigned port, reported by startup JSON. Credentials are
captured before listening. `--owner`, `--index-run` and `--watch-id` cannot override
service ownership. Absolute private state is required; startup validates through
owned operation storage and pins its device/inode. An optional root is pinned
through the existing scope interface. Authenticated `GET /healthz` reports
transport health without requiring a model.

Multipart requires `X-Sources-Format: text|pdf|html|docx|image|wav|media` and one
`Content-Disposition: form-data; name="file"` part with an optional ignored quoted
filename and optional Content-Type. Part headers are<=4096bytes; their filename
never becomes a filesystem path. The closing delimiter is validated before
immutable ingestion commits. Additional embedded part delimiters refuse. Actual
media acceptance remains the existing Source format/sniff policy.

The parent bounds16accepted peers and their header/JSON buffers. Two normal
workers share8waiting work slots, with FIFO per owner and round-robin selection
across owners. One separate control worker has2waiting slots for describe,
operation_get/cancel and health. JSON100Continue follows validated headers;
multipart100Continue follows worker admission. Slow headers expire after5seconds.
Upload ingress has a120second budget from authenticated headers. JSON uses the
public60second default, or120seconds for analyze_video; a valid timeout_ms reduces
that same budget, including waiting admission. Responses are<=8MiB. Poll/write
waits are bounded; disconnects and parent death feed scoped worker cancellation.
SIGTERM/SIGINT stop admission, signal owned workers, and reap them with a3second
grace followed by SIGKILL. Committed Source receipts remain authoritative.

Finite area6 acceptance, derived from the architecture C11 transport requirements
and existing Sources upload/queue contracts:

- Authenticate before reading or staging a large body; refuse invalid/foreign
  credentials and owner overrides. Qualify model-less health and configuration
  refusal, owner-scoped requests, multipart formats/limits and immutable readback.
- Bound simultaneous connections, header/body buffering, active workers and
  waiting work before expensive processing. Qualify slow/truncated/oversized
  ingress and overload without starving already admitted work.
- Preserve FIFO order within each owner and bounded progress across admitted
  owners. Operation cancellation/status must remain reachable during expensive
  work; cancellation targets the durable owner/operation identity.
- Integrate the existing bounded index/audio/enrichment/publication queue runtime
  with explicit ownership. Headless service progress must not require an App.
- Bound request deadlines, disconnect cleanup and stop admission during shutdown;
  reap only owned workers, retain committed receipts and prove cold restart/retry.
  Qualify active native/provider work, upload and background work interruption.
- Run actual socket/client and native normal/sanitizer gates on macOS/Linux,
  including multipart64MiB, concurrency/fairness, scoped controls and shutdown.
  Header-unit checks alone do not close service acceptance.

The service holds private, inode-bound service-runtime.lock and
reindex-runtime.lock leases, excluding another daemon or standalone index runtime
for the same state. State, root or named lease replacement stops admission and
exits unsuccessfully. Configured host root/store/provider grants apply to all
configured credentials; Source receipts, jobs, caches and cancellation remain
owner-scoped.

Background phases reuse the public refresh/index/publication/audio/visual/
enrichment/embedding queue operations. They rotate owners, share the two normal
worker slots, admit at most one background worker, and alternate with foreground
work. Host flags enable the corresponding phases; --index-interval-ms controls
phase pacing. Job reads may return the existing transient busy response while a
queue phase owns its lease. Health reports per-owner background phase/failure.
Foreground deadlines permit three seconds of cleanup after the request budget.

The adapter runs in a dedicated daemon process owning its worker tree. Linux
uses child-subreaper adoption and /proc child enumeration to reap descendants
orphaned during provider interruption, including forced shutdown. macOS uses the
existing owned native guardian/process cleanup. No unrelated processes are killed.

Finite area6 acceptance is closed: actual sockets, all seven formats, 64MiB,
owner fairness/overload/control access, two-owner background indexing, active
provider and background interruption, retained cold recovery, shared leases and
state/root replacement pass native normal/sanitizer gates on macOS/Linux.
No Python runtime is introduced. Independent packaging/release acceptance is7.
