#ifndef SAMOSA_WATCH_H
#define SAMOSA_WATCH_H
#include "sources.h"
int sources_watch_run(const SourcesConfig *,const char *,size_t,FILE *);
const char *sources_watch_monitor(int,const char *);
/* Component-private health recording; public clients cannot set counters. */
const char *sources_watch_record(const SourcesConfig *,const char *,const char *);
const char *sources_watch_lock(int,const char *,int *);
const char *sources_watch_admit(const SourcesConfig *,const char *,int *,int *);
const char *sources_watch_create_lease(int,const char *,int *);
int sources_watch_unlink(int,const char *,int);
int sources_queue_run(const SourcesConfig *,size_t,FILE *);
const char *sources_queue_admit(const SourcesConfig *,int *,int *);
const char *sources_queue_create_lease(int,int *);
#endif
