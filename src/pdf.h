#ifndef SAMOSA_PDF_H
#define SAMOSA_PDF_H
#include <stdio.h>
#include <stddef.h>
#include "sources.h"
/* Adapter does not own source selection/storage. Public JSON emitted only
 * after every selected page succeeds, never a half-written qualified result. */
const char *pdf_fingerprint(const char *,char [65]);
const char *pdf_ocr_fingerprint(const SourcesConfig *,char [65]);
const char *pdf_ocr_extract(const SourcesConfig *,const void *,size_t,const char *,const char *,size_t,size_t,const char *,FILE *);
const char *pdf_extract(const SourcesConfig *cfg,const char *library,const void *bytes,size_t length,
                       const char *source,const char *version,size_t start,
                       size_t end,const char *locator,FILE *out);
#endif
