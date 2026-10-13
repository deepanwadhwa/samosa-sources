#ifndef SAMOSA_UPLOADS_H
#define SAMOSA_UPLOADS_H
#include "sources.h"
#include "json.h"
#include <sqlite3.h>
const char *source_upload_reserved(sqlite3 *,size_t *,size_t *);
int source_upload_session(const SourcesConfig *,const Json *,const char *,sqlite3 *,FILE *);
/* Private admission joins an existing transaction; its caller must commit or
 * roll back and withhold the buffered response until commit succeeds. */
int source_snapshot_accept(const SourcesConfig *,sqlite3 *,FILE *,const unsigned char *,size_t,const char *,int);
#endif
