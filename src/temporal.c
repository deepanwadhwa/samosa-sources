#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "temporal.h"
#include "control.h"
#include "native.h"
#include "sha256.h"
static int pin(const char *s) {
    return s && strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64;
}
static int equal(const Json *j, int object, const char *name,
                 const char *expected) {
    char *value = json_string(j, json_get(j, object, name));
    int same = value && expected && !strcmp(value, expected);
    free(value);
    return same;
}
static int number(const Json *j, int object, const char *name,
                  size_t expected) {
    size_t value = 0;
    return json_uint(j, json_get(j, object, name), &value) && value == expected;
}
static int object_count(const Json *j, int token, int count) {
    return token >= 0 && j->tokens[token].type == JT_OBJECT &&
           j->tokens[token].count == count;
}
const char *sources_temporal_response(const SourcesConfig *cfg,
                                      const SourcesTemporalInput *input,
                                      const char *body, size_t bytes) {
    const SourcesInferenceConfig *provider =
        cfg ? cfg->temporal_inference : NULL;
    if (!provider || !input || !input->count || input->count > 16)
        return "invalid_configuration";
    Json j;
    if (!body || bytes > 65536u || !json_parse(&j, body, bytes) ||
        !object_count(&j, 0, 11) || !number(&j, 0, "contract_version", 1) ||
        !equal(&j, 0, "provider_id", provider->provider_id) ||
        !equal(&j, 0, "kind", "derived") ||
        !equal(&j, 0, "artifact_type", "video_observation") ||
        !equal(&j, 0, "input_sha256", input->input_sha256))
        return "inference_response_invalid";
    int range = json_get(&j, 0, "range"), frames = json_get(&j, 0, "frames"),
        usage = json_get(&j, 0, "usage"),
        coverage = json_get(&j, 0, "coverage"),
        provenance = json_get(&j, 0, "provenance");
    char *text = json_string(&j, json_get(&j, 0, "text"));
    int valid = text && *text && strlen(text) <= 16384u;
    free(text);
    size_t in = 0, out = 0;
    valid =
        valid && object_count(&j, range, 3) &&
        number(&j, range, "start_us", input->start_us) &&
        number(&j, range, "end_us", input->end_us) &&
        equal(&j, range, "coordinate_space", "input_video_us") && frames >= 0 &&
        j.tokens[frames].type == JT_ARRAY &&
        j.tokens[frames].count == (int)input->count &&
        object_count(&j, usage, 2) &&
        json_uint(&j, json_get(&j, usage, "input_tokens"), &in) && in &&
        in <= 2048 &&
        json_uint(&j, json_get(&j, usage, "output_tokens"), &out) && out &&
        out <= 128 && object_count(&j, coverage, 2) &&
        equal(&j, coverage, "status", "partial") &&
        equal(&j, coverage, "reason", "sampled_video_analysis_uncertain") &&
        object_count(&j, provenance, 8) &&
        equal(&j, provenance, "engine", "molmo2") &&
        equal(&j, provenance, "engine_sha256", provider->engine_sha256) &&
        equal(&j, provenance, "model_sha256", provider->model_sha256) &&
        equal(&j, provenance, "prepared_frames_sha256",
              input->prepared_sha256) &&
        equal(&j, provenance, "alpha_policy", "rgba8_composited_on_white") &&
        equal(&j, provenance, "timestamp_policy",
              "molmo2_one_decimal_seconds") &&
        equal(&j, provenance, "coordinate_space", "input_video_us");
    char *manifest =
        json_string(&j, json_get(&j, provenance, "manifest_sha256"));
    if (valid && pin(manifest)) {
        char material[96], identity[65];
        int n = snprintf(material, sizeof(material), "molmo2-package-v1:%s",
                         manifest);
        hash_bytes(material, (size_t)n, identity);
        valid = !strcmp(identity, provider->model_sha256);
    } else
        valid = 0;
    free(manifest);
    size_t k = 0;
    for (int t = frames + 1; valid && t < j.tokens[frames].next;
         t = j.tokens[t].next, k++) {
        const SourcesPreparedFrame *frame = &input->frames[k];
        valid = object_count(&j, t, 5) &&
                number(&j, t, "timestamp_us", frame->timestamp) &&
                number(&j, t, "width", frame->width) &&
                number(&j, t, "height", frame->height) &&
                equal(&j, t, "rgba_sha256", frame->input) &&
                equal(&j, t, "prepared_sha256", frame->ppm);
    }
    return valid ? sources_interrupted(cfg) : "inference_response_invalid";
}
void sources_temporal_input_release(SourcesTemporalInput *input) {
    for (size_t k = 0; k < 16; k++)
        sources_prepared_frame_release(&input->frames[k]);
    memset(input, 0, sizeof(*input));
}
const char *sources_temporal_hashes(SourcesTemporalInput *input) {
    if (!input || !input->count || input->count > 16)
        return "invalid_request";
    char *canonical = NULL, *prepared = NULL;
    size_t cn = 0, pn = 0;
    FILE *hash = open_memstream(&canonical, &cn),
         *prep = open_memstream(&prepared, &pn);
    const char *error = (!hash || !prep) ? "resource_limit" : NULL;
    if (!error) {
        fprintf(hash,
                "{\"recipe\":\"molmo2-temporal-rgba8-v1\",\"start_us\":%zu,"
                "\"end_us\":%zu,\"frames\":[",
                input->start_us, input->end_us);
        fputc('[', prep);
        for (size_t k = 0; k < input->count; k++) {
            const SourcesPreparedFrame *frame = &input->frames[k];
            if (k) {
                fputc(',', hash);
                fputc(',', prep);
            }
            fprintf(hash,
                    "{\"timestamp_us\":%zu,\"width\":%zu,\"height\":%zu,\"rgba_"
                    "sha256\":\"%s\"}",
                    frame->timestamp, frame->width, frame->height,
                    frame->input);
            fprintf(prep, "{\"timestamp_us\":%zu,\"ppm_sha256\":\"%s\"}",
                    frame->timestamp, frame->ppm);
        }
        fputs("]}", hash);
        fputc(']', prep);
    }
    if (hash && fclose(hash))
        error = "resource_limit";
    if (prep && fclose(prep))
        error = "resource_limit";
    if (!error) {
        hash_bytes(canonical, cn, input->input_sha256);
        hash_bytes(prepared, pn, input->prepared_sha256);
    }
    free(canonical);
    free(prepared);
    return error;
}
const char *sources_temporal_input(
    const SourcesConfig *cfg, const char *const *bodies, const size_t *lengths,
    size_t count, size_t start_ms, size_t end_ms, const char *prompt,
    SourcesTemporalInput *input, char **request, size_t *bytes) {
    memset(input, 0, sizeof(*input));
    *request = NULL;
    *bytes = 0;
    const SourcesInferenceConfig *provider =
        cfg ? cfg->temporal_inference : NULL;
    if (!provider || !provider->provider_id || !*provider->provider_id ||
        strlen(provider->provider_id) > 128 || !pin(provider->engine_sha256) ||
        !pin(provider->model_sha256))
        return "inference_unconfigured";
    for (const unsigned char *p = (const unsigned char *)provider->provider_id;
         *p; p++)
        if (*p > 127 || (!isalnum(*p) && !strchr("-_:.", *p)))
            return "invalid_configuration";
    if (!bodies || !lengths || !count || count > 16 || !prompt || !*prompt ||
        strlen(prompt) > 4096 ||
        !utf8_valid((const unsigned char *)prompt, strlen(prompt)))
        return "invalid_request";
    if (start_ms >= end_ms || end_ms > 14400000u)
        return "invalid_range";
    input->start_us = start_ms * 1000;
    input->end_us = end_ms * 1000;
    const char *error = sources_interrupted(cfg);
    for (size_t k = 0; !error && k < count; k++) {
        SourcesPreparedFrame *frame = &input->frames[k];
        if (!bodies[k])
            error = "invalid_request";
        else
            error = sources_prepared_frame(cfg, bodies[k], lengths[k], frame);
        if (!error &&
            (frame->start < start_ms || frame->end > end_ms ||
             frame->timestamp < input->start_us ||
             frame->timestamp >= input->end_us ||
             (k && (strcmp(frame->source, input->frames[0].source) ||
                    frame->timestamp <= input->frames[k - 1].timestamp))))
            error = "invalid_range";
        if (!error)
            input->count++;
    }
    FILE *out = NULL;
    if (!error) {
        out = open_memstream(request, bytes);
        if (!out)
            error = "resource_limit";
    }
    if (!error) {
        fprintf(out, "{\"contract_version\":1,\"op\":\"analyze_video_frames\","
                     "\"provider_id\":");
        json_quote(out, provider->provider_id, strlen(provider->provider_id));
        fprintf(out,
                ",\"start_us\":%zu,\"end_us\":%zu,\"prompt\":", input->start_us,
                input->end_us);
        json_quote(out, prompt, strlen(prompt));
        fputs(",\"max_output_tokens\":128,\"timeout_ms\":120000,\"frames\":[",
              out);
        for (size_t k = 0; k < input->count; k++) {
            SourcesPreparedFrame *frame = &input->frames[k];
            if (k)
                fputc(',', out);
            fprintf(out,
                    "{\"timestamp_us\":%zu,\"width\":%zu,\"height\":%zu,\"rgba_"
                    "base64\":",
                    frame->timestamp, frame->width, frame->height);
            json_quote(out, frame->data, strlen(frame->data));
            fputc('}', out);
        }
        fputs("]}", out);
    }
    if (out && fclose(out))
        error = "resource_limit";
    if (!error && *bytes > 25165824u)
        error = "resource_limit";
    if (!error)
        error = sources_temporal_hashes(input);
    if (!error)
        error = sources_interrupted(cfg);
    if (error) {
        free(*request);
        *request = NULL;
        *bytes = 0;
        sources_temporal_input_release(input);
    }
    return error;
}
static char *prepared_body(const char *source, const char *locator,
                           const char *decoder, size_t bytes, size_t start,
                           size_t end, int last, size_t *length) {
    Json j;
    if (!json_parse(&j, decoder, bytes))
        return NULL;
    char *data = json_string(&j, json_get(&j, 0, "data")),
         *fingerprint = json_string(&j, json_get(&j, 0, "fingerprint"));
    const char *cut = strstr(decoder, ",\"encoding\":");
    if (!data || !pin(fingerprint) || !cut) {
        free(data);
        free(fingerprint);
        return NULL;
    }
    const char *recipe =
        last ? "visual-media-last-frame-v1" : "visual-media-frame-v1";
    char material[256], artifact[65];
    int n = snprintf(material, sizeof(material), "%s:%s:%s:%zu:%zu", recipe,
                     fingerprint, source, start, end);
    hash_bytes(material, (size_t)n, artifact);
    char *body = NULL;
    FILE *out = open_memstream(&body, length);
    if (out) {
        fprintf(out,
                "{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%"
                "s\",\"artifact_id\":\"%s\",\"kind\":\"derived\",\"artifact_"
                "type\":\"prepared_visual\",\"media_type\":\"application/"
                "x-samosa-rgba\",\"encoding\":\"base64\",\"data\":",
                source, source, artifact);
        json_quote(out, data, strlen(data));
        fputs(",\"decoder\":", out);
        fwrite(decoder, 1, (size_t)(cut - decoder), out);
        fputc('}', out);
        fprintf(out,
                ",\"range\":{\"start_ms\":%zu,\"end_ms\":%zu,\"coordinate_"
                "space\":\"source_time_ms\"},\"coverage\":{\"status\":"
                "\"partial\",\"reason\":\"media_preparation_only\"},"
                "\"provenance\":{\"reader\":\"%s\",\"extractor_fingerprint\":"
                "\"%s\",\"derivation\":\"%s\",\"locator\":",
                start, end, recipe, fingerprint,
                last ? "native_decode_scale_rgba_last"
                     : "native_decode_scale_rgba");
        json_quote(out, locator, strlen(locator));
        fputs("}}", out);
        if (fclose(out)) {
            free(body);
            body = NULL;
        }
    }
    free(data);
    free(fingerprint);
    return body;
}
const char *sources_temporal_prepare(const SourcesConfig *cfg, sqlite3 *db,
                                     const char *source, size_t start,
                                     size_t end, const char *prompt,
                                     SourcesTemporalInput *input,
                                     char **request, size_t *bytes) {
    memset(input, 0, sizeof(*input));
    *request = NULL;
    *bytes = 0;
    if (!cfg || !db || !pin(source))
        return "invalid_request";
    if (start >= end || end > 14400000u)
        return "invalid_range";
    const char *error = sources_interrupted(cfg);
    sqlite3_stmt *statement = NULL;
    unsigned char *snapshot = NULL;
    size_t length = 0;
    char *locator = NULL;
    if (!error &&
        sqlite3_prepare_v2(
            db,
            "SELECT b.bytes,c.locator FROM snapshots b JOIN source_claims c ON "
            "c.id=b.id WHERE c.id=? AND c.owner=? AND "
            "c.media='application/x-samosa-encoded-media' LIMIT 1",
            -1, &statement, NULL) != SQLITE_OK)
        error = "store_failed";
    if (!error) {
        sqlite3_bind_text(statement, 1, source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, cfg->owner ? cfg->owner : "local-owner",
                          -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(statement);
        if (rc == SQLITE_DONE)
            error = "unknown_source";
        else if (rc != SQLITE_ROW)
            error = "store_failed";
        else {
            int n = sqlite3_column_bytes(statement, 0);
            const void *blob = sqlite3_column_blob(statement, 0);
            const char *path = (const char *)sqlite3_column_text(statement, 1);
            if (n <= 0 || (size_t)n > SOURCES_INPUT_MAX || !blob || !path ||
                !*path || strlen(path) > 4095)
                error = "corrupt_snapshot";
            else {
                length = (size_t)n;
                snapshot = malloc(length);
                locator = strdup(path);
                if (!snapshot || !locator)
                    error = "resource_limit";
                else {
                    memcpy(snapshot, blob, length);
                    char actual[65];
                    hash_bytes(snapshot, length, actual);
                    if (strcmp(actual, source))
                        error = "corrupt_snapshot";
                }
            }
        }
    }
    sqlite3_finalize(statement);
    char fingerprint[65];
    if (!error)
        error = sources_native_media_fingerprint(cfg, fingerprint);
    char *probe=NULL;size_t probe_bytes=0;
    if(!error)error=sources_native_video_probe(cfg,snapshot,length,fingerprint,&probe,&probe_bytes);
    if(!error){Json info;size_t duration_ms=0;
        if(!json_parse(&info,probe,probe_bytes)||!json_uint(&info,json_get(&info,0,"duration_ms"),&duration_ms))error="native_helper_protocol";
        else if(end>duration_ms)error="invalid_range";
    }
    free(probe);
    size_t duration = end - start, count = duration / 500 + 1;
    if (count < 2)
        count = 2;
    if (count > 16)
        count = 16;
    char *bodies[16] = {0};
    size_t lengths[16] = {0}, used = 0, last_timestamp = 0;
    for (size_t k = 0; !error && k < count; k++) {
        int last = k + 1 == count;
        size_t a = last ? (duration > 30000 ? end - 30000 : start)
                        : start + duration * k / (count - 1);
        size_t b = last ? end : (end - a > 30000 ? a + 30000 : end);
        char *decoded = NULL;
        size_t decoded_bytes = 0;
        error = last ? sources_native_media_last_frame(cfg, snapshot, length, a,
                                                       b, fingerprint, &decoded,
                                                       &decoded_bytes)
                     : sources_native_media_frame(cfg, snapshot, length, a, b,
                                                  fingerprint, &decoded,
                                                  &decoded_bytes);
        if (!error) {
            bodies[used] =
                prepared_body(source, locator, decoded, decoded_bytes, a, b,
                              last, &lengths[used]);
            if (!bodies[used])
                error = "resource_limit";
        }
        SourcesPreparedFrame frame = {0};
        if (!error)
            error = sources_prepared_frame(cfg, bodies[used], lengths[used],
                                           &frame);
        if (!error && used && frame.timestamp < last_timestamp)
            error = "media_timeline_unsupported";
        if (!error) {
            if (used && frame.timestamp == last_timestamp) {
                if (last) {
                    free(bodies[used - 1]);
                    bodies[used - 1] = bodies[used];
                    lengths[used - 1] = lengths[used];
                } else
                    free(bodies[used]);
                bodies[used] = NULL;
            } else {
                last_timestamp = frame.timestamp;
                used++;
            }
        }
        sources_prepared_frame_release(&frame);
        free(decoded);
    }
    if (!error)
        error = sources_temporal_input(cfg, (const char *const *)bodies,
                                       lengths, used, start, end, prompt, input,
                                       request, bytes);
    for (size_t k = 0; k < 16; k++)
        free(bodies[k]);
    free(snapshot);
    free(locator);
    return error;
}
