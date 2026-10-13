#include "media_input.h"
#include "control.h"
#include <libavutil/error.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int sources_media_read_memory(void *opaque,uint8_t *out,int requested) {
    SourcesMediaInput *input=opaque;
    if(sources_interrupted(input->cfg))return AVERROR_EXIT;
    if(requested<=0)return AVERROR(EINVAL);
    size_t n=input->length-input->offset;
    if(!n)return AVERROR_EOF;
    if(n>(size_t)requested)n=(size_t)requested;
    memcpy(out,input->bytes+input->offset,n);input->offset+=n;return (int)n;
}
int64_t sources_media_seek_memory(void *opaque,int64_t offset,int whence) {
    SourcesMediaInput *input=opaque;
    if(sources_interrupted(input->cfg))return AVERROR_EXIT;
    if(whence&AVSEEK_SIZE)return (int64_t)input->length;
    whence&=~AVSEEK_FORCE;
    int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)input->offset:
                 whence==SEEK_END?(int64_t)input->length:-1;
    if(base<0||offset< -base||offset>(int64_t)input->length-base)return AVERROR(EINVAL);
    input->offset=(size_t)(base+offset);return (int64_t)input->offset;
}
int sources_media_interrupt(void *opaque) {return sources_interrupted(((SourcesMediaInput*)opaque)->cfg)!=NULL;}
int sources_media_refuse_secondary(AVFormatContext *context,AVIOContext **io,
                            const char *url,int flags,AVDictionary **options) {
    (void)io;(void)url;(void)flags;(void)options;
    SourcesMediaInput *input=context->opaque;
    if(input)input->secondary_refused=1;
    return AVERROR(EACCES);
}
