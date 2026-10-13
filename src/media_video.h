#ifndef SOURCES_MEDIA_VIDEO_H
#define SOURCES_MEDIA_VIDEO_H
#include "sources.h"
/* Container/selected-video timeline, not decoded-frame coverage. */
typedef struct {
    size_t duration_ms;
    unsigned stream,source_width,source_height;
    long long origin_us;
    char fingerprint[65];
} SourcesMediaVideoInfo;
const char *sources_media_video_probe(const SourcesConfig *,const void *,size_t,
                                      SourcesMediaVideoInfo *);
typedef struct {
    unsigned char *rgba;size_t bytes;
    unsigned width,height,source_width,source_height,stream,orientation;
    long long timestamp_us,origin_us;
    int colorspace,color_range;
    unsigned source_transfer,source_primaries,output_transfer,output_primaries,tone_mapped;
    char fingerprint[65];
} SourcesMediaFrame;
const char *sources_media_video_frame(const SourcesConfig *,const void *,size_t,
                                      size_t,size_t,SourcesMediaFrame *);
const char *sources_media_video_last_frame(const SourcesConfig *,const void *,size_t,size_t,size_t,SourcesMediaFrame *);
const char *sources_media_last_frame_response(const SourcesConfig *,const void *,size_t,size_t,size_t,FILE *);
const char *sources_video_probe_response(const SourcesConfig *,const void *,size_t,FILE *);
void sources_media_frame_free(SourcesMediaFrame *);
const char *sources_media_frame_response(const SourcesConfig *,const void *,size_t,
                                         size_t,size_t,FILE *);
#endif
