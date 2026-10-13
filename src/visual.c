#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "audio.h"
#include "cache.h"
#include "control.h"
#include "native.h"
#include "sha256.h"
#include "visual.h"
#include <time.h>
#define RESULT_MAX 65536u
static const char *default_prompt = "Describe the scene and visible text in this frame.";
typedef SourcesPreparedFrame Frame;
static int sha(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static int equal(const Json *j, int object, const char *name, const char *expected) {
    char *value = json_string(j, json_get(j, object, name));
    int same = value && expected && !strcmp(value, expected);
    free(value);
    return same;
}
static int number(const Json *j, int object, const char *name, size_t expected) {
    size_t value = 0;
    return json_uint(j, json_get(j, object, name), &value) && value == expected;
}
static char *object(const Json *j, int token) {
    if (token < 0 || j->tokens[token].type != JT_OBJECT)
        return NULL;
    size_t n = j->tokens[token].end - j->tokens[token].begin;
    char *s = malloc(n + 1);
    if (s) {
        memcpy(s, j->text + j->tokens[token].begin, n);
        s[n] = 0;
    }
    return s;
}
static int configured(const SourcesConfig *cfg) {
    const SourcesInferenceConfig *i = cfg ? cfg->visual_inference : NULL;
    if (!i || !i->provider_id || !*i->provider_id || strlen(i->provider_id) > 128 ||
        !sha(i->engine_sha256) || !sha(i->model_sha256))
        return 0;
    for (const unsigned char *p = (const unsigned char *)i->provider_id; *p; p++)
        if (*p > 127 || (!isalnum(*p) && !strchr("-_:.", *p)))
            return 0;
    return 1;
}
void sources_prepared_frame_release(SourcesPreparedFrame *f) {
    free(f->source);
    free(f->prepared);
    free(f->data);
    free(f->decoder);
    free(f->preparation);
    memset(f, 0, sizeof(*f));
}
static void release(Frame *f) { sources_prepared_frame_release(f); }
static void identity(const SourcesConfig *cfg, Frame *frame, const char *prompt) {
    Sha256 hash;
    sha256_init(&hash);
    const SourcesInferenceConfig *i = cfg->visual_inference;
    const char *parts[] = {"source-visual-observation-v1:",
                           frame->source,
                           frame->prepared,
                           frame->input,
                           frame->ppm,
                           i->provider_id,
                           i->engine_sha256,
                           i->model_sha256,
                           prompt,
                           frame->decoder,
                           frame->preparation};
    for (size_t k = 0; k < sizeof(parts) / sizeof(*parts); k++) {
        sha256_update(&hash, parts[k], strlen(parts[k]));
        sha256_update(&hash, "\n", 1);
    }
    unsigned char digest[32];
    sha256_final(&hash, digest);
    sha256_hex(digest, frame->artifact);
}
const char *sources_prepared_frame(const SourcesConfig *cfg, const char *body, size_t bytes,
                                  SourcesPreparedFrame *frame) {
    Json j;
    if (bytes > 2097152u || !json_parse(&j, body, bytes) || j.tokens[0].type != JT_OBJECT ||
        j.tokens[0].count != 13 || !number(&j, 0, "contract_version", 1) ||
        !equal(&j, 0, "kind", "derived") || !equal(&j, 0, "artifact_type", "prepared_visual") ||
        !equal(&j, 0, "media_type", "application/x-samosa-rgba") ||
        !equal(&j, 0, "encoding", "base64"))
        return "corrupt_cache";
    frame->source = json_string(&j, json_get(&j, 0, "source_id"));
    frame->prepared = json_string(&j, json_get(&j, 0, "artifact_id"));
    frame->data = json_string(&j, json_get(&j, 0, "data"));
    int range = json_get(&j, 0, "range"), decoder = json_get(&j, 0, "decoder"),
        provenance = json_get(&j, 0, "provenance"), coverage = json_get(&j, 0, "coverage");
    if (!sha(frame->source) || !sha(frame->prepared) || !frame->data ||
        !equal(&j, 0, "version", frame->source) ||
        !json_uint(&j, json_get(&j, range, "start_ms"), &frame->start) ||
        !json_uint(&j, json_get(&j, range, "end_ms"), &frame->end) || frame->start >= frame->end ||
        frame->end - frame->start > 30000 || frame->end > 14400000 ||
        !equal(&j, range, "coordinate_space", "source_time_ms") || decoder < 0 ||
        j.tokens[decoder].count != 20 || provenance < 0 || j.tokens[provenance].count != 4 ||
        !equal(&j, coverage, "status", "partial") ||
        !equal(&j, coverage, "reason", "media_preparation_only") ||
        (!equal(&j, provenance, "reader", "visual-media-frame-v1") &&
         !equal(&j, provenance, "reader", "visual-media-last-frame-v1")))
        return "corrupt_cache";
    int last=equal(&j,provenance,"reader","visual-media-last-frame-v1");
    if(!equal(&j,provenance,"derivation",last?"native_decode_scale_rgba_last":"native_decode_scale_rgba"))return "corrupt_cache";
    char *fingerprint = json_string(&j, json_get(&j, decoder, "fingerprint")),
         *locator = json_string(&j, json_get(&j, provenance, "locator"));
    int valid = sha(fingerprint) && locator && *locator && strlen(locator) <= 4095 &&
                equal(&j, provenance, "extractor_fingerprint", fingerprint);
    char material[256], expected[65];
    if (valid) {
        int n = snprintf(material, sizeof(material), "%s:%s:%s:%zu:%zu",
                         last?"visual-media-last-frame-v1":"visual-media-frame-v1",fingerprint, frame->source, frame->start, frame->end);
        hash_bytes(material, (size_t)n, expected);
        valid = !strcmp(expected, frame->prepared);
    }
    free(fingerprint);
    free(locator);
    if (!valid)
        return "corrupt_cache";
    frame->decoder = object(&j, decoder);
    frame->preparation = object(&j, provenance);
    if (!frame->decoder || !frame->preparation)
        return "resource_limit";
    char *dto = NULL;
    size_t dto_bytes = 0;
    FILE *f = open_memstream(&dto, &dto_bytes);
    if (!f)
        return "resource_limit";
    fwrite(frame->decoder, 1, strlen(frame->decoder) - 1, f);
    fputs(",\"encoding\":\"base64\",\"data\":", f);
    json_quote(f, frame->data, strlen(frame->data));
    fputc('}', f);
    const char *error = fclose(f)
                            ? "resource_limit"
                            : sources_media_frame_hashes(cfg, dto, dto_bytes, frame->start,
                                                         frame->end, frame->input, frame->ppm);
    free(dto);
    if (error)
        return !strcmp(error, "cancelled") || !strcmp(error, "deadline_exceeded") ||
                       !strcmp(error, "resource_limit")
                   ? error
                   : "corrupt_cache";
    if (!json_uint(&j, json_get(&j, decoder, "width"), &frame->width) ||
        !json_uint(&j, json_get(&j, decoder, "height"), &frame->height) ||
        !json_uint(&j, json_get(&j, decoder, "timestamp_us"), &frame->timestamp))
        return "corrupt_cache";
    return sources_interrupted(cfg);
}
static const char *prepared_frame(const SourcesConfig *cfg,const char *body,size_t bytes,
                                  const char *prompt,Frame *frame) {
    const char *error=sources_prepared_frame(cfg,body,bytes,frame);
    Json provenance;
    if(!error&&(!json_parse(&provenance,frame->preparation,strlen(frame->preparation))||
       !equal(&provenance,0,"reader","visual-media-frame-v1")))error="corrupt_cache";
    if(!error)identity(cfg,frame,prompt);
    return error;
}
static const char *project(const SourcesConfig *cfg, const char *body, size_t bytes, Frame *frame,
                           const char *prompt, char **result, size_t *length) {
    Json j;
    const SourcesInferenceConfig *i = cfg->visual_inference;
    if (bytes > RESULT_MAX || !json_parse(&j, body, bytes) || j.tokens[0].type != JT_OBJECT ||
        json_get(&j, 0, "error") >= 0 || !number(&j, 0, "contract_version", 1) ||
        !equal(&j, 0, "provider_id", i->provider_id) || !equal(&j, 0, "kind", "derived") ||
        !equal(&j, 0, "artifact_type", "visual_observation") ||
        !equal(&j, 0, "input_sha256", frame->input) || !number(&j, 0, "width", frame->width) ||
        !number(&j, 0, "height", frame->height))
        return "inference_response_invalid";
    int p = json_get(&j, 0, "provenance"), coverage = json_get(&j, 0, "coverage"),
        usage = json_get(&j, 0, "usage");
    size_t in = 0, out = 0;
    if (!equal(&j, coverage, "status", "partial") ||
        !equal(&j, coverage, "reason", "visual_analysis_uncertain") ||
        !equal(&j, p, "engine", "visionpsy") || !equal(&j, p, "engine_sha256", i->engine_sha256) ||
        !equal(&j, p, "model_sha256", i->model_sha256) ||
        !equal(&j, p, "prepared_image_sha256", frame->ppm) ||
        !equal(&j, p, "alpha_policy", "rgba8_composited_on_white") ||
        !equal(&j, p, "coordinate_space", "input_pixels") ||
        !json_uint(&j, json_get(&j, usage, "input_tokens"), &in) || in > 131072 ||
        !json_uint(&j, json_get(&j, usage, "output_tokens"), &out) || out > 128)
        return "inference_response_invalid";
    char *text = json_string(&j, json_get(&j, 0, "text"));
    if (!text || !*text || strlen(text) > 16384) {
        free(text);
        return "inference_response_invalid";
    }
    FILE *f = open_memstream(result, length);
    if (!f) {
        free(text);
        return "resource_limit";
    }
    fprintf(f,
            "{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\",\"artifact_id\":\"%"
            "s\",\"kind\":\"derived\",\"artifact_type\":\"visual_observation\",\"text\":",
            frame->source, frame->source, frame->artifact);
    json_quote(f, text, strlen(text));
    free(text);
    fprintf(f,
            ",\"frame\":{\"timestamp_us\":%zu,\"coordinate_space\":\"source_time_us\",\"width\":%"
            "zu,\"height\":%zu},\"range\":{\"start_ms\":%zu,\"end_ms\":%zu,\"coordinate_space\":"
            "\"source_time_ms\"},\"usage\":{\"input_tokens\":%zu,\"output_tokens\":%zu},"
            "\"coverage\":{\"status\":\"partial\",\"reason\":\"visual_analysis_uncertain\","
            "\"sampled_frames\":1,\"temporal_scope\":\"selected_frame_only\"},"
            "\"provenance\":{\"reader\":\"public-inference-visual-v1\",\"prepared_artifact_id\":\"%"
            "s\",\"input_sha256\":\"%s\",\"prepared_image_sha256\":\"%s\",\"provider_id\":\"%s\","
            "\"engine\":\"visionpsy\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"alpha_"
            "policy\":\"rgba8_composited_on_white\",\"derivation\":\"visual_analysis\",\"prompt\":",
            frame->timestamp, frame->width, frame->height, frame->start, frame->end, in, out,
            frame->prepared, frame->input, frame->ppm, i->provider_id, i->engine_sha256,
            i->model_sha256);
    json_quote(f, prompt, strlen(prompt));
    fprintf(f, ",\"decoder\":%s,\"preparation\":%s}}\n", frame->decoder, frame->preparation);
    return fclose(f) || *length > RESULT_MAX ? "resource_limit" : sources_interrupted(cfg);
}
const char *sources_visual_project(const SourcesConfig *cfg, const char *prepared,
                                   size_t prepared_bytes, const char *response,
                                   size_t response_bytes, const char *prompt, char **result,
                                   size_t *length) {
    *result = NULL;
    *length = 0;
    if (!configured(cfg) || !prompt || !*prompt || strlen(prompt) > 4096 ||
        !utf8_valid((unsigned char *)prompt, strlen(prompt)))
        return "invalid_configuration";
    Frame frame = {0};
    const char *error = sources_interrupted(cfg);
    if (!error)
        error = prepared_frame(cfg, prepared, prepared_bytes, prompt, &frame);
    if (!error)
        error = project(cfg, response, response_bytes, &frame, prompt, result, length);
    release(&frame);
    if (error) {
        free(*result);
        *result = NULL;
        *length = 0;
    }
    return error;
}
/* Retained capture validation never reruns a model. Pixel and PPM identities
 * are compared with fresh prepared pixels on cache access below. */
const char *sources_visual_capture_validate(const SourcesConfig *cfg, const char *body,
                                            size_t bytes) {
    if (!configured(cfg))
        return "inference_unconfigured";
    Json j;
    if (bytes > RESULT_MAX || !json_parse(&j, body, bytes) || j.tokens[0].type != JT_OBJECT ||
        j.tokens[0].count != 12 || !number(&j, 0, "contract_version", 1) ||
        !equal(&j, 0, "kind", "derived") || !equal(&j, 0, "artifact_type", "visual_observation"))
        return "corrupt_cache";
    int p = json_get(&j, 0, "provenance"), r = json_get(&j, 0, "range"),
        f = json_get(&j, 0, "frame"), u = json_get(&j, 0, "usage"), c = json_get(&j, 0, "coverage");
    const SourcesInferenceConfig *i = cfg->visual_inference;
    Frame frame = {0};
    frame.source = json_string(&j, json_get(&j, 0, "source_id"));
    frame.prepared = json_string(&j, json_get(&j, p, "prepared_artifact_id"));
    frame.decoder = object(&j, json_get(&j, p, "decoder"));
    frame.preparation = object(&j, json_get(&j, p, "preparation"));
    char *input = json_string(&j, json_get(&j, p, "input_sha256")),
         *ppm = json_string(&j, json_get(&j, p, "prepared_image_sha256")),
         *prompt = json_string(&j, json_get(&j, p, "prompt")),
         *text = json_string(&j, json_get(&j, 0, "text"));
    size_t in = 0, out = 0;
    int valid =
        sha(frame.source) && sha(frame.prepared) && sha(input) && sha(ppm) && frame.decoder &&
        frame.preparation && prompt && *prompt && strlen(prompt) <= 4096 && text && *text &&
        strlen(text) <= 16384 && equal(&j, 0, "version", frame.source) &&
        equal(&j, r, "coordinate_space", "source_time_ms") &&
        json_uint(&j, json_get(&j, r, "start_ms"), &frame.start) &&
        json_uint(&j, json_get(&j, r, "end_ms"), &frame.end) && frame.start < frame.end &&
        frame.end - frame.start <= 30000 && frame.end <= 14400000 &&
        equal(&j, f, "coordinate_space", "source_time_us") &&
        json_uint(&j, json_get(&j, f, "timestamp_us"), &frame.timestamp) &&
        frame.timestamp >= frame.start * 1000 && frame.timestamp < frame.end * 1000 &&
        json_uint(&j, json_get(&j, f, "width"), &frame.width) && frame.width &&
        frame.width <= 512 && json_uint(&j, json_get(&j, f, "height"), &frame.height) &&
        frame.height && frame.height <= 512 &&
        json_uint(&j, json_get(&j, u, "input_tokens"), &in) && in <= 131072 &&
        json_uint(&j, json_get(&j, u, "output_tokens"), &out) && out <= 128 &&
        equal(&j, c, "status", "partial") && equal(&j, c, "reason", "visual_analysis_uncertain") &&
        number(&j, c, "sampled_frames", 1) &&
        equal(&j, c, "temporal_scope", "selected_frame_only") &&
        equal(&j, p, "reader", "public-inference-visual-v1") &&
        equal(&j, p, "provider_id", i->provider_id) && equal(&j, p, "engine", "visionpsy") &&
        equal(&j, p, "engine_sha256", i->engine_sha256) &&
        equal(&j, p, "model_sha256", i->model_sha256) &&
        equal(&j, p, "alpha_policy", "rgba8_composited_on_white") &&
        equal(&j, p, "derivation", "visual_analysis");
    if (valid) {
        memcpy(frame.input, input, 65);
        memcpy(frame.ppm, ppm, 65);
        identity(cfg, &frame, prompt);
        valid = equal(&j, 0, "artifact_id", frame.artifact);
        Json decoder, prep;
        if (valid)
            valid = json_parse(&decoder, frame.decoder, strlen(frame.decoder)) &&
                    json_parse(&prep, frame.preparation, strlen(frame.preparation)) &&
                    number(&decoder, 0, "width", frame.width) &&
                    number(&decoder, 0, "height", frame.height) &&
                    number(&decoder, 0, "timestamp_us", frame.timestamp) &&
                    number(&decoder, 0, "start_ms", frame.start) &&
                    number(&decoder, 0, "end_ms", frame.end) &&
                    equal(&prep, 0, "reader", "visual-media-frame-v1") &&
                    equal(&prep, 0, "derivation", "native_decode_scale_rgba");
        if (valid) {
            const char *error = sources_media_frame_metadata_validate(
                cfg, frame.decoder, strlen(frame.decoder), frame.start, frame.end);
            if (error) {
                release(&frame);
                free(input);
                free(ppm);
                free(prompt);
                free(text);
                return !strcmp(error, "cancelled") || !strcmp(error, "deadline_exceeded")
                           ? error
                           : "corrupt_cache";
            }
            char *fingerprint = json_string(&decoder, json_get(&decoder, 0, "fingerprint")),
                 *locator = json_string(&prep, json_get(&prep, 0, "locator"));
            valid = prep.tokens[0].count == 4 && locator && *locator && strlen(locator) <= 4095 &&
                    fingerprint && equal(&prep, 0, "extractor_fingerprint", fingerprint);
            if (valid) {
                char material[256], expected[65];
                int n = snprintf(material, sizeof(material), "visual-media-frame-v1:%s:%s:%zu:%zu",
                                 fingerprint, frame.source, frame.start, frame.end);
                hash_bytes(material, (size_t)n, expected);
                valid = !strcmp(expected, frame.prepared);
            }
            free(fingerprint);
            free(locator);
        }
    }
    release(&frame);
    free(input);
    free(ppm);
    free(prompt);
    free(text);
    return valid ? sources_interrupted(cfg) : "corrupt_cache";
}
static int fail(FILE *out, const char *error) {
    fprintf(out, "{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n", error);
    return 1;
}
static const char *cache_get(sqlite3 *db, const SourcesConfig *cfg, const char *key,
                             const char *artifact, char **body, size_t *bytes) {
    *body = NULL;
    *bytes = 0;
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, "SELECT response,digest FROM prepared_artifacts WHERE key=?", -1, &s,
                           NULL) != SQLITE_OK)
        return "store_failed";
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    const char *error = NULL;
    if (rc == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(s, 0),
                   *proof = (const char *)sqlite3_column_text(s, 1);
        int n = sqlite3_column_bytes(s, 0);
        char digest[65];
        if (!value || !sha(proof) || n <= 0 || n > (int)RESULT_MAX || strlen(value) != (size_t)n)
            error = "corrupt_cache";
        else {
            hash_bytes(value, (size_t)n, digest);
            if (strcmp(digest, proof))
                error = "corrupt_cache";
            else {
                error = sources_visual_capture_validate(cfg, value, (size_t)n);
                Json j;
                if (!error &&
                    (!json_parse(&j, value, (size_t)n) || !equal(&j, 0, "artifact_id", artifact)))
                    error = "corrupt_cache";
                if (!error) {
                    *body = strdup(value);
                    if (!*body)
                        error = "resource_limit";
                    else
                        *bytes = (size_t)n;
                }
            }
        }
    } else if (rc != SQLITE_DONE)
        error = "store_failed";
    sqlite3_finalize(s);
    return error;
}
static int dispatch(const SourcesConfig *cfg, const Json *request, sqlite3 *db, FILE *out,
                    int persistent) {
    if (!configured(cfg))
        return fail(out, "inference_unconfigured");
    SourcesConfig scoped = *cfg;
    struct timespec now;
    if (!scoped.deadline_monotonic) {
        if (clock_gettime(CLOCK_MONOTONIC, &now))
            return fail(out, "state_unavailable");
        scoped.deadline_monotonic = (double)now.tv_sec + (double)now.tv_nsec / 1e9 + 60;
    }
    cfg = &scoped;
    char *source = json_string(request, json_get(request, 0, "source_id")),
         *version = json_string(request, json_get(request, 0, "version")),
         *prompt = json_string(request, json_get(request, 0, "prompt"));
    if (json_get(request, 0, "prompt") < 0)
        prompt = strdup(default_prompt);
    size_t start = 0, end = 0;
    const char *error = NULL;
    if (!sha(source) || !sha(version) || !prompt || !*prompt || strlen(prompt) > 4096 ||
        !json_uint(request, json_get(request, 0, "start_ms"), &start) ||
        !json_uint(request, json_get(request, 0, "end_ms"), &end))
        error = "invalid_request";
    else if (strcmp(source, version))
        error = "stale_source";
    else if (start >= end || end - start > 30000 || end > 14400000)
        error = "invalid_range";
    char *prepared = NULL, *body = NULL, *response = NULL, *invocation = NULL;
    size_t prepared_bytes = 0, bytes = 0, response_bytes = 0, invocation_bytes = 0;
    Frame frame = {0};
    FILE *f = error ? NULL : open_memstream(&prepared, &prepared_bytes);
    if (!error && !f)
        error = "resource_limit";
    if (f) {
        char input[512];
        snprintf(input, sizeof(input),
                 "{\"contract_version\":1,\"op\":\"prepare_visual\",\"source_id\":\"%s\","
                 "\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu}",
                 source, version, start, end);
        Json j;
        json_parse(&j, input, strlen(input));
        int rc = persistent ? sources_media_prepare(cfg, &j, db, f)
                            : sources_media_prepare_transient(cfg, &j, db, f);
        if (fclose(f))
            error = "resource_limit";
        else if (rc) {
            fwrite(prepared, 1, prepared_bytes, out);
            free(prepared);
            free(source);
            free(version);
            free(prompt);
            return 1;
        }
    }
    if (!error)
        error = prepared_frame(cfg, prepared, prepared_bytes, prompt, &frame);
    char key[65];
    if (!error) {
        Sha256 h;
        sha256_init(&h);
        const char *owner = cfg->owner ? cfg->owner : "local-owner";
        sha256_update(&h, "visual-owner-cache-v1:", 21);
        sha256_update(&h, owner, strlen(owner));
        sha256_update(&h, frame.artifact, 64);
        unsigned char digest[32];
        sha256_final(&h, digest);
        sha256_hex(digest, key);
        error = cache_get(db, cfg, key, frame.artifact, &body, &bytes);
    }
    int cached = body != NULL;
    if (!error && !cached && persistent)
        error = sources_cache_capacity(db, cfg, 0);
    if (!error && !cached) {
        if (!cfg->visual_inference->call)
            error = "inference_unavailable";
        else {
            f = open_memstream(&invocation, &invocation_bytes);
            if (!f)
                error = "resource_limit";
            else {
                fprintf(f,
                        "{\"contract_version\":1,\"op\":\"analyze_image\",\"provider_id\":\"%s\","
                        "\"width\":%zu,\"height\":%zu,\"max_output_tokens\":128,\"prompt\":",
                        cfg->visual_inference->provider_id, frame.width, frame.height);
                json_quote(f, prompt, strlen(prompt));
                fputs(",\"image_rgba_base64\":", f);
                json_quote(f, frame.data, strlen(frame.data));
                fputs(",\"timeout_ms\":60000}", f);
                if (fclose(f))
                    error = "resource_limit";
            }
        }
    }
    if (!error && !cached) {
        SourcesInferenceControl control = {cfg->deadline_monotonic, cfg->cancelled,
                                           cfg->cancellation};
        error = sources_interrupted(cfg);
        if (!error)
            error =
                cfg->visual_inference->call(cfg->visual_inference->context, &control, invocation,
                                            invocation_bytes, &response, &response_bytes);
        if (error && strcmp(error, "cancelled") && strcmp(error, "deadline_exceeded"))
            error = "inference_unavailable";
        if (!error)
            error = sources_interrupted(cfg);
        if (!error && (!response || !response_bytes || response_bytes > RESULT_MAX ||
                       strlen(response) != response_bytes))
            error = "inference_response_invalid";
        if (!error) {
            Json reply;
            if (!json_parse(&reply, response, response_bytes))
                error = "inference_response_invalid";
            else if (json_get(&reply, 0, "error") >= 0) {
                char *code =
                    json_string(&reply, json_get(&reply, json_get(&reply, 0, "error"), "code"));
                error = !code                                ? "inference_response_invalid"
                        : !strcmp(code, "cancelled")         ? "cancelled"
                        : !strcmp(code, "deadline_exceeded") ? "deadline_exceeded"
                        : !strcmp(code, "busy")              ? "inference_busy"
                                                             : "inference_failed";
                free(code);
            }
        }
        if (!error)
            error = project(cfg, response, response_bytes, &frame, prompt, &body, &bytes);
        if (!error)
            error = sources_visual_capture_validate(cfg, body, bytes);
    }
    if (!error && !cached && persistent) {
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
            error = "store_busy";
        else {
            char *old = NULL;
            size_t old_bytes = 0;
            error = cache_get(db, cfg, key, frame.artifact, &old, &old_bytes);
            if (!error && old) {
                if (old_bytes != bytes || memcmp(old, body, bytes))
                    error = "cache_conflict";
            } else if (!error) {
                error = sources_cache_capacity(db, cfg, bytes);
                sqlite3_stmt *s = NULL;
                if (!error && sqlite3_prepare_v2(db, "INSERT INTO prepared_artifacts VALUES(?,?,?)",
                                                 -1, &s, NULL) != SQLITE_OK)
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
            else if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
                error = "store_failed";
            }
        }
    }
    release(&frame);
    free(prepared);
    free(source);
    free(version);
    free(prompt);
    free(response);
    free(invocation);
    if (error) {
        free(body);
        return fail(out, error);
    }
    fwrite(body, 1, bytes, out);
    free(body);
    return 0;
}
int sources_visual_dispatch(const SourcesConfig *cfg, const Json *request, sqlite3 *db, FILE *out) {
    return dispatch(cfg, request, db, out, 1);
}
int sources_visual_window(const SourcesConfig *cfg, const Json *request, sqlite3 *db, FILE *out) {
    return dispatch(cfg, request, db, out, 0);
}

const char *sources_visual_recipe(const SourcesConfig *cfg,char recipe[65]) {
    if(!configured(cfg))return "inference_unconfigured";
    const SourcesInferenceConfig *i=cfg->visual_inference;
    Sha256 hash;sha256_init(&hash);
    const char *parts[]={"visual-index-model-v1:stride30000:rgba512:tokens128:",i->provider_id,i->engine_sha256,i->model_sha256,default_prompt};
    for(size_t p=0;p<5;p++){sha256_update(&hash,parts[p],strlen(parts[p]));sha256_update(&hash,"\n",1);}
    unsigned char digest[32];sha256_final(&hash,digest);sha256_hex(digest,recipe);
    return sources_interrupted(cfg);
}
const char *sources_visual_window_plan(const SourcesConfig *cfg,size_t duration,size_t ordinal,size_t *count,size_t *start,size_t *end) {
    *count=*start=*end=0;const char *error=sources_interrupted(cfg);if(error)return error;
    if(!duration||duration>14400000)return "invalid_range";
    const size_t window=30000,total=(duration+window-1)/window;
    if(ordinal>=total)return "invalid_request";
    *count=total;*start=ordinal*window;*end=duration-*start<window?duration:*start+window;
    return NULL;
}
