#include "service_http.h"
#include "sources.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                               \
    do {                                                                       \
        checks++;                                                              \
        if (!(x)) {                                                            \
            fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #x);                \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
static const char token[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static int header(SourcesHttpAuth *auth, const char *route, const char *extra,
                  const char *type, size_t bytes, const char *credential,
                  SourcesHttpRequest *request) {
    char body[9000];
    int n = snprintf(
        body, sizeof(body),
        "POST %s HTTP/1.1\r\nHost: 127.0.0.1:8643\r\nAuthorization: Bearer "
        "%s\r\n"
        "Content-Type: %s\r\nContent-Length: %zu\r\n%s%s\r\n",
        route, credential, type, bytes,
        !strcmp(route, "/v1/uploads") ? "X-Sources-Format: text\r\n" : "",
        extra);
    CHECK(n > 0 && (size_t)n < sizeof(body));
    return sources_http_header(auth, body, (size_t)n, request);
}
int main(void) {
    SourcesHttpAuth auth = {0};
    char config[4097];
    int n = snprintf(config, sizeof(config),
                     "[{\"owner\":\"alice\",\"token\":\"%s\"},"
                     "{\"owner\":\"bob\",\"token\":"
                     "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaa\"}]",
                     token);
    CHECK(sources_http_auth_parse(config, (size_t)n, &auth));
    CHECK(auth.count == 2 && !strcmp(auth.principals[1].owner, "bob"));
    SourcesHttpRequest request;
    CHECK(header(&auth, "/v1/sources",
                 "Expect: 100-continue\r\nX-Request-ID: request-1\r\n",
                 "application/json", 123, token, &request) == 0);
    CHECK(request.owner_index == 0 && request.expect_continue &&
          request.content_length == 123 &&
          !strcmp(request.request_id, "request-1"));
    CHECK(header(&auth, "/v1/sources", "", "application/json",
                 SOURCES_REQUEST_MAX, auth.principals[1].token, &request) == 0);
    CHECK(request.owner_index == 1);
    CHECK(header(&auth, "/v1/sources", "", "application/json",
                 SOURCES_REQUEST_MAX + 1, token, &request) == 413);
    CHECK(header(&auth, "/v1/uploads", "",
                 "multipart/form-data; boundary=test-123",
                 SOURCES_INPUT_MAX + 8192u, token, &request) == 0);
    CHECK(request.upload && !strcmp(request.boundary, "test-123"));
    CHECK(header(&auth, "/v1/uploads", "", "multipart/form-data; boundary=b",
                 SOURCES_INPUT_MAX + 8193u, token, &request) == 413);
    CHECK(header(&auth, "/v1/uploads", "",
                 "multipart/form-data; boundary=bad space", 100, token,
                 &request) == 400);
    CHECK(header(&auth, "/v1/uploads", "", "application/json", 100, token,
                 &request) == 415);
    CHECK(header(&auth, "/v1/sources", "", "text/plain", 100, token,
                 &request) == 415);
    CHECK(header(&auth, "/v1/sources", "", "application/json", 0, token,
                 &request) == 400);
    CHECK(header(&auth, "/v1/sources", "", "application/json", SIZE_MAX, token,
                 &request) == 413);
    CHECK(header(&auth, "/v1/sources", "", "application/json", SIZE_MAX,
                 "incorrect", &request) == 401);
    char wrong[65];
    strcpy(wrong, token);
    wrong[63] = '0';
    CHECK(header(&auth, "/v1/sources", "", "application/json", 1, wrong,
                 &request) == 401);
    const char *bad[] = {"Content-Length: 1\r\n",
                         "content-length: 1\r\n",
                         "Authorization: Bearer anything\r\n",
                         "Transfer-Encoding: chunked\r\n",
                         "Origin: http://127.0.0.1:8643\r\n",
                         "Expect: other\r\n",
                         "Connection: keep-alive\r\n",
                         " folded: header\r\n",
                         "Bad Header: x\r\n",
                         "X-Request-ID: invalid/owner\r\n",
                         "Content-Length : 1\r\n",
                         "Host: other\r\n"};
    const int status[] = {400, 400, 400, 400, 403, 417,
                          400, 400, 400, 400, 400, 400};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(header(&auth, "/v1/sources", bad[i], "application/json", 1, token,
                     &request) == status[i]);
    const char malformed[] = "POST /v1/sources HTTP/1.1\n\n";
    CHECK(sources_http_header(&auth, malformed, sizeof(malformed) - 1,
                              &request) == 400);
    CHECK(header(&auth, "/v1/sources", "\r\nInjected: header\r\n",
                 "application/json", 1, token, &request) == 400);
    char large[8193];
    memset(large, 'x', sizeof(large));
    CHECK(sources_http_header(&auth, large, sizeof(large), &request) == 431);
    const char *invalid[] = {"[]", "{}", "[{\"owner\":\"a\",\"token\":\"x\"}]",
                             "[{\"owner\":\"a/b\",\"token\":\"x\"}]"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(!sources_http_auth_parse(invalid[i], strlen(invalid[i]), &auth));
    n = snprintf(config, sizeof(config),
                 "[{\"owner\":\"a\",\"token\":\"%s\"},{\"owner\":\"b\","
                 "\"token\":\"%s\"}]",
                 token, token);
    CHECK(!sources_http_auth_parse(config, (size_t)n, &auth));
    n = snprintf(
        config, sizeof(config),
        "[{\"owner\":\"a\",\"token\":\"%s\"},{\"owner\":\"a\",\"token\":"
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"
        "]",
        token);
    CHECK(!sources_http_auth_parse(config, (size_t)n, &auth));
    puts("PASS native service HTTP authentication/header boundary");
    printf("%u checks; no socket service or multipart-body qualification "
           "claimed\n",
           checks);
    return 0;
}
