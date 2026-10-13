#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "cache.h"
#include "control.h"
#include "native.h"
#include "sha256.h"
#include "temporal.h"
#include <time.h>
#define RECIPE "source-temporal-uniform-last-v1"
#define CAPTURE_MAX 65536u
static int pin(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static int equal(const Json *j, int object, const char *name,
                 const char *expected) {
    char *s = json_string(j, json_get(j, object, name));
    int valid = s && expected && !strcmp(s, expected);
    free(s);
    return valid;
}
static int number(const Json *j, int object, const char *name,
                  size_t expected) {
    size_t n = 0;
    return json_uint(j, json_get(j, object, name), &n) && n == expected;
}
static char *object(const Json *j, int t) {
    if (t < 0 || j->tokens[t].type != JT_OBJECT)
        return NULL;
    size_t n = j->tokens[t].end - j->tokens[t].begin;
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, j->text + j->tokens[t].begin, n);
        s[n] = 0;
    }
    return s;
}
static const char *selector(const SourcesConfig *cfg, const char *source,
                            size_t start, size_t end, const char *prompt,
                            char identity[65], char key[65]) {
    const SourcesInferenceConfig *p = cfg->temporal_inference;
    if (!p || !p->provider_id || !*p->provider_id ||
        strlen(p->provider_id) > 128 || !pin(p->engine_sha256) ||
        !pin(p->model_sha256))
        return "inference_unconfigured";
    for(const unsigned char *id=(const unsigned char *)p->provider_id;*id;id++)
        if(*id>127||(!isalnum(*id)&&!strchr("-_:.",*id)))return "invalid_configuration";
    char *body = NULL;
    size_t bytes = 0;
    FILE *out = open_memstream(&body, &bytes);
    if (!out)
        return "resource_limit";
    fprintf(
        out,
        "{\"recipe\":\"" RECIPE
        "\",\"source_id\":\"%s\",\"start_us\":%zu,\"end_us\":%zu,\"prompt\":",
        source, start * 1000, end * 1000);
    json_quote(out, prompt, strlen(prompt));
    fputs(",\"provider_id\":", out);
    json_quote(out, p->provider_id, strlen(p->provider_id));
    fprintf(out, ",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\"}",
            p->engine_sha256, p->model_sha256);
    if (fclose(out)) {
        free(body);
        return "resource_limit";
    }
    hash_bytes(body, bytes, identity);
    free(body);
    Sha256 h;
    sha256_init(&h);
    const char *owner = cfg->owner ? cfg->owner : "local-owner";
    sha256_update(
        &h, "temporal-owner-cache-v1:", sizeof("temporal-owner-cache-v1:") - 1);
    sha256_update(&h, owner, strlen(owner));
    sha256_update(&h, identity, 64);
    unsigned char digest[32];
    sha256_final(&h, digest);
    sha256_hex(digest, key);
    return NULL;
}
static void artifact(const char *identity, const SourcesTemporalInput *input,
                     char result[65]) {
    char material[256];
    int n = snprintf(material, sizeof(material), "temporal-capture-v1:%s:%s:%s",
                     identity, input->input_sha256, input->prepared_sha256);
    hash_bytes(material, (size_t)n, result);
}
static const char *snapshot_valid(sqlite3 *db, const SourcesConfig *cfg,
                                  const char *source, char **locator) {
    sqlite3_stmt *s = NULL;
    const char *error = NULL;
    *locator = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT b.bytes,c.locator FROM snapshots b JOIN source_claims c ON "
            "c.id=b.id WHERE c.id=? AND c.owner=? AND "
            "c.media='application/x-samosa-encoded-media' LIMIT 1",
            -1, &s, NULL) != SQLITE_OK)
        return "store_failed";
    sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, cfg->owner ? cfg->owner : "local-owner", -1,
                      SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_DONE)
        error = "unknown_source";
    else if (rc != SQLITE_ROW)
        error = "store_failed";
    else {
        int n = sqlite3_column_bytes(s, 0);
        const void *blob = sqlite3_column_blob(s, 0);
        const char *path = (const char *)sqlite3_column_text(s, 1);
        char actual[65];
        if (n <= 0 || (size_t)n > SOURCES_INPUT_MAX || !blob || !path ||
            !*path || strlen(path) > 4095)
            error = "corrupt_snapshot";
        else {
            hash_bytes(blob, (size_t)n, actual);
            if (strcmp(actual, source))
                error = "corrupt_snapshot";
            else if (!(*locator = strdup(path)))
                error = "resource_limit";
        }
    }
    sqlite3_finalize(s);
    return error ? error : sources_interrupted(cfg);
}
static const char *validate(const SourcesConfig *cfg, const char *body,
                            size_t bytes, const char *source,
                            const char *locator, size_t start, size_t end,
                            const char *prompt, const char *identity) {
    Json j;
    if (bytes > CAPTURE_MAX || !json_parse(&j, body, bytes) ||
        j.tokens[0].type != JT_OBJECT || j.tokens[0].count != 13 ||
        !number(&j, 0, "contract_version", 1) ||
        !equal(&j, 0, "source_id", source) ||
        !equal(&j, 0, "version", source) || !equal(&j, 0, "kind", "derived") ||
        !equal(&j, 0, "artifact_type", "video_observation") ||
        !equal(&j, 0, "prompt", prompt) || !equal(&j, 0, "recipe", RECIPE))
        return "corrupt_cache";
    int range = json_get(&j, 0, "range"), frames = json_get(&j, 0, "frames"),
        coverage = json_get(&j, 0, "coverage");
    if (range < 0 || j.tokens[range].type != JT_OBJECT ||
        j.tokens[range].count != 3 ||
        !number(&j, range, "start_us", start * 1000) ||
        !number(&j, range, "end_us", end * 1000) ||
        !equal(&j, range, "coordinate_space", "source_time_us") || frames < 0 ||
        j.tokens[frames].type != JT_ARRAY || j.tokens[frames].count < 1 ||
        j.tokens[frames].count > 16 || coverage < 0 ||
        j.tokens[coverage].type != JT_OBJECT || j.tokens[coverage].count != 2 ||
        !equal(&j, coverage, "status", "partial") ||
        !equal(&j, coverage, "reason", "sampled_video_analysis_uncertain"))
        return "corrupt_cache";
    SourcesTemporalInput input = {.start_us = start * 1000,
                                  .end_us = end * 1000};
    const char *error = NULL;
    for (int t = frames + 1; !error && t < j.tokens[frames].next;
         t = j.tokens[t].next) {
        SourcesPreparedFrame *f = &input.frames[input.count];
        int selection = json_get(&j, t, "selection");
        char *pixels = json_string(&j, json_get(&j, t, "rgba_sha256")),
             *ppm = json_string(&j, json_get(&j, t, "prepared_sha256"));
        f->prepared = json_string(&j, json_get(&j, t, "prepared_artifact_id"));
        f->decoder = object(&j, json_get(&j, t, "decoder"));
        f->preparation = object(&j, json_get(&j, t, "preparation"));
        if (j.tokens[t].type != JT_OBJECT || j.tokens[t].count != 9 ||
            !pin(pixels) || !pin(ppm) || !pin(f->prepared) || !f->decoder ||
            !f->preparation || selection < 0 ||
            j.tokens[selection].type != JT_OBJECT ||
            j.tokens[selection].count != 2 ||
            !json_uint(&j, json_get(&j, selection, "start_ms"), &f->start) ||
            !json_uint(&j, json_get(&j, selection, "end_ms"), &f->end) ||
            f->start < start || f->end > end ||
            !json_uint(&j, json_get(&j, t, "timestamp_us"), &f->timestamp) ||
            f->timestamp < input.start_us || f->timestamp >= input.end_us ||
            (input.count &&
             f->timestamp <= input.frames[input.count - 1].timestamp) ||
            !json_uint(&j, json_get(&j, t, "width"), &f->width) ||
            !json_uint(&j, json_get(&j, t, "height"), &f->height))
            error = "corrupt_cache";
        if (!error) {
            memcpy(f->input, pixels, 65);
            memcpy(f->ppm, ppm, 65);
            error = sources_media_frame_metadata_validate(
                cfg, f->decoder, strlen(f->decoder), f->start, f->end);
            if(error&&strcmp(error,"cancelled")&&strcmp(error,"deadline_exceeded"))error="corrupt_cache";
        }
        Json decoder, prep;
        if (!error &&
            (!json_parse(&decoder, f->decoder, strlen(f->decoder)) ||
             !json_parse(&prep, f->preparation, strlen(f->preparation)) ||
             decoder.tokens[0].type != JT_OBJECT || decoder.tokens[0].count != 20 ||
             prep.tokens[0].type != JT_OBJECT || prep.tokens[0].count != 4 ||
             !number(&decoder, 0, "timestamp_us", f->timestamp) ||
             !number(&decoder, 0, "width", f->width) ||
             !number(&decoder, 0, "height", f->height) ||
             !equal(&prep, 0, "locator", locator)))
            error = "corrupt_cache";
        if (!error) {
            int last = equal(&prep, 0, "reader", "visual-media-last-frame-v1");
            char *fingerprint =
                json_string(&decoder, json_get(&decoder, 0, "fingerprint"));
            char material[256], expected[65];
            if (!pin(fingerprint) ||
                !equal(&prep, 0, "reader",
                       last ? "visual-media-last-frame-v1"
                            : "visual-media-frame-v1") ||
                !equal(&prep, 0, "extractor_fingerprint", fingerprint) ||
                !equal(&prep, 0, "derivation",
                       last ? "native_decode_scale_rgba_last"
                            : "native_decode_scale_rgba"))
                error = "corrupt_cache";
            else {
                int n = snprintf(material, sizeof(material), "%s:%s:%s:%zu:%zu",
                                 last ? "visual-media-last-frame-v1"
                                      : "visual-media-frame-v1",
                                 fingerprint, source, f->start, f->end);
                hash_bytes(material, (size_t)n, expected);
                if (strcmp(expected, f->prepared))
                    error = "corrupt_cache";
            }
            if (!error && input.count + 1 == (size_t)j.tokens[frames].count &&
                (!last || f->end != end))
                error = "corrupt_cache";
            free(fingerprint);
        }
        free(pixels);
        free(ppm);
        if (!error)
            input.count++;
    }
    char *response = object(&j, json_get(&j, 0, "inference"));
    if (!error)
        error = sources_temporal_hashes(&input);
    if (!error) {
        const char *checked=response?sources_temporal_response(cfg,&input,response,strlen(response)):"corrupt_cache";
        if(checked)error=(!strcmp(checked,"cancelled")||!strcmp(checked,"deadline_exceeded"))?checked:"corrupt_cache";
    }
    if (!error) {
        Json reply;
        if (!json_parse(&reply, response, strlen(response)))
            error = "corrupt_cache";
        else {
            char *text = json_string(&reply, json_get(&reply, 0, "text"));
            if (!text || !equal(&j, 0, "text", text))
                error = "corrupt_cache";
            free(text);
        }
    }
    if (!error) {
        char expected[65];
        artifact(identity, &input, expected);
        if (!equal(&j, 0, "artifact_id", expected))
            error = "corrupt_cache";
    }
    free(response);
    sources_temporal_input_release(&input);
    return error ? error : sources_interrupted(cfg);
}
static const char *cache_get(sqlite3 *db, const SourcesConfig *cfg,
                             const char *key, const char *source,
                             const char *locator, size_t start, size_t end,
                             const char *prompt, const char *identity,
                             char **body, size_t *bytes) {
    *body = NULL;
    *bytes = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT response,digest FROM prepared_artifacts WHERE key=?",
            -1, &s, NULL) != SQLITE_OK)
        return "store_failed";
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    const char *error = NULL;
    if (rc == SQLITE_ROW) {
        int n = sqlite3_column_bytes(s, 0);
        const char *value = (const char *)sqlite3_column_text(s, 0),
                   *digest = (const char *)sqlite3_column_text(s, 1);
        char actual[65];
        if (n <= 0 || n > (int)CAPTURE_MAX || !value ||
            strlen(value) != (size_t)n || !pin(digest))
            error = "corrupt_cache";
        else {
            hash_bytes(value, (size_t)n, actual);
            if (strcmp(actual, digest))
                error = "corrupt_cache";
            else
                error = validate(cfg, value, (size_t)n, source, locator, start,
                                 end, prompt, identity);
            if (!error) {
                *body = strdup(value);
                if (!*body)
                    error = "resource_limit";
                else
                    *bytes = (size_t)n;
            }
        }
    } else if (rc != SQLITE_DONE)
        error = "store_failed";
    sqlite3_finalize(s);
    return error;
}
static const char *project(const char *source, size_t start, size_t end,
                           const char *prompt, const char *identity,
                           const SourcesTemporalInput *input,
                           const char *response, size_t response_bytes,
                           char **body, size_t *bytes) {
    Json reply;
    if (!json_parse(&reply, response, response_bytes))
        return "inference_response_invalid";
    char *text = json_string(&reply, json_get(&reply, 0, "text"));
    if (!text)
        return "inference_response_invalid";
    char id[65];
    artifact(identity, input, id);
    FILE *out = open_memstream(body, bytes);
    if (!out) {
        free(text);
        return "resource_limit";
    }
    fprintf(out,
            "{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\","
            "\"artifact_id\":\"%s\",\"kind\":\"derived\",\"artifact_type\":"
            "\"video_observation\",\"recipe\":\"" RECIPE "\",\"text\":",
            source, source, id);
    json_quote(out, text, strlen(text));
    fputs(",\"prompt\":", out);
    json_quote(out, prompt, strlen(prompt));
    fprintf(out,
            ",\"range\":{\"start_us\":%zu,\"end_us\":%zu,\"coordinate_space\":"
            "\"source_time_us\"},\"frames\":[",
            start * 1000, end * 1000);
    for (size_t k = 0; k < input->count; k++) {
        const SourcesPreparedFrame *f = &input->frames[k];
        if (k)
            fputc(',', out);
        fprintf(out,
                "{\"prepared_artifact_id\":\"%s\",\"timestamp_us\":%zu,"
                "\"width\":%zu,\"height\":%zu,\"rgba_sha256\":\"%s\","
                "\"prepared_sha256\":\"%s\",\"selection\":{\"start_ms\":%zu,"
                "\"end_ms\":%zu},\"decoder\":%s,\"preparation\":%s}",
                f->prepared, f->timestamp, f->width, f->height, f->input,
                f->ppm, f->start, f->end, f->decoder, f->preparation);
    }
    fputs("],\"coverage\":{\"status\":\"partial\",\"reason\":\"sampled_video_"
          "analysis_uncertain\"},\"inference\":",
          out);
    fwrite(response, 1, response_bytes, out);
    fputs("}\n", out);
    free(text);
    if (fclose(out) || *bytes > CAPTURE_MAX) {
        free(*body);
        *body = NULL;
        *bytes = 0;
        return "resource_limit";
    }
    return NULL;
}
int sources_temporal_dispatch(const SourcesConfig *configuration,
                              const Json *request, sqlite3 *db, FILE *out) {
    SourcesConfig scoped = *configuration;
    struct timespec now;
    if (!scoped.deadline_monotonic) {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) {
            fputs("{\"contract_version\":1,\"error\":{\"code\":\"state_"
                  "unavailable\"}}\n",
                  out);
            return 1;
        }
        scoped.deadline_monotonic =
            (double)now.tv_sec + (double)now.tv_nsec / 1e9 + 120;
    }
    const SourcesConfig *cfg = &scoped;
    char *source = json_string(request, json_get(request, 0, "source_id")),
         *version = json_string(request, json_get(request, 0, "version")),
         *prompt = json_string(request, json_get(request, 0, "prompt"));
    if (json_get(request, 0, "prompt") < 0)
        prompt =
            strdup("Describe the changes across these sampled video frames.");
    size_t start = 0, end = 0;
    const char *error = NULL;
    char identity[65], key[65], *locator = NULL, *body = NULL,
                                *invocation = NULL, *response = NULL;
    size_t bytes = 0, invocation_bytes = 0, response_bytes = 0;
    SourcesTemporalInput input = {0};
    if (!pin(source) || !pin(version) || !prompt || !*prompt ||
        strlen(prompt) > 4096 ||
        !utf8_valid((const unsigned char *)prompt, strlen(prompt)) ||
        !json_uint(request, json_get(request, 0, "start_ms"), &start) ||
        !json_uint(request, json_get(request, 0, "end_ms"), &end))
        error = "invalid_request";
    else if (strcmp(source, version))
        error = "stale_source";
    else if (start >= end || end > 14400000)
        error = "invalid_range";
    if (!error)
        error = selector(cfg, source, start, end, prompt, identity, key);
    if (!error)
        error = snapshot_valid(db, cfg, source, &locator);
    if (!error)
        error = cache_get(db, cfg, key, source, locator, start, end, prompt,
                          identity, &body, &bytes);
    int cached = body != NULL;
    if (!error && !cached)
        error = sources_cache_capacity(db, cfg, 0);
    if(!error&&!cached&&!cfg->temporal_inference->call)error="inference_unavailable";
    if (!error && !cached)
        error =
            sources_temporal_prepare(cfg, db, source, start, end, prompt,
                                     &input, &invocation, &invocation_bytes);
    if (!error && !cached) {
        const SourcesInferenceConfig *provider = cfg->temporal_inference;
        SourcesInferenceControl control = {cfg->deadline_monotonic,
                                           cfg->cancelled, cfg->cancellation};
        if (!provider->call)
            error = "inference_unavailable";
        else
            error =
                provider->call(provider->context, &control, invocation,
                               invocation_bytes, &response, &response_bytes);
        if (error && strcmp(error, "cancelled") &&
            strcmp(error, "deadline_exceeded"))
            error = "inference_unavailable";
        if (!error &&
            (!response || !response_bytes || response_bytes > CAPTURE_MAX ||
             strlen(response) != response_bytes))
            error = "inference_response_invalid";
        if (!error)
            error = sources_interrupted(cfg);
        if (!error) {
            Json reply;
            if (!json_parse(&reply, response, response_bytes))
                error = "inference_response_invalid";
            else if (json_get(&reply, 0, "error") >= 0) {
                char *code = json_string(
                    &reply,
                    json_get(&reply, json_get(&reply, 0, "error"), "code"));
                error = code && !strcmp(code, "cancelled") ? "cancelled"
                        : code && !strcmp(code, "deadline_exceeded")
                            ? "deadline_exceeded"
                        : code && !strcmp(code, "busy") ? "inference_busy"
                                                        : "inference_failed";
                free(code);
            }
        }
        if (!error)
            error = sources_temporal_response(cfg, &input, response,
                                              response_bytes);
        if (!error)
            error = project(source, start, end, prompt, identity, &input,
                            response, response_bytes, &body, &bytes);
        if (!error)
            error = validate(cfg, body, bytes, source, locator, start, end,
                             prompt, identity);
    }
    if (!error && !cached) {
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            error = "store_busy";
        else {
            char *old = NULL;
            size_t old_bytes = 0;
            error = cache_get(db, cfg, key, source, locator, start, end, prompt,
                              identity, &old, &old_bytes);
            if (!error && old) {
                if (old_bytes != bytes || memcmp(old, body, bytes))
                    error = "cache_conflict";
            } else if (!error) {
                error = sources_cache_capacity(db, cfg, bytes);
                sqlite3_stmt *s = NULL;
                if (!error &&
                    sqlite3_prepare_v2(
                        db, "INSERT INTO prepared_artifacts VALUES(?,?,?)", -1,
                        &s, NULL) != SQLITE_OK)
                    error = "store_failed";
                if (!error) {
                    char digest[65];
                    hash_bytes(body, bytes, digest);
                    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(s, 2, body, (int)bytes, SQLITE_TRANSIENT);
                    sqlite3_bind_text(s, 3, digest, -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(s) != SQLITE_DONE)
                        error = "store_failed";
                }
                sqlite3_finalize(s);
            }
            free(old);
            if (!error)
                error = sources_interrupted(cfg);
            if (error)
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            else if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) !=
                     SQLITE_OK) {
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                error = "store_failed";
            }
        }
    }
    if (error)
        fprintf(out, "{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",
                error);
    else
        fwrite(body, 1, bytes, out);
    sources_temporal_input_release(&input);
    free(source);
    free(version);
    free(prompt);
    free(locator);
    free(body);
    free(invocation);
    free(response);
    return error ? 1 : 0;
}
