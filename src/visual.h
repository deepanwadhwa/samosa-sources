#ifndef SOURCES_VISUAL_H
#define SOURCES_VISUAL_H
#include "json.h"
#include "sources.h"
#include <sqlite3.h>
typedef struct {
    char *source, *prepared, *data, *decoder, *preparation;
    size_t start, end, width, height, timestamp;
    char input[65], ppm[65], artifact[65];
} SourcesPreparedFrame;
/* Component-owned validation shared by single-frame and temporal clients. */
const char *sources_prepared_frame(const SourcesConfig *,const char *,size_t,SourcesPreparedFrame *);
void sources_prepared_frame_release(SourcesPreparedFrame *);
/* Bind an uncertain public Inference observation to validated prepared pixels,
 * original source range/timestamp and decoder/preparation provenance. */
const char *sources_visual_project(const SourcesConfig *, const char *, size_t, const char *,
                                   size_t, const char *, char **, size_t *);
const char *sources_visual_capture_validate(const SourcesConfig *, const char *, size_t);
const char *sources_visual_recipe(const SourcesConfig *,char[65]);
const char *sources_visual_window_plan(const SourcesConfig *,size_t,size_t,size_t *,size_t *,size_t *);
int sources_visual_dispatch(const SourcesConfig *, const Json *, sqlite3 *, FILE *);
int sources_visual_window(const SourcesConfig *, const Json *, sqlite3 *, FILE *);
#endif
