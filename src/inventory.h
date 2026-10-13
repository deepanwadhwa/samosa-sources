#ifndef SAMOSA_INVENTORY_H
#define SAMOSA_INVENTORY_H
#include "sources.h"
#include <sys/stat.h>
void inventory_file_identity(const struct stat *,char [65]);
const char *inventory_scope_identity(const SourcesConfig *,const char *,char [65]);
int inventory_run(const SourcesConfig *,const char *,FILE *);
#endif
