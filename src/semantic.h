#ifndef SOURCES_SEMANTIC_H
#define SOURCES_SEMANTIC_H
#include "sources.h"
#include "json.h"
/* Build the exact bounded public-SDK query from a host-owned profile and a
 * request vector. No embedding engine or alternate ranking lives in Sources. */
const char *sources_semantic_vector_canonical(const Json *,int,char **);
const char *sources_semantic_arguments(const char *,const Json *,int,char **);
const char *sources_semantic_arguments_validate(const char *);
const char *sources_chutni_semantic_validate(const char *,size_t,const char *,const char *);
#endif
