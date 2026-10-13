#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "media_decode.h"
#include "media_artifacts.h"
#include "audio.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL media decoder %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static int cancelled(void *context) {(void)context;return 1;}
int main(int argc,char **argv) {
    if(argc!=2&&argc!=3)return 2;
    FILE *f=fopen(argv[1],"rb");CHECK(f);
    unsigned char *bytes=malloc(SOURCES_INPUT_MAX+1);CHECK(bytes);
    size_t n=fread(bytes,1,SOURCES_INPUT_MAX+1,f);CHECK(n&&!ferror(f));CHECK(!fclose(f));
    SourcesConfig cfg={0};SourcesMediaAudio result={0};
    SourcesMediaArtifacts artifacts;char fingerprint[65];
    CHECK(!sources_media_artifacts_open(&cfg,&artifacts,fingerprint));
    CHECK(strlen(fingerprint)==64&&!sources_media_artifacts_check(&cfg,&artifacts));
    /* Identical file bytes at a new inode do not represent the mapped image. */
    char clone[]="/tmp/samosa-media-image.XXXXXX";int clone_fd=mkstemp(clone);CHECK(clone_fd>=0);
    unsigned char buffer[65536];off_t offset=0;
    while(offset<artifacts.files[0].before.st_size) {
        ssize_t take=pread(artifacts.files[0].fd,buffer,sizeof(buffer),offset);CHECK(take>0);
        size_t written=0;while(written<(size_t)take){ssize_t put=write(clone_fd,buffer+written,(size_t)take-written);CHECK(put>0);written+=(size_t)put;}
        offset+=take;
    }
    CHECK(!close(artifacts.files[0].fd));artifacts.files[0].fd=clone_fd;
    CHECK(!fstat(clone_fd,&artifacts.files[0].before));
    CHECK(strlen(clone)<sizeof(artifacts.files[0].path));strcpy(artifacts.files[0].path,clone);
    const char *changed=sources_media_artifacts_check(&cfg,&artifacts);
    CHECK(changed&&!strcmp(changed,"media_artifact_changed"));
    sources_media_artifacts_close(&artifacts);CHECK(!unlink(clone));
    const char *error=sources_media_audio_decode(&cfg,bytes,n,1000,1500,&result);
    if(error)fprintf(stderr,"decode error: %s\n",error);
    CHECK(!error&&result.wave&&result.wave_bytes==16044);
    CHECK(!strcmp(result.fingerprint,fingerprint));
    SourcesWave wave;CHECK(!sources_wave_inspect(NULL,result.wave,result.wave_bytes,&wave));
    CHECK(wave.frames==8000&&result.source_channels>=1&&result.source_channels<=8);
    CHECK(result.source_rate>=8000&&result.source_rate<=192000);
    if(argc==3) {
        FILE *reference=fopen(argv[2],"rb");CHECK(reference);
        unsigned char expected[16001];size_t count=fread(expected,1,sizeof(expected),reference);
        CHECK(!ferror(reference)&&!fclose(reference));
        CHECK(count==16000&&!memcmp(wave.pcm,expected,16000));
    }
    if(n>12&&!memcmp(bytes,"RIFF",4)&&!memcmp(bytes+8,"WAVE",4)) {
        SourcesWave original;
        if(!sources_wave_inspect(NULL,bytes,n,&original))
            CHECK(!memcmp(wave.pcm,original.pcm+32000,16000));
    }
    sources_media_audio_free(&result);
    error=sources_media_audio_probe(&cfg,bytes,n,&result);
    if(error)fprintf(stderr,"probe error: %s\n",error);
    /* Encoded fixtures originate from at most31s PCM; some platform decoders
     * retain AAC's final padding, so the usable decoded timeline can exceed31s. */
    CHECK(!error&&!result.wave&&!result.wave_bytes&&result.frames>=24000&&result.frames<=512000);
    CHECK(!strcmp(result.fingerprint,fingerprint));
    size_t full_frames=result.frames,whole_ms=full_frames/16;
    sources_media_audio_free(&result);
    error=sources_media_audio_decode(&cfg,bytes,n,whole_ms-1,whole_ms,&result);
    CHECK(!error&&result.frames==16&&result.wave_bytes==76);
    sources_media_audio_free(&result);
    error=sources_media_audio_decode(&cfg,bytes,n,whole_ms,whole_ms+1,&result);
    CHECK(error&&!strcmp(error,"invalid_range")&&!result.wave);
    cfg.cancelled=cancelled;
    error=sources_media_audio_probe(&cfg,bytes,n,&result);
    CHECK(error&&!strcmp(error,"cancelled")&&!result.wave&&!result.frames);
    error=sources_media_audio_decode(&cfg,bytes,n,1000,1500,&result);
    CHECK(error&&!strcmp(error,"cancelled")&&!result.wave);
    cfg.cancelled=NULL;cfg.deadline_monotonic=1;
    error=sources_media_audio_probe(&cfg,bytes,n,&result);
    CHECK(error&&!strcmp(error,"deadline_exceeded")&&!result.wave&&!result.frames);
    error=sources_media_audio_decode(&cfg,bytes,n,1000,1500,&result);
    CHECK(error&&!strcmp(error,"deadline_exceeded")&&!result.wave);
    cfg.deadline_monotonic=0;
    /* These native fixtures are at most31s. A later selection must refuse
     * uncovered source time rather than manufacturing a padded waveform. */
    error=sources_media_audio_decode(&cfg,bytes,n,60000,60100,&result);
    CHECK(error&&!strcmp(error,"invalid_range")&&!result.wave&&!result.wave_bytes);
    error=sources_media_audio_decode(&cfg,bytes,n,1000,1000,&result);
    CHECK(error&&!strcmp(error,"invalid_range")&&!result.wave);
    error=sources_media_audio_decode(&cfg,"malformed",9,1000,1500,&result);
    CHECK(error&&!result.wave);
    free(bytes);
    printf("PASS optional native media primitive: %u checks; actual decode/resample, selected PCM shape, WAV exact samples, cancel/deadline and invalid input\n",checks);
    return 0;
}
