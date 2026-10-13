#ifndef SAMOSA_SOURCES_CHUTNI_H
#define SAMOSA_SOURCES_CHUTNI_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
const char *sources_chutni_worker(const SourcesConfig *,const void *,size_t,FILE *);
const char *sources_chutni_validate(const char *,size_t,const char *);
int source_chutni_dispatch(const SourcesConfig *,const char *,size_t,sqlite3 *,FILE *);
const char *sources_chutni_publish_worker(const SourcesConfig *,const void *,size_t,FILE *);
const char *sources_chutni_publish_validate(const char *,const char *,size_t);
const char *sources_chutni_read_worker(const SourcesConfig *,const void *,size_t,FILE *);
const char *sources_chutni_read_validate(const char *,const char *,size_t);
const char *sources_chutni_embedding_worker(const SourcesConfig *,const void *,size_t,FILE *);
const char *sources_chutni_embedding_validate(const char *,const char *,size_t);
int source_index_publish(const SourcesConfig *,const char *,size_t,sqlite3 *,FILE *);
int source_visual_publish_capture(const SourcesConfig *,const char *,size_t,sqlite3 *,FILE *);
int source_visual_publish(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
int source_transcript_publish(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
/* Sources-owned queue capture; owner snapshot and complete capture are validated
 * by the same writer used by the public cached-window operation. */
int source_transcript_publish_capture(const SourcesConfig *,const char *,size_t,sqlite3 *,FILE *);
int source_summary_publish_capture(const SourcesConfig *,const char *,size_t,sqlite3 *,FILE *);
int source_summary_publish(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
#endif
