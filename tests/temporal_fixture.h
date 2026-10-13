/* Independent public protocol fixture; no model quality claim. */
typedef struct {
    const SourcesInferenceConfig *provider;
    const char *manifest, *mode;
    size_t calls;
} TemporalFixture;
static const char *temporal_transport(void *context,
                                      const SourcesInferenceControl *control,
                                      const char *input, size_t bytes,
                                      char **result, size_t *length) {
    TemporalFixture *fixture = context;
    fixture->calls++;
    CHECK(control->deadline_monotonic > 0);
    Json j;
    CHECK(json_parse(&j, input, bytes));
    size_t start = 0, end = 0, budget = 0;
    CHECK(json_uint(&j, json_get(&j, 0, "start_us"), &start) &&
          json_uint(&j, json_get(&j, 0, "end_us"), &end));
    CHECK(json_uint(&j, json_get(&j, 0, "timeout_ms"), &budget) &&
          budget == 120000);
    char *op = json_string(&j, json_get(&j, 0, "op"));
    CHECK(op && !strcmp(op, "analyze_video_frames"));
    free(op);
    int frames = json_get(&j, 0, "frames");
    CHECK(frames >= 0 && j.tokens[frames].count >= 1 &&
          j.tokens[frames].count <= 16);
    char *canonical = NULL, *prepared = NULL, *frame_reply = NULL;
    size_t cn = 0, pn = 0, fn = 0;
    FILE *hash = open_memstream(&canonical, &cn),
         *prep = open_memstream(&prepared, &pn),
         *reply = open_memstream(&frame_reply, &fn);
    CHECK(hash && prep && reply);
    fprintf(hash,
            "{\"recipe\":\"molmo2-temporal-rgba8-v1\",\"start_us\":%zu,\"end_"
            "us\":%zu,\"frames\":[",
            start, end);
    fputc('[', prep);
    fputc('[', reply);
    size_t count = 0;
    for (int t = frames + 1; t < j.tokens[frames].next;
         t = j.tokens[t].next, count++) {
        size_t timestamp = 0, w = 0, h = 0;
        CHECK(json_uint(&j, json_get(&j, t, "timestamp_us"), &timestamp));
        CHECK(json_uint(&j, json_get(&j, t, "width"), &w) &&
              json_uint(&j, json_get(&j, t, "height"), &h));
        char *data = json_string(&j, json_get(&j, t, "rgba_base64"));
        CHECK(data);
        size_t n = strlen(data), used = 0;
        unsigned char *rgba = malloc(n / 4 * 3);
        CHECK(rgba && n % 4 == 0);
        for (size_t k = 0; k < n; k += 4) {
            unsigned a = (unsigned)digit(data[k]),
                     b = (unsigned)digit(data[k + 1]),
                     c = data[k + 2] == '=' ? 0u : (unsigned)digit(data[k + 2]),
                     d = data[k + 3] == '=' ? 0u : (unsigned)digit(data[k + 3]);
            rgba[used++] = (unsigned char)((a << 2) | (b >> 4));
            if (data[k + 2] != '=')
                rgba[used++] = (unsigned char)((b << 4) | (c >> 2));
            if (data[k + 3] != '=')
                rgba[used++] = (unsigned char)((c << 6) | d);
        }
        CHECK(used == w * h * 4);
        free(data);
        char pixels[65], ppm_sha[65];
        hash_bytes(rgba, used, pixels);
        char *ppm = NULL;
        size_t ppm_bytes = 0;
        FILE *image = open_memstream(&ppm, &ppm_bytes);
        CHECK(image);
        fprintf(image, "P6\n%zu %zu\n255\n", w, h);
        for (size_t k = 0; k < w * h; k++)
            for (size_t c = 0; c < 3; c++)
                fputc((int)(((unsigned)rgba[k * 4 + c] * rgba[k * 4 + 3] +
                             255u * (255u - rgba[k * 4 + 3]) + 127u) /
                            255u),
                      image);
        CHECK(!fclose(image));
        hash_bytes(ppm, ppm_bytes, ppm_sha);
        free(ppm);
        free(rgba);
        if (count) {
            fputc(',', hash);
            fputc(',', prep);
            fputc(',', reply);
        }
        fprintf(hash,
                "{\"timestamp_us\":%zu,\"width\":%zu,\"height\":%zu,\"rgba_"
                "sha256\":\"%s\"}",
                timestamp, w, h, pixels);
        fprintf(prep, "{\"timestamp_us\":%zu,\"ppm_sha256\":\"%s\"}", timestamp,
                ppm_sha);
        fprintf(
            reply,
            "{\"timestamp_us\":%zu,\"width\":%zu,\"height\":%zu,\"rgba_"
            "sha256\":\"%s\",\"prepared_sha256\":\"%s\"}",
            timestamp +
                (fixture->mode && !strcmp(fixture->mode, "timestamp") ? 1 : 0),
            w, h, pixels, ppm_sha);
    }
    fputs("]}", hash);
    fputc(']', prep);
    fputc(']', reply);
    CHECK(!fclose(hash) && !fclose(prep) && !fclose(reply));
    char input_sha[65], prepared_sha[65];
    hash_bytes(canonical, cn, input_sha);
    hash_bytes(prepared, pn, prepared_sha);
    free(canonical);
    free(prepared);
    FILE *out = open_memstream(result, length);
    CHECK(out);
    fprintf(out,
            "{\"contract_version\":1,\"provider_id\":\"%s\",\"kind\":"
            "\"derived\",\"artifact_type\":\"video_observation\",\"input_"
            "sha256\":\"%s\",\"text\":\"These sampled frames show a changing "
            "scene.\",\"range\":{\"start_us\":%zu,\"end_us\":%zu,\"coordinate_"
            "space\":\"input_video_us\"},\"frames\":%s,\"usage\":{\"input_"
            "tokens\":500,\"output_tokens\":9},\"coverage\":{\"status\":"
            "\"partial\",\"reason\":\"sampled_video_analysis_uncertain\"},"
            "\"provenance\":{\"engine\":\"molmo2\",\"engine_sha256\":\"%s\","
            "\"model_sha256\":\"%s\",\"manifest_sha256\":\"%s\",\"prepared_"
            "frames_sha256\":\"%s\",\"alpha_policy\":\"rgba8_composited_on_"
            "white\",\"timestamp_policy\":\"molmo2_one_decimal_seconds\","
            "\"coordinate_space\":\"input_video_us\"}}",
            fixture->provider->provider_id, input_sha, start, end, frame_reply,
            fixture->provider->engine_sha256, fixture->provider->model_sha256,
            fixture->manifest, prepared_sha);
    free(frame_reply);
    CHECK(!fclose(out));
    return NULL;
}
