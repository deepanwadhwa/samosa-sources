#include "audio.h"
#include "control.h"
#include "native.h"
#include "sha256.h"
const char *sources_media_probe_validate(const SourcesConfig *cfg, const char *body, size_t n) {
    Json j;
    size_t frames, duration, rate, channels, stream, prepared_rate;
    if (!json_parse(&j, body, n) || j.tokens[0].type != JT_OBJECT || j.tokens[0].count != 9 ||
        !json_uint(&j, json_get(&j, 0, "frames"), &frames) || frames < 16 || frames > 230400000 ||
        !json_uint(&j, json_get(&j, 0, "duration_ms"), &duration) || duration != frames / 16 ||
        !json_uint(&j, json_get(&j, 0, "source_rate"), &rate) || rate < 8000 || rate > 192000 ||
        !json_uint(&j, json_get(&j, 0, "source_channels"), &channels) || !channels ||
        channels > 8 || !json_uint(&j, json_get(&j, 0, "stream"), &stream) || stream >= 8 ||
        !json_uint(&j, json_get(&j, 0, "sample_rate"), &prepared_rate) || prepared_rate != 16000)
        return "native_helper_protocol";
    int origin = json_get(&j, 0, "origin_us");
    if (origin < 0 || j.tokens[origin].type != JT_NUMBER ||
        j.tokens[origin].end - j.tokens[origin].begin > 20)
        return "native_helper_protocol";
    char number[21];
    size_t len = j.tokens[origin].end - j.tokens[origin].begin;
    memcpy(number, body + j.tokens[origin].begin, len);
    number[len] = 0;
    char *tail;
    errno = 0;
    (void)strtoll(number, &tail, 10);
    if (errno || *tail)
        return "native_helper_protocol";
    char *fp = json_string(&j, json_get(&j, 0, "fingerprint")),
         *policy = json_string(&j, json_get(&j, 0, "duration_policy"));
    int valid = fp && strlen(fp) == 64 && policy && !strcmp(policy, "decoded_samples_floor_ms");
    for (size_t i = 0; valid && i < 64; i++)
        if (!strchr("0123456789abcdef", fp[i]))
            valid = 0;
    free(fp);
    free(policy);
    return valid ? sources_interrupted(cfg) : "native_helper_protocol";
}
static int digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static const char *frame_validate(const SourcesConfig *cfg, const char *body, size_t n,
                                  size_t start, size_t end, char rgba_sha[65], char ppm_sha[65],
                                  int metadata_only) {
    if (end <= start || end - start > 30000 || end > 14400000)
        return "invalid_range";
    Json j;
    size_t a, b, w, h, sw, sh, bytes, time, stream, matrix, range, orientation, transfer, primaries,
        output_transfer, output_primaries, tonemapped;
    if (!json_parse(&j, body, n) || j.tokens[0].type != JT_OBJECT ||
        !json_uint(&j, json_get(&j, 0, "start_ms"), &a) || a != start ||
        !json_uint(&j, json_get(&j, 0, "end_ms"), &b) || b != end ||
        !json_uint(&j, json_get(&j, 0, "width"), &w) || !w || w > 512 ||
        !json_uint(&j, json_get(&j, 0, "height"), &h) || !h || h > 512 ||
        !json_uint(&j, json_get(&j, 0, "source_width"), &sw) || !sw || sw > 4096 ||
        !json_uint(&j, json_get(&j, 0, "source_height"), &sh) || !sh || sh > 4096 ||
        sw * sh > 4194304 || !json_uint(&j, json_get(&j, 0, "rgba_bytes"), &bytes) ||
        bytes != w * h * 4 || !json_uint(&j, json_get(&j, 0, "timestamp_us"), &time) ||
        time < start * 1000 || time >= end * 1000 ||
        !json_uint(&j, json_get(&j, 0, "stream"), &stream) || stream >= 8 ||
        !json_uint(&j, json_get(&j, 0, "colorspace"), &matrix) ||
        (matrix != 1 && matrix != 5 && matrix != 9) ||
        !json_uint(&j, json_get(&j, 0, "color_range"), &range) || range > 1 ||
        !json_uint(&j, json_get(&j, 0, "orientation"), &orientation) || orientation > 7 ||
        !json_uint(&j, json_get(&j, 0, "source_transfer"), &transfer) || transfer > 18 ||
        !json_uint(&j, json_get(&j, 0, "source_primaries"), &primaries) || primaries > 22 ||
        !json_uint(&j, json_get(&j, 0, "output_transfer"), &output_transfer) ||
        output_transfer > 18 ||
        !json_uint(&j, json_get(&j, 0, "output_primaries"), &output_primaries) ||
        output_primaries > 22 || !json_uint(&j, json_get(&j, 0, "tone_mapped"), &tonemapped) ||
        tonemapped > 1)
        return "native_helper_protocol";
    int hdr = transfer == 16 || transfer == 18;
    if (hdr ? (!tonemapped || primaries != 9 || matrix != 9 || output_transfer != 13 ||
               output_primaries != 1)
            : (tonemapped || transfer != output_transfer || primaries != output_primaries))
        return "native_helper_protocol";
    size_t expected_width = sw, expected_height = sh;
    if (sw > 512 || sh > 512) {
        size_t axis = sw > sh ? sw : sh;
        expected_width = sw * 512 / axis;
        expected_height = sh * 512 / axis;
        if (!expected_width)
            expected_width = 1;
        if (!expected_height)
            expected_height = 1;
    }
    if (orientation == 1 || orientation == 3 || orientation == 6 || orientation == 7) {
        size_t swap = expected_width;
        expected_width = expected_height;
        expected_height = swap;
    }
    if (w != expected_width || h != expected_height)
        return "native_helper_protocol";
    int origin = json_get(&j, 0, "origin_us");
    if (origin < 0 || j.tokens[origin].type != JT_NUMBER ||
        j.tokens[origin].end - j.tokens[origin].begin > 20)
        return "native_helper_protocol";
    char number[21];
    size_t len = j.tokens[origin].end - j.tokens[origin].begin;
    memcpy(number, body + j.tokens[origin].begin, len);
    number[len] = 0;
    char *tail;
    errno = 0;
    (void)strtoll(number, &tail, 10);
    if (errno || *tail)
        return "native_helper_protocol";
    if (metadata_only) {
        char *fingerprint = json_string(&j, json_get(&j, 0, "fingerprint")),
             *pixel_format = json_string(&j, json_get(&j, 0, "pixel_format"));
        int valid = j.tokens[0].count == 20 && fingerprint && strlen(fingerprint) == 64 &&
                    strspn(fingerprint, "0123456789abcdef") == 64 && pixel_format &&
                    !strcmp(pixel_format, "rgba8");
        free(fingerprint);
        free(pixel_format);
        return valid ? sources_interrupted(cfg) : "native_helper_protocol";
    }
    char *fp = json_string(&j, json_get(&j, 0, "fingerprint")),
         *data = json_string(&j, json_get(&j, 0, "data"));
    char *encoding = json_string(&j, json_get(&j, 0, "encoding")),
         *format = json_string(&j, json_get(&j, 0, "pixel_format"));
    const char *error = "native_helper_protocol";
    if (!fp || strlen(fp) != 64 || !data || strlen(data) != (bytes + 2) / 3 * 4 || !encoding ||
        strcmp(encoding, "base64") || !format || strcmp(format, "rgba8"))
        goto done;
    for (size_t i = 0; i < 64; i++)
        if (!strchr("0123456789abcdef", fp[i]))
            goto done;
    size_t encoded = strlen(data), padding = (3 - bytes % 3) % 3;
    for (size_t i = 0; i < encoded; i++) {
        if (i % 12288 == 0 && (error = sources_interrupted(cfg)))
            goto done;
        error = "native_helper_protocol";
        if (i >= encoded - padding) {
            if (data[i] != '=')
                goto done;
        } else if (digit(data[i]) < 0)
            goto done;
    }
    if ((padding == 2 && (digit(data[encoded - 3]) & 15)) ||
        (padding == 1 && (digit(data[encoded - 2]) & 3)))
        goto done;
    if (rgba_sha && ppm_sha) {
        Sha256 rgba, ppm;
        sha256_init(&rgba);
        sha256_init(&ppm);
        char header[64];
        int header_bytes = snprintf(header, sizeof(header), "P6\n%zu %zu\n255\n", w, h);
        sha256_update(&ppm, header, (size_t)header_bytes);
        unsigned char pixel[4];
        size_t channel = 0, seen = 0;
        for (size_t i = 0; i < encoded; i += 4) {
            if (i % 12288 == 0 && (error = sources_interrupted(cfg)))
                goto done;
            unsigned value = (unsigned)digit(data[i]) << 18 | (unsigned)digit(data[i + 1]) << 12;
            if (data[i + 2] != '=')
                value |= (unsigned)digit(data[i + 2]) << 6;
            if (data[i + 3] != '=')
                value |= (unsigned)digit(data[i + 3]);
            for (size_t c = 0; c < 3 && seen < bytes; c++, seen++) {
                pixel[channel++] = (unsigned char)(value >> (16 - c * 8));
                if (channel == 4) {
                    sha256_update(&rgba, pixel, 4);
                    unsigned char rgb[3];
                    for (size_t k = 0; k < 3; k++)
                        rgb[k] = (unsigned char)(((unsigned)pixel[k] * pixel[3] +
                                                  255u * (255u - pixel[3]) + 127u) /
                                                 255u);
                    sha256_update(&ppm, rgb, 3);
                    channel = 0;
                }
            }
        }
        unsigned char digest[32];
        sha256_final(&rgba, digest);
        sha256_hex(digest, rgba_sha);
        sha256_final(&ppm, digest);
        sha256_hex(digest, ppm_sha);
    }
    error = sources_interrupted(cfg);
done:
    free(fp);
    free(data);
    free(encoding);
    free(format);
    return error;
}
const char *sources_media_frame_validate(const SourcesConfig *cfg, const char *body, size_t n,
                                         size_t start, size_t end) {
    return frame_validate(cfg, body, n, start, end, NULL, NULL, 0);
}
const char *sources_media_frame_hashes(const SourcesConfig *cfg, const char *body, size_t n,
                                       size_t start, size_t end, char rgba_sha[65],
                                       char ppm_sha[65]) {
    rgba_sha[0] = ppm_sha[0] = 0;
    const char *error = frame_validate(cfg, body, n, start, end, rgba_sha, ppm_sha, 0);
    if (error)
        rgba_sha[0] = ppm_sha[0] = 0;
    return error;
}
const char *sources_media_frame_metadata_validate(const SourcesConfig *cfg, const char *body,
                                                  size_t n, size_t start, size_t end) {
    return frame_validate(cfg, body, n, start, end, NULL, NULL, 1);
}
const char *sources_media_response_validate(const SourcesConfig *cfg, const char *body, size_t n,
                                            size_t start, size_t end) {
    if (end <= start || end - start > 30000 || end > 14400000)
        return "invalid_range";
    Json j;
    size_t a, b, bytes, stream, rate, channels;
    if (!json_parse(&j, body, n) || j.tokens[0].type != JT_OBJECT ||
        !json_uint(&j, json_get(&j, 0, "start_ms"), &a) || a != start ||
        !json_uint(&j, json_get(&j, 0, "end_ms"), &b) || b != end ||
        !json_uint(&j, json_get(&j, 0, "wave_bytes"), &bytes) || bytes != 44 + (end - start) * 32 ||
        !json_uint(&j, json_get(&j, 0, "stream"), &stream) || stream >= 8 ||
        !json_uint(&j, json_get(&j, 0, "source_rate"), &rate) || rate < 8000 || rate > 192000 ||
        !json_uint(&j, json_get(&j, 0, "source_channels"), &channels) || !channels || channels > 8)
        return "native_helper_protocol";
    int origin = json_get(&j, 0, "origin_us");
    if (origin < 0 || j.tokens[origin].type != JT_NUMBER ||
        j.tokens[origin].end - j.tokens[origin].begin > 20)
        return "native_helper_protocol";
    char number[21];
    size_t numlen = j.tokens[origin].end - j.tokens[origin].begin;
    memcpy(number, body + j.tokens[origin].begin, numlen);
    number[numlen] = 0;
    errno = 0;
    char *tail;
    (void)strtoll(number, &tail, 10);
    if (errno || *tail)
        return "native_helper_protocol";
    char *fp = json_string(&j, json_get(&j, 0, "fingerprint"));
    char *encoding = json_string(&j, json_get(&j, 0, "encoding"));
    char *data = json_string(&j, json_get(&j, 0, "data"));
    const char *error = "native_helper_protocol";
    unsigned char *wave = NULL;
    if (!fp || strlen(fp) != 64 || !encoding || strcmp(encoding, "base64") || !data ||
        strlen(data) != ((bytes + 2) / 3) * 4)
        goto done;
    for (size_t i = 0; i < 64; i++)
        if (!strchr("0123456789abcdef", fp[i]))
            goto done;
    wave = malloc(bytes);
    if (!wave) {
        error = "resource_limit";
        goto done;
    }
    size_t written = 0, encoded = strlen(data);
    for (size_t i = 0; i < encoded; i += 4) {
        if (i % 12288 == 0 && (error = sources_interrupted(cfg)))
            goto done;
        error = "native_helper_protocol";
        int x = digit(data[i]), y = digit(data[i + 1]);
        int z = data[i + 2] == '=' ? 0 : digit(data[i + 2]);
        int w = data[i + 3] == '=' ? 0 : digit(data[i + 3]);
        size_t count = bytes - written;
        if (count > 3)
            count = 3;
        if (x < 0 || y < 0 || z < 0 || w < 0 || !count ||
            (count == 1 && (data[i + 2] != '=' || data[i + 3] != '=' || (y & 15))) ||
            (count == 2 && (data[i + 2] == '=' || data[i + 3] != '=' || (z & 3))) ||
            (count == 3 && (data[i + 2] == '=' || data[i + 3] == '=')))
            goto done;
        uint32_t value = (uint32_t)x << 18 | (uint32_t)y << 12 | (uint32_t)z << 6 | (uint32_t)w;
        wave[written++] = (unsigned char)(value >> 16);
        if (count > 1)
            wave[written++] = (unsigned char)(value >> 8);
        if (count > 2)
            wave[written++] = (unsigned char)value;
    }
    SourcesWave parsed = {0};
    error = sources_wave_inspect(cfg, wave, bytes, &parsed);
    if (error && strcmp(error, "cancelled") && strcmp(error, "deadline_exceeded"))
        error = "native_helper_protocol";
    if (!error && (parsed.frames != (end - start) * 16 || parsed.pcm != wave + 44))
        error = "native_helper_protocol";
done:
    free(wave);
    free(fp);
    free(encoding);
    free(data);
    return error;
}

const char *sources_video_probe_validate(const SourcesConfig *cfg,const char *body,size_t n) {
    Json j;size_t duration,stream,width,height;
    if(!body||n>4096||!json_parse(&j,body,n)||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count!=7||
       !json_uint(&j,json_get(&j,0,"duration_ms"),&duration)||!duration||duration>14400000||
       !json_uint(&j,json_get(&j,0,"stream"),&stream)||stream>=8||
       !json_uint(&j,json_get(&j,0,"source_width"),&width)||!width||width>4096||
       !json_uint(&j,json_get(&j,0,"source_height"),&height)||!height||height>4096||
       width*height>4194304)return "native_helper_protocol";
    int origin=json_get(&j,0,"origin_us");
    if(origin<0||j.tokens[origin].type!=JT_NUMBER||
       j.tokens[origin].end-j.tokens[origin].begin>20)return "native_helper_protocol";
    char number[21];size_t length=j.tokens[origin].end-j.tokens[origin].begin;
    memcpy(number,body+j.tokens[origin].begin,length);number[length]=0;
    char *tail;errno=0;(void)strtoll(number,&tail,10);
    if(errno||*tail)return "native_helper_protocol";
    char *fp=json_string(&j,json_get(&j,0,"fingerprint")),
         *policy=json_string(&j,json_get(&j,0,"duration_policy"));
    int valid=fp&&strlen(fp)==64&&policy&&!strcmp(policy,"container_floor_ms");
    for(size_t i=0;valid&&i<64;i++)if(!strchr("0123456789abcdef",fp[i]))valid=0;
    free(fp);free(policy);return valid?sources_interrupted(cfg):"native_helper_protocol";
}
