#include "media_video.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int cancelled(void *unused){(void)unused;return 1;}
int main(int argc,char **argv) {
    assert(argc==2||argc==3);FILE *input=fopen(argv[1],"rb");assert(input);
    unsigned char *bytes=malloc(SOURCES_INPUT_MAX+1);assert(bytes);
    size_t n=fread(bytes,1,SOURCES_INPUT_MAX+1,input);assert(n&&!ferror(input));fclose(input);
    SourcesConfig cfg={0};SourcesMediaFrame frame={0};
    SourcesMediaVideoInfo info={0};
    const char *error=sources_media_video_probe(&cfg,bytes,n,&info);
    assert(!error&&info.duration_ms==2000&&info.source_width==64&&info.source_height==64&&strlen(info.fingerprint)==64);
    error=sources_media_video_frame(&cfg,bytes,n,1000,1500,&frame);
    if(error){fprintf(stderr,"video: %s\n",error);return 1;}
    assert(frame.rgba&&frame.width==64&&frame.height==64&&frame.bytes==64*64*4);
    assert(frame.timestamp_us>=1000000&&frame.timestamp_us<1500000&&strlen(frame.fingerprint)==64);
    /* Actual native fixture is blue; conversion must retain its color. */
    for(size_t i=0;i<frame.bytes;i+=4)assert(frame.rgba[i]<10&&frame.rgba[i+1]<10&&frame.rgba[i+2]>240&&frame.rgba[i+3]==255);
    if(argc==3) {
        FILE *reference=fopen(argv[2],"rb");assert(reference);
        unsigned char *expected=malloc(frame.bytes+1);assert(expected);
        assert(fread(expected,1,frame.bytes+1,reference)==frame.bytes&&!ferror(reference));
        assert(!memcmp(expected,frame.rgba,frame.bytes));free(expected);fclose(reference);
    }
    sources_media_frame_free(&frame);
    error=sources_media_video_frame(&cfg,bytes,n,60000,60100,&frame);
    assert(error&&!strcmp(error,"invalid_range")&&!frame.rgba);
    cfg.cancelled=cancelled;error=sources_media_video_probe(&cfg,bytes,n,&info);
    assert(error&&!strcmp(error,"cancelled")&&!info.duration_ms&&!info.fingerprint[0]);
    error=sources_media_video_frame(&cfg,bytes,n,1000,1500,&frame);
    assert(error&&!strcmp(error,"cancelled")&&!frame.rgba);cfg.cancelled=NULL;
    cfg.deadline_monotonic=1;error=sources_media_video_probe(&cfg,bytes,n,&info);
    assert(error&&!strcmp(error,"deadline_exceeded")&&!info.duration_ms);
    error=sources_media_video_frame(&cfg,bytes,n,1000,1500,&frame);
    assert(error&&!strcmp(error,"deadline_exceeded")&&!frame.rgba);cfg.deadline_monotonic=0;
    error=sources_media_video_frame(&cfg,"malformed",9,1000,1500,&frame);
    assert(error&&!strcmp(error,"invalid_media")&&!frame.rgba);
    error=sources_media_video_probe(&cfg,"malformed",9,&info);
    assert(error&&!strcmp(error,"invalid_media")&&!info.duration_ms);
    free(bytes);puts("PASS native timed color frame, source timestamp, bounded RGBA and uncovered-time refusal");return 0;
}
