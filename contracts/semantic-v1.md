# Semantic retrieval through Chutni, version 1

Sources delegates cosine ranking to the existing public Chutni SDK. It uses the
same isolated worker, documented four-function ABI, read-only existing-store
access and immutable retrieval capture as memory_search. Sources adds no vector
store or ranking implementation.

The host supplies chutni_library, chutni_store, chutni_helper, the expected
chutni_store_id UUID and chutni_semantic_profile. The CLI reads the latter from
--chutni-semantic-profile-file using the existing owner/private-file checks.
Public requests cannot select a store, profile, model, library or executable.

Example host profile:

```json
{"representation_kind":"text_embedding","model_id":"example/embed","model_revision":"1","dimensions":2,"dtype":"f32","normalization":"none"}
```

The required fields above have nonempty UTF-8 string values through256 bytes,
except dimensions, which is an integer1–1024. Only text_embedding/f32 and
normalization none or l2 are admitted. Optional tokenizer_hash and projector_hash
are nonempty strings through256 bytes or null. Missing optional fields normalize
to null. Unknown/duplicate fields are refused. The profile input is at most4096
bytes and its complete identity is preserved in captured query metadata.

```json
{"contract_version":1,"op":"memory_search_semantic","vector":[1,0],"operation_id":"semantic-1","timeout_ms":10000}
```

Only vector and ordinary operation controls are accepted. The vector must match
the host dimensions, contain finite representable f32 numbers, and have nonzero
norm after f32 conversion. For l2 profiles the squared norm must be within0.001
of1; Sources refuses incorrect normalization instead of silently normalizing.
Numbers are canonicalized to round-trippable f32 JSON, with C numeric locale
scoped to the thread and restored afterward. Equivalent decimal spellings and
request key order replay the same completed operation. Public requests remain
bounded to16KiB; normalized SDK arguments are also bounded to16KiB.

The worker verifies store_info against the granted UUID and calls the public
search_semantic operation with the complete profile, limit8 and
include_stale=false. Its normalized response is at most64KiB, with the same
external references, artifact kind, snippet, freshness, score and score_type
fields as lexical retrieval. score_type is cosine_bruteforce; scores are kept
separate from BM25 scores. The parent independently checks the exact captured
query/profile/vector, store UUID, external UUIDs, eight-result limit, current
freshness, finite cosine bounds and descending order. Model/profile mismatch
returns the SDK's empty result, without mixing representations or falling back.

The existing Sources transaction admits owner-scoped immutable retrieval
metadata after final cancellation/deadline checks. Completed operations replay
without the profile or SDK configured; their metadata remains readable through
read_text for the owning principal. Coverage is partial/retrieval_metadata;
ranked snippets do not become literal original-source evidence. No SDK writes,
store creation or registry discovery occur during retrieval.

make test-semantic sanitize-semantic CHUTNI_LIBRARY=/absolute/public/sdk exercises
actual SDK representations, sorted positive/negative queries, profile isolation,
read-only counts, invalid vectors, store pins, owner/offline replay, cancellation,
deadlines, malformed parent replies and numerical/dimension boundaries. The
representation fixtures prove retrieval, not embedding-model quality.

This is the prepared-vector retrieval interface. The natural-language interface
is documented in [embedding-v1.md](embedding-v1.md). Automatic representation
publication and broader actual-model qualification remain required architecture
work; these retrieval adapters alone do not close area4.
