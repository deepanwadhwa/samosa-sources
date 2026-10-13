
S07a provenance includes extractor_fingerprint (lowercase SHA-256). Evidence ID
binds reader contract, extractor fingerprint, exact immutable version and selector.
Different configured PDFium artifacts produce different evidence IDs even when
extracted text is equal. Artifact unavailable/invalid/changed yields typed error
before publishing evidence; source version and absolute citations remain stable.

S07b caching is an owner implementation detail. Public reads return identical
qualified evidence on reuse; consumers need no cache paths or schema. Hits still
validate immutable bytes and configured extractor fingerprint. Literal/partial
coverage is retained, failed output is excluded. Typed cache_limit/corrupt_cache/
cache_conflict never become successful evidence. Describe publishes maximum cache
and artifact bounds. Host library config can lower cache admission bounds.
