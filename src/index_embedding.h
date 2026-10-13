#ifndef SAMOSA_INDEX_EMBEDDING_H
#define SAMOSA_INDEX_EMBEDDING_H
#include "embedding.h"
#include <sqlite3.h>
int sources_index_embed(const SourcesConfig *,const Json *,sqlite3 *,int,FILE *);
#endif
