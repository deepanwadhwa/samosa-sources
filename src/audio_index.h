#ifndef SOURCES_AUDIO_INDEX_H
#define SOURCES_AUDIO_INDEX_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
int sources_audio_index(const SourcesConfig *,const Json *,sqlite3 *,int,FILE *);
/* Internal discovery/reindex admission. Caller holds the index lease and an
 * active transaction; the queue row commits/rolls back with its observation. */
const char *sources_audio_index_admit(const SourcesConfig *,sqlite3 *,const char *,size_t,char [65],char [65]);
/* Read a linked job using the same snapshot, recipe and saved-window validation
 * as public audio_index_get. Caller supplies its expected source identity. */
const char *sources_audio_index_report(const SourcesConfig *,sqlite3 *,const char *,const char *,FILE *,int *);
const char *sources_audio_index_read_retained(const SourcesConfig *,sqlite3 *,const char *,const char *,size_t,FILE *);
const char *sources_audio_index_scan_retained(const SourcesConfig *,sqlite3 *,const char *,const char *,const char *(*)(void *,const char *,size_t,const char *),void *,size_t *);
#endif
