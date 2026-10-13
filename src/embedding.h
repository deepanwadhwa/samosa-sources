#ifndef SAMOSA_SOURCES_EMBEDDING_H
#define SAMOSA_SOURCES_EMBEDDING_H
#include "json.h"
#include "sources.h"
const char *sources_embedding_identity(const SourcesConfig *, char[65]);
int sources_embedding_config_parse(const char *, size_t,
                                   SourcesEmbeddingConfig *);
void sources_embedding_config_free(SourcesEmbeddingConfig *);
const char *sources_embedding_capture_arguments(const SourcesConfig *,const char *,const char *,const char *,size_t,char **);
const char *sources_embedding_generate(const SourcesConfig *, const char *,
                                       const char *, const char *, char **,
                                       size_t *);
const char *sources_embedding_search_arguments(const SourcesConfig *,
                                               const char *, char **, char **,
                                               size_t *);
#endif
