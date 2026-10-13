#ifndef SAMOSA_SOURCE_CONTROL_H
#define SAMOSA_SOURCE_CONTROL_H
#include "sources.h"
const char *sources_interrupted(const SourcesConfig *);
int sources_dispatch_core(const SourcesConfig *,const char *,size_t,FILE *);
#endif
