#ifndef SAMOSA_SOURCES_SEARCH_H
#define SAMOSA_SOURCES_SEARCH_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
/* Host-owned declaration. Public requests provide only the search query. All
 * buffers stay stable until return. Values are treated as private credentials. */
typedef struct SourcesSearchConfig {
    const char *provider,*origin,*url_template,*headers,*body,*values;
    const char *results_path,*title_path,*url_path,*description_path;
} SourcesSearchConfig;
/* Returns0 for a known preset; fills a host declaration, never grants access. */
int sources_search_preset(const char *,SourcesSearchConfig *);
int source_search_dispatch(const SourcesConfig *,const char *,size_t,sqlite3 *,FILE *);
#endif
