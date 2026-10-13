#ifndef SOURCES_MEDIA_DECODE_H
#define SOURCES_MEDIA_DECODE_H
#include "sources.h"
/* Optional native worker primitive. Never link codecs into the minimal host. */
typedef struct {
    unsigned char *wave;
    size_t wave_bytes,frames;
    unsigned stream,source_rate,source_channels;
    long long origin_us;
    char fingerprint[65];
} SourcesMediaAudio;
const char *sources_media_audio_decode(const SourcesConfig *,const void *,size_t,
                                       size_t,size_t,SourcesMediaAudio *);
void sources_media_audio_free(SourcesMediaAudio *);
/* Complete bounded decode/resample scan, retaining no waveform. frames counts
 * usable mono16k samples; whole source milliseconds are floor(frames/16). */
const char *sources_media_audio_probe(const SourcesConfig *,const void *,size_t,
                                      SourcesMediaAudio *);
/* Private worker DTO; consumers must validate before publication. */
const char *sources_media_audio_response(const SourcesConfig *,const void *,size_t,
                                         size_t,size_t,FILE *);
const char *sources_media_probe_response(const SourcesConfig *,const void *,size_t,FILE *);
#endif
