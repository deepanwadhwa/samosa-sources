#ifndef SAMOSA_LOCATOR_H
#define SAMOSA_LOCATOR_H
#include "sources.h"
#include <sqlite3.h>
int source_locator_dispatch(const SourcesConfig *,const char *,size_t,sqlite3 *,int,FILE *);
/* Private bounded background observation; no client operation receipt. */
const char *source_locator_observe_index(const SourcesConfig *,sqlite3 *,int,const char *,const char *,const char *);
/* Publishes owner/scope-bound committed history; never implies a new live read. */
const char *source_locator_freshness(const SourcesConfig *,sqlite3 *,const char *,const char *,const char *,FILE *);
const char *source_locator_schema(sqlite3 *);
const char *source_watch_admit(const SourcesConfig *,sqlite3 *,int,const char *,int *);
const char *source_watch_health(const SourcesConfig *,sqlite3 *,const char *,const char *);
const char *source_locator_index_target(const SourcesConfig *,sqlite3 *,const char *,const char *,sqlite3_int64,char [65],char [65]);
#endif
