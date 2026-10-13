#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "media_artifacts.h"
#include "media_decode.h"
#include "control.h"
#include "sha256.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
static int same(const struct stat *a,const struct stat *b) {
    if(a->st_dev!=b->st_dev||a->st_ino!=b->st_ino||a->st_size!=b->st_size)return 0;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec==b->st_mtimespec.tv_sec&&a->st_mtimespec.tv_nsec==b->st_mtimespec.tv_nsec&&a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec&&a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
#endif
}
static int mapped_file(void *address,const struct stat *file) {
#ifdef __APPLE__
    struct proc_regionwithpathinfo region;
    int n=proc_pidinfo(getpid(),PROC_PIDREGIONPATHINFO,(uint64_t)(uintptr_t)address,&region,sizeof(region));
    if(n!=(int)sizeof(region))return 0;
    const struct vinfo_stat *mapped=&region.prp_vip.vip_vi.vi_stat;
    return mapped->vst_ino==(uint64_t)file->st_ino&&mapped->vst_dev==(uint32_t)file->st_dev;
#elif defined(__linux__)
    FILE *maps=fopen("/proc/self/maps","r");if(!maps)return 0;
    char line[8192];int found=0;unsigned lines=0;
    while(++lines<=4096&&fgets(line,sizeof(line),maps)) {
        unsigned long long start,end,offset,inode;unsigned major_id,minor_id;char permissions[5];
        if(sscanf(line,"%llx-%llx %4s %llx %x:%x %llu",&start,&end,permissions,&offset,&major_id,&minor_id,&inode)!=7)continue;
        uintptr_t at=(uintptr_t)address;
        if(at>=start&&at<end){found=inode==(unsigned long long)file->st_ino&&major_id==major(file->st_dev)&&minor_id==minor(file->st_dev);break;}
    }
    fclose(maps);return found;
#else
    (void)address;(void)file;return 0;
#endif
}
void sources_media_artifacts_close(SourcesMediaArtifacts *artifacts) {
    for(unsigned i=0;i<SOURCES_MEDIA_ARTIFACTS_COUNT;i++){if(artifacts->files[i].fd>=0)close(artifacts->files[i].fd);artifacts->files[i].fd=-1;}
}
const char *sources_media_artifacts_check(const SourcesConfig *cfg,const SourcesMediaArtifacts *artifacts) {
    const char *error=sources_interrupted(cfg);if(error)return error;
    for(unsigned i=0;i<SOURCES_MEDIA_ARTIFACTS_COUNT;i++) {
        const SourcesMediaArtifact *a=&artifacts->files[i];struct stat now,path;
        if(i==7&&!a->address&&a->fd<0)continue;
        if(a->fd<0||fstat(a->fd,&now)||stat(a->path,&path)||!same(&a->before,&now)||
           !same(&a->before,&path)||!mapped_file(a->address,&now))return "media_artifact_changed";
    }
    return NULL;
}
const char *sources_media_artifacts_open(const SourcesConfig *cfg,SourcesMediaArtifacts *artifacts,char fingerprint[65]) {
    memset(artifacts,0,sizeof(*artifacts));for(unsigned i=0;i<SOURCES_MEDIA_ARTIFACTS_COUNT;i++)artifacts->files[i].fd=-1;
    const char *names[]={NULL,"av_read_frame","avcodec_receive_frame","swr_convert","av_malloc","sws_scale","avfilter_graph_alloc","zimg_get_api_version"};
    const char *error=sources_interrupted(cfg);Sha256 combined;sha256_init(&combined);
    const char *contract="media-worker-v6:memory-only:PCM16-mono16000:RGBA512-bilinear-orthogonal:HDR-Hable-sRGB-peak10:visual-keyframe-seek30s:audio-seek30s-preroll2s-grid:probe-packets1000000:source-ms:30s";
    char contract_digest[65];hash_bytes(contract,strlen(contract),contract_digest);
    sha256_update(&combined,contract_digest,64);
    for(unsigned i=0;i<SOURCES_MEDIA_ARTIFACTS_COUNT&&!error;i++) {
        SourcesMediaArtifact *a=&artifacts->files[i];
        if(!i) {
            const char *(*entry)(const SourcesConfig*,const void*,size_t,size_t,size_t,SourcesMediaAudio*)=sources_media_audio_decode;
            _Static_assert(sizeof(entry)==sizeof(a->address),"supported function address representation");
            memcpy(&a->address,&entry,sizeof(a->address));
        } else a->address=dlsym(RTLD_DEFAULT,names[i]);
        if(i==7&&!a->address){sha256_update(&combined,"no-zimg",7);continue;}
        Dl_info info;
        if(!a->address||!dladdr(a->address,&info)||!info.dli_fname||!realpath(info.dli_fname,a->path)){error="media_artifact_unavailable";break;}
        a->fd=open(a->path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(a->fd<0||fstat(a->fd,&a->before)||!S_ISREG(a->before.st_mode)||a->before.st_size<=0||a->before.st_size>128LL*1048576||!mapped_file(a->address,&a->before)){error="media_artifact_unavailable";break;}
        Sha256 digest;sha256_init(&digest);unsigned char buffer[65536],result[32];off_t offset=0;
        while(offset<a->before.st_size) {
            error=sources_interrupted(cfg);if(error)break;
            size_t wanted=(size_t)(a->before.st_size-offset);if(wanted>sizeof(buffer))wanted=sizeof(buffer);
            ssize_t n=pread(a->fd,buffer,wanted,offset);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){error="media_artifact_changed";break;}
            sha256_update(&digest,buffer,(size_t)n);offset+=n;
        }
        if(!error){sha256_final(&digest,result);sha256_update(&combined,&i,sizeof(i));sha256_update(&combined,result,sizeof(result));}
    }
    if(!error)error=sources_media_artifacts_check(cfg,artifacts);
    if(!error){unsigned char digest[32];sha256_final(&combined,digest);sha256_hex(digest,fingerprint);}
    if(error){fingerprint[0]=0;sources_media_artifacts_close(artifacts);}return error;
}
