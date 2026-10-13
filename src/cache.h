#ifndef SOURCES_CACHE_H
#define SOURCES_CACHE_H
#include "sources.h"
#include <sqlite3.h>
/* Shared admission budget for literal evidence and prepared derived artifacts. */
const char *sources_cache_capacity(sqlite3 *,const SourcesConfig *,size_t);
/* Reuse an owned retained literal capture without reopening its origin or
 * loading an extractor. Verifies snapshot, cache digest and selector identity. */
const char *sources_literal_capture(sqlite3 *,const SourcesConfig *,const char *,const char *,char **,size_t *);
const char *sources_literal_retained_validate(sqlite3 *,const SourcesConfig *,const char *,size_t);
#endif
