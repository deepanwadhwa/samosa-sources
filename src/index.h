#ifndef SAMOSA_SOURCE_INDEX_H
#define SAMOSA_SOURCE_INDEX_H
#include "sources.h"
#include <sqlite3.h>
/* Shared reader identities and bounded index projection; no new extractor. */
void sources_html_fingerprint(char [65]);
void sources_docx_fingerprint(char [65]);
char *source_index_literal(const char *,size_t);
/* Regenerate the same bounded PDF text/OCR projection from owned bytes in the
 * publication worker. Result and error strings belong to the caller. */
char *source_index_pdf_regenerate(const SourcesConfig *,const void *,size_t,const char *,size_t,size_t,const char *,char **);
int source_index_dispatch(const SourcesConfig *,const char *,size_t,sqlite3 *,int,FILE *);
/* Shared one-at-a-time indexing admission lease; caller closes the returned fd. */
const char *source_index_lease(int,int *);
/* Called within the locator observation transaction. */
const char *source_index_observed(sqlite3 *,const char *,const char *,const char *,sqlite3_int64,const char *,const char *);
#endif
