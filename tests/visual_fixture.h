/* Shared independent public-Inference fixture for native visual gates. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "json.h"
#include "native.h"
#include "sha256.h"
#include "sources.h"
#include "visual.h"
#include <ftw.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static inline char *call(const SourcesConfig *cfg, const char *request, const char *error) {
    char *body = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&body, &n);
    CHECK(f);
    int rc = sources_dispatch(cfg, request, strlen(request), f);
    CHECK(!fclose(f));
    if (rc != (error ? 1 : 0))
        fprintf(stderr, "response: %s\n", body);
    CHECK(rc == (error ? 1 : 0));
    if (error) {
        Json j;
        CHECK(json_parse(&j, body, n));
        char *code = json_string(&j, json_get(&j, json_get(&j, 0, "error"), "code"));
        if (!code || strcmp(code, error))
            fprintf(stderr, "response: %s; expected: %s\n", body, error);
        CHECK(code && !strcmp(code, error));
        CHECK(!strstr(body, "artifact_id"));
        free(code);
    }
    return body;
}
static inline char *field(const char *body, const char *name) {
    Json j;
    CHECK(json_parse(&j, body, strlen(body)));
    char *s = json_string(&j, json_get(&j, 0, name));
    CHECK(s);
    return s;
}
static inline int digit(unsigned char c) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *p = strchr(alphabet, c);
    if (!p) {
        fprintf(stderr, "FAIL public pixel encoding\n");
        exit(1);
    }
    return (int)(p - alphabet);
}
typedef struct {
    SourcesInferenceConfig *inference;
    SourcesConfig *sources;
    const char *mode;
    size_t calls;
} Fixture;
static const char *transport(void *context, const SourcesInferenceControl *control,
                             const char *input, size_t bytes, char **result, size_t *length) {
    Fixture *f = context;
    f->calls++;
    CHECK(control->deadline_monotonic > 0);
    Json j;
    CHECK(json_parse(&j, input, bytes));
    char *op = json_string(&j, json_get(&j, 0, "op")),
         *provider = json_string(&j, json_get(&j, 0, "provider_id")),
         *prompt = json_string(&j, json_get(&j, 0, "prompt")),
         *data = json_string(&j, json_get(&j, 0, "image_rgba_base64"));
    size_t w = 0, h = 0, tokens = 0;
    CHECK(op && !strcmp(op, "analyze_image") && provider &&
          !strcmp(provider, f->inference->provider_id) && prompt && *prompt && data);
    CHECK(json_uint(&j, json_get(&j, 0, "width"), &w) && w && w <= 512);
    CHECK(json_uint(&j, json_get(&j, 0, "height"), &h) && h && h <= 512);
    CHECK(json_uint(&j, json_get(&j, 0, "max_output_tokens"), &tokens) && tokens == 128);
    free(op);
    free(provider);
    free(prompt);
    size_t n = strlen(data), used = 0;
    unsigned char *rgba = malloc(n / 4 * 3);
    CHECK(rgba && n % 4 == 0);
    for (size_t k = 0; k < n; k += 4) {
        unsigned a = (unsigned)digit(data[k]), b = (unsigned)digit(data[k + 1]),
                 c = data[k + 2] == '=' ? 0u : (unsigned)digit(data[k + 2]),
                 d = data[k + 3] == '=' ? 0u : (unsigned)digit(data[k + 3]);
        rgba[used++] = (unsigned char)((a << 2) | (b >> 4));
        if (data[k + 2] != '=')
            rgba[used++] = (unsigned char)((b << 4) | (c >> 2));
        if (data[k + 3] != '=')
            rgba[used++] = (unsigned char)((c << 6) | d);
    }
    free(data);
    CHECK(used == w * h * 4);
    char input_sha[65], ppm_sha[65];
    hash_bytes(rgba, used, input_sha);
    char *ppm = NULL;
    size_t ppm_bytes = 0;
    FILE *out = open_memstream(&ppm, &ppm_bytes);
    CHECK(out);
    fprintf(out, "P6\n%zu %zu\n255\n", w, h);
    for (size_t k = 0; k < w * h; k++)
        for (size_t c = 0; c < 3; c++)
            fputc((int)(((unsigned)rgba[k * 4 + c] * rgba[k * 4 + 3] +
                         255u * (255u - rgba[k * 4 + 3]) + 127u) /
                        255u),
                  out);
    CHECK(!fclose(out));
    hash_bytes(ppm, ppm_bytes, ppm_sha);
    free(ppm);
    free(rgba);
    if (!strcmp(f->mode, "unavailable"))
        return "transport_failure";
    if (!strcmp(f->mode, "deadline")) {
        struct timespec delay = {0, 50000000};
        nanosleep(&delay, NULL);
        return NULL;
    }
    if (!strcmp(f->mode, "busy")) {
        *result = strdup("{\"contract_version\":1,\"error\":{\"code\":\"busy\"}}");
        CHECK(*result);
        *length = strlen(*result);
        return NULL;
    }
    if (!strcmp(f->mode, "cancel")) {
        char *body = call(f->sources,
                          "{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":"
                          "\"cancel-visual\"}",
                          NULL);
        CHECK(strstr(body, "true"));
        free(body);
        CHECK(control->cancelled && control->cancelled(control->cancellation));
        return "cancelled";
    }
    if (!strcmp(f->mode, "wrong-input"))
        memset(input_sha, '0', 64);
    if (!strcmp(f->mode, "wrong-ppm"))
        memset(ppm_sha, '0', 64);
    out = open_memstream(result, length);
    CHECK(out);
    fprintf(out,
            "{\"contract_version\":1,\"provider_id\":\"%s\",\"kind\":\"%s\",\"artifact_type\":"
            "\"visual_observation\",\"input_sha256\":\"%s\",\"width\":%zu,\"height\":%zu,\"text\":"
            "\"%s\",\"usage\":{\"input_tokens\":12,\"output_tokens\":%u},\"coverage\":{\"status\":"
            "\"%s\",\"reason\":\"visual_analysis_uncertain\"},\"provenance\":{\"engine\":"
            "\"visionpsy\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"prepared_image_"
            "sha256\":\"%s\",\"alpha_policy\":\"%s\",\"coordinate_space\":\"input_pixels\"}}",
            !strcmp(f->mode, "wrong-provider") ? "other-provider" : f->inference->provider_id,
            !strcmp(f->mode, "literal") ? "literal" : "derived", input_sha,
            !strcmp(f->mode, "wrong-width") ? w + 1 : w, h,
            !strcmp(f->mode, "empty") ? "" : "A colored frame.",
            !strcmp(f->mode, "over-budget") ? 129u : 8u,
            !strcmp(f->mode, "full") ? "complete" : "partial", f->inference->engine_sha256,
            !strcmp(f->mode, "wrong-model") ? f->inference->engine_sha256
                                            : f->inference->model_sha256,
            ppm_sha, !strcmp(f->mode, "wrong-alpha") ? "ignored" : "rgba8_composited_on_white");
    CHECK(!fclose(out));
    return NULL;
}
static inline char *request(const char *source, size_t start, size_t end, const char *prompt,
                     const char *id) {
    char *body = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&body, &n);
    CHECK(f);
    fprintf(f,
            "{\"contract_version\":1,\"op\":\"analyze_visual\",\"source_id\":\"%s\",\"version\":\"%"
            "s\",\"start_ms\":%zu,\"end_ms\":%zu",
            source, source, start, end);
    if (prompt) {
        fputs(",\"prompt\":", f);
        json_quote(f, prompt, strlen(prompt));
    }
    if (id) {
        fputs(",\"operation_id\":", f);
        json_quote(f, id, strlen(id));
        fputs(",\"parent_operation_id\":\"visual-parent\"", f);
    }
    fputc('}', f);
    CHECK(!fclose(f));
    return body;
}
static inline int remove_file(const char *path, const struct stat *s, int type, struct FTW *walk) {
    (void)s;
    (void)type;
    (void)walk;
    return remove(path);
}
static inline size_t count(sqlite3 *db) {
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM prepared_artifacts", -1, &s, NULL) ==
              SQLITE_OK &&
          sqlite3_step(s) == SQLITE_ROW);
    size_t n = (size_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}
static inline void replace(sqlite3 *db, const char *artifact, const char *value) {
    char digest[65];
    hash_bytes(value, strlen(value), digest);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db,
                             "UPDATE prepared_artifacts SET response=?,digest=? WHERE "
                             "json_extract(response,'$.artifact_id')=?",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, digest, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, artifact, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1);
    sqlite3_finalize(s);
}
