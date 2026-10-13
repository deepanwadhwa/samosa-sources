#ifndef SAMOSA_SOURCES_H
#define SAMOSA_SOURCES_H
#include <stdio.h>
#include <stddef.h>
#define SOURCES_VERSION "0.1.0"
#define SOURCES_REQUEST_MAX 16384u
#define SOURCES_INPUT_MAX (64u * 1048576u)
#define SOURCES_READ_MAX 65536u
struct SourcesOcrConfig;
struct SourcesSearchConfig;
typedef struct {double deadline_monotonic;int (*cancelled)(void *);void *cancellation;} SourcesInferenceControl;
/* Host transport consumes only public Inference JSON. It owns blocking,
 * cancellation and child/socket cleanup and returns a malloc-owned, NUL-terminated
 * response; returned length excludes the terminator. */
typedef struct {
    const char *provider_id,*engine_sha256,*model_sha256;
    const char *(*call)(void *,const SourcesInferenceControl *,const char *,size_t,char **,size_t *);
    void *context;
} SourcesInferenceConfig;
typedef struct {
    SourcesInferenceConfig inference;
    const char *recipe,*query_prefix,*document_prefix;
    size_t context_tokens;
} SourcesEmbeddingConfig;
/* Public single-request stdin/stdout process adapter. Arguments are a JSON array
 * of host-owned strings; no shell is invoked. The Sources worker supervises it. */
typedef struct {
    const struct SourcesConfig *sources;
    const char *executable,*arguments_json;
    /* Host-only opt-in: bounded 24MiB prepared temporal DTO, 120s deadline. */
    int temporal_frames;
} SourcesInferenceProcess;
const char *sources_inference_process_call(void *,const SourcesInferenceControl *,const char *,size_t,char **,size_t *);
/* Configuration belongs to the host, never to a request. Zero uses defaults.
 * A library caller may lower snapshot_limit/byte_limit for constrained hosts. */
typedef struct SourcesConfig {
    const char *root, *state;
    size_t snapshot_limit, byte_limit;
    size_t cache_limit, cache_byte_limit;
    const char *pdfium_library;
    const struct SourcesOcrConfig *ocr;
    /* Executable implementing the private Sources native worker entry point.
     * Hosts configure it; public requests cannot select an executable. */
    const char *native_helper;
    /* Optional host-selected codec worker; independent of the plain/OCR helper. */
    const char *media_helper;
    /* Host grants one exact HTTP(S) origin and selects the optional fetch worker. */
    const char *fetch_origin, *fetch_helper;
    /* Optional host-owned PEM CA bundle, max8192 bytes; copied into worker. */
    const char *fetch_ca_pem;
    /* Host-selected public address validation/pinning; clients cannot override it. */
    int fetch_public_only;
    /* Public web acquisition may follow bounded redirects; search stays exact-origin. */
    int fetch_public_web;
    /* Private worker callback for parent-owned public host rate reservations. */
    const char *(*fetch_rate)(void *,const char *);
    void *fetch_rate_context;
    /* Deny new network work; retained acquisition/operation receipts stay readable. */
    int fetch_offline;
    const struct SourcesSearchConfig *search;
    /* Host-selected public Chutni SDK and existing store, opened read-only. */
    const char *chutni_library, *chutni_store, *chutni_helper;
    /* Explicit write grant and expected existing store UUID for publication. */
    const char *chutni_store_id;
    /* Fixed public SDK representation profile for semantic retrieval. Requests
     * may supply a bounded query vector, never a model/profile/store override. */
    const char *chutni_semantic_profile;
    const SourcesInferenceConfig *inference;
    /* Separate visual provider; audio and visual recipes may coexist. Both use
     * the same public Inference transport/control contract. */
    const SourcesInferenceConfig *visual_inference;
    const SourcesInferenceConfig *temporal_inference;
    const SourcesInferenceConfig *summary_inference;
    /* Native public embedding transport and preprocessing expectations. The
     * retrieval profile remains the host-selected Chutni semantic profile. */
    const SourcesEmbeddingConfig *embedding;
    int chutni_publish;
    /* Sampled worker RSS kill threshold: zero=512MiB; hosts may lower to16MiB. */
    size_t native_memory_limit;
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *native_test_fault;
    void (*native_test_event)(void *,int,const char *,struct SourcesConfig *);
    void *native_test_context;
#endif
    /* Optional host binding pins; clients cannot select or change them. */
    const char *root_scope_pin;
    int state_pinned;size_t state_device,state_inode;
    const char *host_id;
    const char *owner;
    /* Scoped owning request control, never a shared global cancellation slot. */
    /* Host opt-in to bounded periodic discovery and initial indexing, zero disables. */
    size_t discovery_interval_ms;
    double deadline_monotonic;
    int (*cancelled)(void *);void *cancellation;
} SourcesConfig;
/* Returns 0 on success, 1 for a structured operation error. Always one JSON
 * envelope; no response contains pointers or private database paths. */
int sources_dispatch(const SourcesConfig *, const char *, size_t, FILE *);
typedef enum {SOURCES_UPLOAD_TEXT,SOURCES_UPLOAD_PDF,SOURCES_UPLOAD_HTML,
    SOURCES_UPLOAD_DOCX,SOURCES_UPLOAD_IMAGE,SOURCES_UPLOAD_WAV,
    SOURCES_UPLOAD_MEDIA} SourcesUploadFormat;
/* Host-supplied immutable buffer, at most SOURCES_INPUT_MAX; no root grant.
 * Keep the buffer stable until return. The component owns its ingestion copy.
 * This binary entry point does not bypass snapshot/owner/cache quotas. */
int sources_upload(const SourcesConfig *,SourcesUploadFormat,const void *,size_t,FILE *);
/* Reader returns zero on success, sets bytes read <= capacity; zero bytes is
 * EOF. The host bounds callback blocking and owns its transport/cancellation.
 * Sources checks declared length and scoped controls around every callback. */
typedef int (*SourcesUploadRead)(void *,void *,size_t,size_t *);
int sources_upload_stream(const SourcesConfig *,SourcesUploadFormat,size_t,SourcesUploadRead,void *,FILE *);
/* Host acquisition entry: at most 1MiB, exact granted origin, no redirects.
 * Response uses immutable snapshot admission and retains URL provenance.
 * HTTP is limited to 127.0.0.1; HTTPS verifies system trust and hostname. */
int sources_fetch(const SourcesConfig *,SourcesUploadFormat,const char *,FILE *);
#endif
