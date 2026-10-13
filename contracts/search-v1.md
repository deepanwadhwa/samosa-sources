# Sources search contract, version 1

Search migrates the legacy W1 declarative provider interface into Sources.
It does not link the gateway or import its environment, conversations or jobs.
The host chooses an adapter and grants its exact HTTP(S) origin; requests cannot
choose endpoints, headers or credentials. Search requires the optional isolated
libcurl fetch worker. The default core has no libcurl dependency.

Public request:

```json
{"contract_version":1,"op":"search","query":"Find official libcurl documentation","operation_id":"search-1","timeout_ms":30000}
```

`query` is required, nonempty UTF-8, at most 512 bytes. Existing operation controls
(`operation_id`, `parent_operation_id`, `timeout_ms`) apply. Only one provider
attempt is made; failures do not select a fallback provider.

The host API uses `SourcesConfig.search` and `SourcesSearchConfig` from
`src/search.h`. `sources_search_preset` declares parallel, brave, tavily, serpapi,
google or searxng. A generic declaration supplies provider, origin, url_template,
results_path, title_path, url_path and description_path, with optional headers,
body and values. Dot paths traverse object fields (up to eight steps); an empty
results_path selects a root array. Description fields accept a string or up to
32 strings, joined with newlines. A body selects POST; otherwise the adapter
uses GET. Headers and bodies are rendered from JSON templates, preserving JSON
escaping. URL substitutions are percent encoded, except host-provided base_url.
Supported placeholders are query, a fresh session_id and named host values.
Missing or empty substitutions fail explicitly.

CLI configuration uses `--search-provider NAME` or `--search-config-file FILE`,
followed by optional `--search-values-file FILE`, `--search-origin ORIGIN` and
`--search-url TEMPLATE`; select `--fetch-helper EXECUTABLE`. Searxng requires an
explicit origin and base_url value. Declaration files contain JSON objects;
headers and values are JSON objects, and body is JSON. Configuration files are
bounded to 8 KiB and values files to 4 KiB; both must be regular files owned by
the effective user with no group/other permissions. Final symlinks, embedded NUL,
oversize files and changed identity/metadata during capture are refused.
Select the declaration before applying overrides. Credentials stay in host files
and private worker inputs, rather than public request fields or argument values.

The HTTP worker retains the fetch contract: exact origin authorization, verified
TLS, optional copied CA trust, no redirects or environment proxies, bounded
headers (64 KiB) and response (1 MiB), execution deadline and cancellation.
POST bodies are bounded to 8 KiB; header objects to eight entries and 4 KiB.
Control characters and transport-controlling headers are refused. The parent
validates the worker response length, canonical base64 and content hash.

Provider replies may be JSON or bounded SSE data records. Parallel additionally
requires JSON-RPC 2.0, matching request id 1 and no tool-error flag. Multiple final
SSE results and malformed normalized items are refused. Output retains at most
eight items, titles up to 256 Unicode characters, descriptions up to 400 and
HTTP(S) reference URLs up to 2048 ASCII bytes. Reference URLs are not fetched by
search. Host values, including percent-encoded forms, are redacted from text;
reference URLs containing them are refused. Raw provider replies and credentials
are not stored.

Success returns the ordinary immutable source envelope, an evidence_id and a
search object containing provider, query, observation time, coverage and items.
Coverage is always partial/provider_search_metadata: snippets describe search
results and do not establish the contents of a referenced page. The normalized
JSON is admitted through existing immutable snapshots, owner claims and quotas.
Admission uses one transaction with a final interruption check. Stored metadata
is subsequently readable through read_text with no provider or worker.
An identical completed operation_id replays its recorded response offline.
Search has no separate acquisition receipt: an interrupted attempt whose result
was not recorded can require another provider request. Fetch acquisition_id
recovery semantics must not be inferred for search.

Qualification targets: `make test-search sanitize-search` exercise actual HTTP
POST, JSON/SSE, escaping, redaction, result bounds, invalid replies, host
configuration files, ownership and offline operation replay/read using real
worker and CLI processes. Live Parallel checks are separate from deterministic
fixtures. Preset declarations alone do not qualify paid provider integrations.
`make test-search-presets sanitize-search-presets` exercises all six unchanged
presets through actual native worker HTTP, preserving their original path/query
templates, headers and POST bodies with only the host-granted fixture origin
replaced. It checks provider-shaped results, malformed replies, provider errors,
redirect refusal, redaction and exactly six accepted snapshots across24 cases.
This qualifies the component mappings; paid live-service access still requires
host credentials and is not asserted by fixture results.
Shared service admission and resource
policy, broader concurrency/crash coverage and full Sources release remain open.
