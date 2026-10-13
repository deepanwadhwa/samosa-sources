#ifndef SOURCES_MEDIA_INPUT_H
#define SOURCES_MEDIA_INPUT_H
#include "sources.h"
#include <libavformat/avformat.h>
/* Optional worker memory transport shared by audio and visual decoding. */
typedef struct {
    const unsigned char *bytes;
    size_t length,offset;
    const SourcesConfig *cfg;
    int secondary_refused;
} SourcesMediaInput;
int sources_media_read_memory(void *,uint8_t *,int);
int64_t sources_media_seek_memory(void *,int64_t,int);
int sources_media_interrupt(void *);
int sources_media_refuse_secondary(AVFormatContext *,AVIOContext **,
                                   const char *,int,AVDictionary **);
#endif
