#ifndef SAMOSA_SOURCES_ENRICHMENT_H
#define SAMOSA_SOURCES_ENRICHMENT_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
/* Retained index versions supply work; reuse the index lease and publisher. */
int sources_index_enrich(const SourcesConfig *,const Json *,sqlite3 *,int,FILE *);
#endif
