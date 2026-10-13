#ifndef SAMOSA_SOURCES_OPERATIONS_H
#define SAMOSA_SOURCES_OPERATIONS_H
#include "sources.h"
#include "bound_db.h"
#include <stdio.h>
#include <sqlite3.h>
typedef struct {sqlite3 *db;SourceBoundDb *binding;int directory,lease,cancel,invalid,committed_result;char *id;const char *owner;double deadline_monotonic;
    int (*parent_cancelled)(void *);void *parent_cancellation;
} SourcesOperation;
const char *source_operation_begin(const SourcesConfig *,const char *,const char *,SourcesOperation *,char **);
const char *source_operation_begin_parent(const SourcesConfig *,const char *,const char *,const char *,SourcesOperation *,char **);
const char *source_operation_finish(SourcesOperation *,char **,int *);
void source_operation_close(SourcesOperation *);
int source_operation_cancelled(void *);
int source_operation_public(const SourcesConfig *,const char *,const char *,FILE *);
#endif
