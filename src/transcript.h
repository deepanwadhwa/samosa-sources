#ifndef SOURCES_TRANSCRIPT_H
#define SOURCES_TRANSCRIPT_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
/* Public Inference result validator and original-timeline projection. */
const char *sources_transcript_project(const SourcesConfig *,const char *,size_t,
    const char *,const char *,const char *,size_t,size_t,char **,size_t *);
int sources_transcript_dispatch(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
/* One queue window using bounded working buffers, without persistent PCM/capture
 * insertion. Existing qualified cached captures may still be reused. */
int sources_transcript_window(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
const char *sources_transcript_recipe(const SourcesConfig *,char [65]);
/* Validate an immutable captured transcript without rerunning its model. */
const char *sources_transcript_capture_validate(const SourcesConfig *,const char *,size_t);
#endif
