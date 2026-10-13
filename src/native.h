#ifndef SAMOSA_SOURCES_NATIVE_H
#define SAMOSA_SOURCES_NATIVE_H
#include "sources.h"
#include "ocr.h"
/* Stable protocol error identity; unknown codes remain protocol failures. */
const char *sources_native_error_code(const char *);
enum { SOURCES_NATIVE_IMAGE=1,SOURCES_NATIVE_PDF_OCR=2,
       SOURCES_NATIVE_PDF_TEXT=3,SOURCES_NATIVE_RENDERED=4,
       SOURCES_NATIVE_MEDIA_AUDIO=5,SOURCES_NATIVE_MEDIA_FINGERPRINT=6,
       SOURCES_NATIVE_MEDIA_FRAME=7,SOURCES_NATIVE_FETCH=8,
       SOURCES_NATIVE_CHUTNI=9,SOURCES_NATIVE_CHUTNI_PUBLISH=10,SOURCES_NATIVE_CHUTNI_READ=11,
       SOURCES_NATIVE_INFERENCE=12,SOURCES_NATIVE_MEDIA_PROBE=13,SOURCES_NATIVE_VIDEO_PROBE=14,
       SOURCES_NATIVE_CHUTNI_EMBEDDING=15,SOURCES_NATIVE_MEDIA_LAST_FRAME=16 };
const char *sources_native_media_last_frame(const SourcesConfig *,const void *,size_t,size_t,size_t,const char *,char **,size_t *);
const char *sources_inference_process_worker(const SourcesConfig *,const void *,size_t,FILE *);
const char *sources_native_ocr(const SourcesConfig *,const void *,size_t,int,SourcesOcrResult *);
const char *sources_native_pdf(const SourcesConfig *,const void *,size_t,const char *,const char *,size_t,size_t,const char *,FILE *,int);
/* Private exec entry: fd3 is a dedicated bounded duplex protocol, no database. */
int sources_native_worker_main(int,char **);
const char *sources_native_self(char *,size_t,const char *);
/* Private media DTO, validated before returning. Host selects native_helper. */
const char *sources_native_media_audio(const SourcesConfig *,const void *,size_t,
                                       size_t,size_t,const char *,char **,size_t *);
const char *sources_native_media_fingerprint(const SourcesConfig *,char[65]);
const char *sources_native_media_probe(const SourcesConfig *,const void *,size_t,
                                       const char *,char **,size_t *);
const char *sources_native_video_probe(const SourcesConfig *,const void *,size_t,
                                       const char *,char **,size_t *);
const char *sources_video_probe_validate(const SourcesConfig *,const char *,size_t);
const char *sources_media_probe_validate(const SourcesConfig *,const char *,size_t);
const char *sources_media_response_validate(const SourcesConfig *,const char *,
                                            size_t,size_t,size_t);
const char *sources_native_media_frame(const SourcesConfig *,const void *,size_t,
    size_t,size_t,const char *,char **,size_t *);
const char *sources_media_frame_validate(const SourcesConfig *,const char *,size_t,
                                         size_t,size_t);
/* Same frame validation plus hashes of packed RGBA and the public Inference
 * P6/white-alpha preparation. No second decoder or unvalidated payload path. */
const char *sources_media_frame_hashes(const SourcesConfig *,const char *,size_t,
    size_t,size_t,char[65],char[65]);
const char *sources_media_frame_metadata_validate(const SourcesConfig *,const char *,size_t,
    size_t,size_t);
const char *sources_native_fetch(const SourcesConfig *,const char *,unsigned char **,size_t *);
const char *sources_native_chutni(const SourcesConfig *,const char *,char **,size_t *);
const char *sources_native_chutni_semantic(const SourcesConfig *,const char *,char **,size_t *);
const char *sources_native_chutni_read(const SourcesConfig *,const char *,char **,size_t *);
const char *sources_native_chutni_publish(const SourcesConfig *,const void *,size_t,const char *,char **,size_t *);
const char *sources_native_chutni_embedding(const SourcesConfig *,const char *,char **,size_t *);
#endif
