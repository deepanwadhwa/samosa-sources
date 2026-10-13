#ifndef SAMOSA_SOURCES_OCR_H
#define SAMOSA_SOURCES_OCR_H
#include "sources.h"
typedef struct SourcesOcrConfig {
    const char *tesseract_library,*leptonica_library,*traineddata_file,*language;
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    /* Request-local observation of the real engine monitor; absent in releases. */
    void (*test_event)(void *,const char *,SourcesConfig *);
    void *test_context;
#endif
} SourcesOcrConfig;
typedef struct {
    char *text;size_t text_bytes;int confidence;
    unsigned width,height;char fingerprint[65];
} SourcesOcrResult;
const char *sources_ocr_fingerprint(const SourcesConfig *,char out[65]);
const char *sources_ocr_extract(const SourcesConfig *,const void *,size_t,SourcesOcrResult *);
/* Owned PDF renderer only: encoded PGM generated from a bounded decoded raster.
 * Original registered images still use the 64MiB sources_ocr_extract limit. */
const char *sources_ocr_extract_rendered(const SourcesConfig *,const void *,size_t,SourcesOcrResult *);
void sources_ocr_free(SourcesOcrResult *);
#endif
