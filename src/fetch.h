#ifndef SAMOSA_SOURCES_FETCH_H
#define SAMOSA_SOURCES_FETCH_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
#define SOURCES_FETCH_MAX (5u * 1048576u)
#define SOURCES_SEARCH_HTTP_MAX 1048576u
#define SOURCES_HTTP_RESPONSE_MAX (((SOURCES_FETCH_MAX+2u)/3u)*4u+32768u)
#define SOURCES_HTTP_REQUEST_MAX 65536u
const char *sources_http_options_validate(const char *,const char *,const char *);
const char *sources_native_http(const SourcesConfig *,const char *,const char *,const char *,const char *,unsigned char **,size_t *);
const char *sources_native_http_limited(const SourcesConfig *,const char *,const char *,const char *,const char *,size_t,unsigned char **,size_t *);
const char *sources_fetch_authorize(const SourcesConfig *,const char *);
const char *sources_fetch_validate(const char *,const char *,size_t,unsigned char **,size_t *);
const char *sources_fetch_validate_route(const SourcesConfig *,const char *,const char *,size_t,unsigned char **,size_t *,char **,char **);
const char *sources_native_fetch_capture(const SourcesConfig *,const char *,unsigned char **,size_t *,char **,char **);
const char *sources_fetch_worker(const SourcesConfig *,const void *,size_t,FILE *);
int source_fetch_dispatch(const SourcesConfig *,const Json *,sqlite3 *,FILE *);
#endif
