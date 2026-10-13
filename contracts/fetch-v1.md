# HTTP acquisition, initial bounded implementation

`sources_fetch(config, format, url, out)` is a host C entry point. The host grants
one exact `fetch_origin` and selects `fetch_helper`. Public JSON clients cannot
configure either. CLI hosts select --fetch-origin and --fetch-helper; public
requests use op=fetch with required acquisition_id, url and format.
`make fetch-worker` builds the optional `samosa-sources-fetch-worker` using
libcurl. The deterministic core does not link libcurl.

The initial URL grammar permits lowercase ASCII HTTP(S) origins with a hostname
or bracketed IPv6 literal and optional numeric port. Origins contain no path. Requests must match the
entire origin and begin the suffix with `/`, `?`, or end of string. Userinfo,
fragments, backslashes, malformed percent escapes, controls and non-ASCII URLs
are refused. IPv6 zone identifiers and malformed brackets are refused. Default
exact-origin HTTP is restricted to `127.0.0.1` or `[::1]`; HTTPS uses system certificate
trust and hostname verification. Hosts may instead supply a bounded PEM trust
bundle (see below). URL normalization conveniences are not implemented. DNS resolution belongs to the granted
origin in the default exact-origin profile. The optional host-owned public-only
profile below validates DNS answers and pins the connection.

## Public-only and offline host policies

SourcesConfig.fetch_public_only or CLI --fetch-public-only selects the extracted
monolith IPv4/IPv6 address policy. HTTP origins beyond loopback become admissible
only under that host policy. Ports must match the scheme's standard80/443. The
owned worker resolves the exact granted hostname, rejects the entire result if
any address is blocked, and pins libcurl to the selected address with CURLOPT_RESOLVE
and the matching address family. TLS trust/hostname validation and proxy/redirect
refusal still apply. The connected peer must equal the selected IP in binary form;
address changes refuse without accepted bytes/receipts. DNS blocking is isolated
inside the owned worker and remains subject to parent cancellation/deadline/reaping.

The address ranges are the working monolith's ipv4_blocked/ipv6_blocked policy,
not a newly substituted classification scheme. Mixed or malformed results fail
closed. Errors include fetch_nonpublic_address, fetch_dns_failed,
fetch_public_port and fetch_address_changed. The private public_only marker is
host-derived; public JSON requests cannot select or disable it. Exact-origin mode
still serves explicitly granted local/private provider fixtures and services.
IPv6 literals use the same address policy and connected-peer verification;
the numeric URL itself fixes the destination address. Public-only keeps
exact-origin redirect refusal. Host SourcesConfig.fetch_public_web or CLI
--fetch-public-web selects public-only acquisition plus bounded manual redirects;
search retains its exact-origin transport. At most six request destinations are
allowed, with exact URL loop refusal, per-hop DNS/pinning/TLS/peer checks and
independently validated original-URL/history/digest/base64/length fields.

Public-web receipts additionally contain final_url and urls. Immutable admission
records the final URL as the captured origin; acquisition identity stays bound to
the original URL. Historical three-field acquisition metadata remains readable;
five-field metadata is validated on replay. Offline replay makes no fresh DNS
claims or requests.

Public-web hops first probe /robots.txt through the same pinned transport,
without recursive robots gating. Probes admit at most256KiB and six destinations.
Text/plain and text/html replies use the extracted monolith evaluator with
agent samosachat; unavailable/non-text/malformed robots responses allow the page,
matching the monolith policy. Caller cancellation/deadline still terminates work.
A matching Disallow returns fetch_robots_denied before fetching the page.

Robots and page requests reserve one-second host:port slots in the Sources host
process, shared by its isolated workers. The bounded64-slot table preserves the
monolith behavior; equivalent default/explicit ports and IPv6 spellings share a
key. Waits occur outside the lock and retain cancellation/deadline/memory checks.
The private worker channel permits at most42 rate reservations per acquisition
(six page hops, each with up to six robots hops). These additions are under final
both-platform qualification. Provider-specific and final area acceptance remain.

SourcesConfig.fetch_offline or CLI --fetch-offline rejects new acquisition/search
network work with fetch_offline before spawning a transport worker. Previously
accepted acquisition and completed operation receipts still replay offline;
historical receipts do not assert a current DNS observation. Host configuration
is stable for a request; scoped cancellation handles active requests. Neither
policy grants a request access to host credentials or configuration.

The worker disables redirects and environment proxies. Only status200 is
accepted. Acquisition response bodies are at most5MiB, preserving the monolith's
MAX_PUBLIC_FETCH_BYTES limit; response headers are at most64KiB. Search retains
its own1MiB worker body limit. Only fetch worker response envelopes have the larger
base64 bound; other native reader DTO limits remain unchanged.
Content-Length and chunked bodies follow the same streaming cap. No response
content decoding is requested. Connect timeout5s and transfer timeout30s are
upper bounds; the existing parent isolation protocol enforces the caller's
earlier deadline, cancellation, sampled memory limit and process cleanup.

The exact-origin private response has exactly five fields: original URL, status, byte count,
SHA-256 digest and canonical base64 bytes. The parent independently validates
shape, URL, status, lengths, base64 padding/bits and content digest. Public-web
responses add the sixth field urls, with at most six unique validated destinations.
No worker
opens the Sources store. Only validated bytes enter the existing immutable
admission transaction, format checks, owner claims and shared quotas.
The host explicitly chooses the format; server MIME claims do not select a
reader. A successful response is the existing immutable snapshot envelope.
Offline reads retain the acquisition URL as provenance. Existing same-owner,
same-digest, same-format deduplication preserves the first claim's locator;
durable public acquisition receipts additionally retain each acquisition URL
and format, including distinct acquisition IDs for deduplicated content.

`test-fetch` and `sanitize-fetch` are optional dependency qualification targets,
using an actual local native HTTP server and the real isolated libcurl worker.
They cover public dispatch, native CLI execution, fresh-process offline receipt
replay, parameter conflicts, receipt corruption, normal/chunked responses, exact1MiB, overflow, truncated bodies,
redirect refusal, header limits, stalled cancellation/deadlines, URL and private
response corruption, immutable offline reading and owner isolation.
Broader certificate/backend qualification, alternate origins, provider
credentials/search adapters, larger fetches, complete concurrent/corruption
qualification, retry policy and service admission are still
pending. These targets do not close Sources #3 or a release milestone.


Public request example:

```json
{"contract_version":1,"op":"fetch","acquisition_id":"capture-1","url":"https://example.test/source","format":"text","operation_id":"attempt-1","timeout_ms":5000}
```

Supported formats are text, pdf, html, docx, image, wav and media. Acquisition
IDs use the same bounded ASCII identity grammar as operation IDs (1..128 bytes).
Acquisition ID is required; operation_id, parent_operation_id and timeout_ms
are optional existing request controls. Changing URL or format for an existing
owned acquisition ID returns idempotency_conflict. A new ID captures a new
remote observation. Clients cannot select an origin, worker, credentials or
private database through the request.

Accepted immutable snapshot, format/owner claim and acquisition receipt commit
in one Sources transaction. At most128 receipts exist globally, with no eviction.
Network transfer finishes before this write transaction. Concurrent transfer
attempts can fetch independently, but the transaction rechecks an already
accepted receipt before admission. A reply contains the ordinary snapshot
fields plus acquisition_id and acquisition {url,format,status:200}.
Receipt reuse verifies its SHA-256, exact shape/parameters and the owning
snapshot's actual bytes/hash/format claim. Corruption is refused with
corrupt_acquisition. Receipt reuse needs no active network grant or worker.

Operation status remains separately durable. A crash after acquisition commit
can leave that attempt interrupted; a new operation_id with the same
acquisition_id recovers the accepted receipt without another network request.
Before commit, cancellation/deadline/SIGKILL leave no acquisition receipt or
new immutable admission. After commit, a late cancellation does not rewrite
success as cancelled. Test-only staging hooks qualify both crash boundaries,
precommit cancellation/deadline and late postcommit cancellation; these hooks
are absent in product builds. Complete crash/concurrency qualification remains
open. The host C sources_fetch entry has no acquisition ID and continues to use
ordinary immutable admission without a public acquisition receipt.


Host trust configuration: SourcesConfig.fetch_ca_pem supplies at most8192 bytes
of PEM CA certificates, copied with URL/origin into the bounded private worker
request. CLI --fetch-ca-file captures a regular non-symlink file at most8192
bytes and rejects embedded NULs and changed descriptor/named-file metadata.
A host keeps the config's PEM buffer stable until return. Public requests cannot
supply a CA file or PEM bundle. No trust file path is sent to the worker.

The worker materializes the captured PEM into a private, immediately unlinked
file and configures libcurl with its own retained descriptor alias. The trust
file descriptor stays open through transfer and closes with cleanup; worker
process death also closes it. This supports the reference macOS SecureTransport
and Linux OpenSSL backends without reopening host certificate paths. Certificate
and hostname verification stay enabled. The default (no PEM) uses system trust;
invalid supplied trust fails rather than falling back. Errors include
fetch_tls_trust_invalid, fetch_tls_verification_failed and fetch_tls_failed;
backend-specific certificate/handshake failures can use different TLS codes.
The private request remains at most16384 bytes including the encoded PEM.
Only the isolated fetch worker ignores SIGPIPE; host signal handlers are untouched.

Optional make test-fetch-tls sanitize-fetch-tls requires the OpenSSL3 SDK only
for the native fixture server/certificate generator. Sources does not link that
fixture SDK; the product worker still uses libcurl and its installed TLS backend.
Actual TLS1.2 fixtures cover trusted signed leaf, unknown/wrong trust issuer,
wrong hostname, expired leaf, malformed PEM, host API oversize, real CLI custom
trust capture and CLI symlink/oversize/NUL refusal. Database inspection proves
only accepted content is admitted. Both platform normal and ASan/UBSan runs pass.
A real public HTTPS fetch using default system trust and offline HTML extraction
also passes on both platforms with identical source/evidence hashes.
Multi-certificate/wildcard/client-certificate/revocation/backend/version and
complete trust-file race qualification remain open, alongside search/providers
and full component release acceptance.
