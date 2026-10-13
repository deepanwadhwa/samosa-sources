#include "media_decode.h"
#include "media_video.h"
#include "control.h"
#include <stdint.h>
const char *sources_media_probe_response(const SourcesConfig *cfg,const void *bytes,
    size_t length,FILE *out) {
    SourcesMediaAudio audio={0};
    const char *error=sources_media_audio_probe(cfg,bytes,length,&audio);
    if(!error)fprintf(out,"{\"fingerprint\":\"%s\",\"stream\":%u,\"source_rate\":%u,"
        "\"source_channels\":%u,\"origin_us\":%lld,\"frames\":%zu,\"duration_ms\":%zu,"
        "\"sample_rate\":16000,\"duration_policy\":\"decoded_samples_floor_ms\"}\n",
        audio.fingerprint,audio.stream,audio.source_rate,audio.source_channels,
        audio.origin_us,audio.frames,audio.frames/16);
    sources_media_audio_free(&audio);return error?error:sources_interrupted(cfg);
}
static const char *payload(const SourcesConfig *cfg,FILE *out,const unsigned char *bytes,size_t length) {
    const char *error=NULL;
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < length; i += 3) {
        if (i % 12288 == 0 && (error = sources_interrupted(cfg))) break;
        uint32_t value = (uint32_t)bytes[i] << 16;
        if (i + 1 < length) value |= (uint32_t)bytes[i + 1] << 8;
        if (i + 2 < length) value |= bytes[i + 2];
        fputc(alphabet[(value >> 18) & 63], out);
        fputc(alphabet[(value >> 12) & 63], out);
        fputc(i + 1 < length ? alphabet[(value >> 6) & 63] : '=', out);
        fputc(i + 2 < length ? alphabet[value & 63] : '=', out);
    }
    return error;
}
static const char *frame_response(const SourcesConfig *cfg,const void *bytes,
    size_t length,size_t start,size_t end,FILE *out,int last) {
    SourcesMediaFrame frame={0};
    const char *error=last?sources_media_video_last_frame(cfg,bytes,length,start,end,&frame):
        sources_media_video_frame(cfg,bytes,length,start,end,&frame);
    if(error)return error;
    fprintf(out,"{\"fingerprint\":\"%s\",\"stream\":%u,\"width\":%u,\"height\":%u,"
        "\"source_width\":%u,\"source_height\":%u,\"timestamp_us\":%lld,\"origin_us\":%lld,"
        "\"colorspace\":%d,\"color_range\":%d,\"start_ms\":%zu,\"end_ms\":%zu,"
        "\"rgba_bytes\":%zu,\"orientation\":%u,\"source_transfer\":%u,\"source_primaries\":%u,"
        "\"output_transfer\":%u,\"output_primaries\":%u,\"tone_mapped\":%u,"
        "\"pixel_format\":\"rgba8\",\"encoding\":\"base64\",\"data\":\"",
        frame.fingerprint,frame.stream,frame.width,frame.height,frame.source_width,
        frame.source_height,frame.timestamp_us,frame.origin_us,frame.colorspace,
        frame.color_range,start,end,frame.bytes,frame.orientation,frame.source_transfer,
        frame.source_primaries,frame.output_transfer,frame.output_primaries,frame.tone_mapped);
    error=payload(cfg,out,frame.rgba,frame.bytes);
    if(!error)fputs("\"}\n",out);
    sources_media_frame_free(&frame);
    return error?error:sources_interrupted(cfg);
}
const char *sources_media_frame_response(const SourcesConfig *cfg,const void *bytes,
    size_t length,size_t start,size_t end,FILE *out) {
    return frame_response(cfg,bytes,length,start,end,out,0);
}
const char *sources_media_last_frame_response(const SourcesConfig *cfg,const void *bytes,
    size_t length,size_t start,size_t end,FILE *out) {
    return frame_response(cfg,bytes,length,start,end,out,1);
}
const char *sources_media_audio_response(const SourcesConfig *cfg,
    const void *bytes, size_t length, size_t start, size_t end, FILE *out) {
    SourcesMediaAudio audio = {0};
    const char *error = sources_media_audio_decode(cfg, bytes, length, start, end, &audio);
    if (error) return error;
    fprintf(out, "{\"fingerprint\":\"%s\",\"stream\":%u,\"source_rate\":%u,"
                 "\"source_channels\":%u,\"origin_us\":%lld,\"start_ms\":%zu,"
                 "\"end_ms\":%zu,\"wave_bytes\":%zu,\"encoding\":\"base64\",\"data\":\"",
            audio.fingerprint, audio.stream, audio.source_rate,
            audio.source_channels, audio.origin_us, start, end, audio.wave_bytes);
    error=payload(cfg,out,audio.wave,audio.wave_bytes);
    if (!error) fputs("\"}\n", out);
    sources_media_audio_free(&audio);
    if (!error) error = sources_interrupted(cfg);
    return error;
}

const char *sources_video_probe_response(const SourcesConfig *cfg,const void *bytes,
    size_t length,FILE *out) {
    SourcesMediaVideoInfo info={0};
    const char *error=sources_media_video_probe(cfg,bytes,length,&info);
    if(!error)fprintf(out,"{\"fingerprint\":\"%s\",\"stream\":%u,"
        "\"source_width\":%u,\"source_height\":%u,\"origin_us\":%lld,"
        "\"duration_ms\":%zu,\"duration_policy\":\"container_floor_ms\"}\n",
        info.fingerprint,info.stream,info.source_width,info.source_height,info.origin_us,
        info.duration_ms);
    return error?error:sources_interrupted(cfg);
}
