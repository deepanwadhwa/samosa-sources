#ifndef SAMOSA_BOUND_DB_H
#define SAMOSA_BOUND_DB_H
#include <sqlite3.h>
typedef struct SourceBoundDb SourceBoundDb;
/* Component-private descriptor-bound rollback-journal store. */
int source_bound_open(int,const char *,sqlite3 **,SourceBoundDb **);
int source_bound_close(sqlite3 *,SourceBoundDb *);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
extern void (*source_bound_test_admitted)(void);
#endif
#endif
