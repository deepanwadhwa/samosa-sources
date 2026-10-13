#ifndef SAMOSA_SOURCES_SUMMARY_H
#define SAMOSA_SOURCES_SUMMARY_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
int sources_summary_dispatch(const SourcesConfig *, const Json *, sqlite3 *, FILE *);
int sources_summary_opening_dispatch(const SourcesConfig *, const Json *, sqlite3 *, FILE *);
const char *sources_summary_recipe(const SourcesConfig *, char[65]);
const char *sources_summary_opening_recipe(const SourcesConfig *, char[65]);
const char *sources_summary_capture_validate(const SourcesConfig *,const char *,size_t);
int sources_summary_opening_retained(const SourcesConfig *,sqlite3 *,const char *,size_t,FILE *);
#endif
