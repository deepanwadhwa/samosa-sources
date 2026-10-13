#ifndef SAMOSA_SOURCES_SERVICE_HTTP_H
#define SAMOSA_SOURCES_SERVICE_HTTP_H
#include <stddef.h>
#define SOURCES_HTTP_HEADER_MAX 8192u
#define SOURCES_HTTP_PRINCIPALS_MAX 8u
typedef struct {
    char owner[129], token[65];
} SourcesHttpPrincipal;
typedef struct {
    SourcesHttpPrincipal principals[SOURCES_HTTP_PRINCIPALS_MAX];
    size_t count;
} SourcesHttpAuth;
typedef struct {
    size_t owner_index, content_length;
    int upload, health, expect_continue;
    char boundary[71], request_id[129], format[8];
} SourcesHttpRequest;
/* Host-owned credentials; tokens are exactly64 lowercase hex characters.
 * The caller loads bytes from an owner-only, descriptor-checked file. */
int sources_http_auth_parse(const char *, size_t, SourcesHttpAuth *);
/* Only a complete header is consumed. No request body access occurs here.
 * Returns HTTP status on refusal, zero for an authenticated bounded request. */
int sources_http_header(const SourcesHttpAuth *, const char *, size_t,
                        SourcesHttpRequest *);
#endif
