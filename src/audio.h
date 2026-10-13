#ifndef SOURCES_AUDIO_H
#define SOURCES_AUDIO_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
typedef struct {
    const unsigned char *pcm;
    size_t pcm_bytes, frames;
} SourcesWave;
const char *sources_wave_inspect(const SourcesConfig *, const void *, size_t,
                                 SourcesWave *);
const char *sources_wave_window(const SourcesConfig *, const void *, size_t,
                                size_t, size_t, unsigned char **, size_t *);
int sources_media_prepare(const SourcesConfig *, const Json *, sqlite3 *, FILE *);
int sources_visual_inspect(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
int sources_audio_inspect(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
/* Queue working buffer: validates/reuses existing entries, never adds PCM to
 * the persistent cache. The owning caller retains its result until consumed. */
int sources_media_prepare_transient(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
/* Monolith overlap/ceil-stride accounting adapted to30s public windows and a
 * one-second overlap; every final partial window is retained. */
const char *sources_audio_window_plan(const SourcesConfig *,size_t,size_t,size_t *,size_t *,size_t *);
#endif
