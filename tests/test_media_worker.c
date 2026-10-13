#define _POSIX_C_SOURCE 200809L
#include "native.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int visual,probing;
static const char *decode(const SourcesConfig *cfg,const void *bytes,size_t length,
    size_t start,size_t end,const char *fp,char **body,size_t *n) {
    return probing?sources_native_media_probe(cfg,bytes,length,fp,body,n):visual?sources_native_media_frame(cfg,bytes,length,start,end,fp,body,n):
        sources_native_media_audio(cfg,bytes,length,start,end,fp,body,n);
}
static const char *validate(const SourcesConfig *cfg,const char *body,size_t n,
    size_t start,size_t end) {
    return probing?sources_media_probe_validate(cfg,body,n):visual?sources_media_frame_validate(cfg,body,n,start,end):
        sources_media_response_validate(cfg,body,n,start,end);
}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
typedef struct {int fd,descendant;} DeathProbe;
static void death_event(void *context,int pid,const char *name,SourcesConfig *cfg) {
    (void)cfg;DeathProbe *p=context;
    if(!strcmp(name,"descendant"))p->descendant=pid;
    if(!strcmp(name,"fault_ready")) {
        int ids[2]={pid,p->descendant};
        assert(write(p->fd,ids,sizeof(ids))==(ssize_t)sizeof(ids));
    }
}
static void caller_death(SourcesConfig cfg,const void *bytes,size_t length,const char *fp) {
#ifdef __linux__
    assert(!prctl(PR_SET_CHILD_SUBREAPER,1));
#endif
    int ready[2];assert(!pipe(ready));pid_t caller=fork();assert(caller>=0);
    if(!caller) {
        close(ready[0]);DeathProbe p={.fd=ready[1]};
        cfg.native_test_fault="descendant_stall";cfg.native_test_event=death_event;
        cfg.native_test_context=&p;
        char *body=NULL;size_t n=0;
        decode(&cfg,bytes,length,1000,1500,fp,&body,&n);
        _exit(3);
    }
    close(ready[1]);struct pollfd descriptor={.fd=ready[0],.events=POLLIN};
    int readable=poll(&descriptor,1,5000),ids[2]={0};
    ssize_t got=readable>0?read(ready[0],ids,sizeof(ids)):0;close(ready[0]);
    assert(!kill(caller,SIGKILL));int status;
    assert(waitpid(caller,&status,0)==caller&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    assert(got==(ssize_t)sizeof(ids)&&ids[0]>0&&ids[1]>0);
    for(unsigned i=0;i<2;i++) {
        int gone=0;
        for(unsigned attempt=0;attempt<200;attempt++) {
#ifdef __linux__
            pid_t reaped=waitpid(ids[i],&status,WNOHANG);
            if(reaped==ids[i])assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
#endif
            if(kill(ids[i],0)==-1&&errno==ESRCH){gone=1;break;}
            struct timespec delay={0,10000000};nanosleep(&delay,NULL);
        }
        if(!gone){kill(-ids[i],SIGKILL);kill(ids[i],SIGKILL);}
        assert(gone);
    }
    char *body=NULL;size_t n=0;
    assert(!decode(&cfg,bytes,length,1000,1500,fp,&body,&n));
    assert(body&&n);free(body);
    puts("PASS media caller SIGKILL: worker/descendant death and healthy survivor");
}
typedef struct {
    int pid, ready, expire, cancel, memory, peer;
    double ready_at;
    SourcesConfig *healthy;
    const void *bytes; size_t length; const char *fingerprint;
} Probe;
static double monotonic(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return t.tv_sec+t.tv_nsec/1e9;
}
static void event(void *context,int pid,const char *name,SourcesConfig *cfg) {
    Probe *p=context; assert(pid>0); p->pid=pid;
    if (!strcmp(name,"fault_ready")) {
        p->ready=1; p->ready_at=monotonic();
        if(p->healthy) {
            char *body=NULL;size_t n=0;
            assert(!decode(p->healthy,p->bytes,p->length,
                1000,1500,p->fingerprint,&body,&n));
            assert(body && n);free(body);p->peer=1;
            /* The synchronous healthy-peer call completes before the parent can
             * observe cancellation. Measure teardown from that observation point,
             * excluding the independent peer's real decode/probe work. */
            p->ready_at=monotonic();
        }
        if (p->expire) cfg->deadline_monotonic=p->ready_at+0.05;
        if (p->memory) cfg->native_memory_limit=64u*1024u*1024u;
    } else assert(!strcmp(name,"started"));
}
static int active_cancel(void *context) { Probe *p=context; return p->cancel && p->ready; }
static void faults(SourcesConfig cfg,const void *bytes,size_t length,const char *fp) {
    const char *names[]={"abort","early_exit","bad_protocol","huge_output","bad_result","stall","stall","memory"};
    for (size_t i=0;i<sizeof(names)/sizeof(*names);i++) {
        Probe p={.expire=i==5,.cancel=i==6,.memory=i==7};
        if(i==6) {p.healthy=&cfg;p.bytes=bytes;p.length=length;p.fingerprint=fp;}
        SourcesConfig work=cfg;
        work.native_test_fault=names[i]; work.native_test_event=event;
        work.native_test_context=&p; work.cancelled=active_cancel; work.cancellation=&p;
        work.deadline_monotonic=monotonic()+20;
        char *body=NULL; size_t n=0;
        const char *error=decode(&work,bytes,length,1000,1500,fp,&body,&n);
        const char *expected=i<2?"native_helper_failed":i<5?"native_helper_protocol":i==5?"deadline_exceeded":i==6?"cancelled":"native_helper_memory_limit";
        if (!error || strcmp(error,expected)) fprintf(stderr,"fault %s: %s\n",names[i],error?error:"success");
        assert(error && !strcmp(error,expected) && !body && !n && p.ready);
        if(i==6)assert(p.peer);
        assert(monotonic()-p.ready_at<2);
        int status; errno=0;
        assert(waitpid(p.pid,&status,WNOHANG)==-1 && errno==ECHILD);
        assert(kill(p.pid,0)==-1 && errno==ESRCH);
        /* A failed/stalled worker must not prevent a subsequent healthy decode. */
        assert(!decode(&cfg,bytes,length,1000,1500,fp,&body,&n));
        assert(body && n); free(body);
    }
    puts("PASS media worker faults, active cancellation/deadline, RSS limit, exact reap, concurrent native peer and healthy survivors");
}
#endif
static int cancelled(void *unused) { (void)unused; return 1; }
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--sources-native-worker"))
        return sources_native_worker_main(argc,argv);
    assert(argc==3||argc==4);
    visual=argc==4;
    if(visual)assert(!strcmp(argv[3],"--visual"));
    FILE *input=fopen(argv[2],"rb"); assert(input);
    assert(!fseek(input,0,SEEK_END)); long length=ftell(input);
    assert(length>0 && length<=SOURCES_INPUT_MAX); rewind(input);
    unsigned char *bytes=malloc((size_t)length); assert(bytes);
    assert(fread(bytes,1,(size_t)length,input)==(size_t)length); fclose(input);
    SourcesConfig cfg={.media_helper=argv[1],.native_helper="/deliberately/unavailable/plain/helper"};
    char *body=NULL; size_t n=0;
    char fingerprint[65];
    SourcesConfig minimal={0};
    assert(!strcmp(sources_native_media_fingerprint(&minimal,fingerprint),"media_unavailable"));
    assert(!fingerprint[0]);
    const char *error=sources_native_media_fingerprint(&cfg,fingerprint);
    if (error) { fprintf(stderr,"media fingerprint: %s\n",error); return 1; }
    assert(strlen(fingerprint)==64);
    if(!visual){
        error=sources_native_media_probe(&cfg,bytes,(size_t)length,fingerprint,&body,&n);
        if(error)fprintf(stderr,"media probe: %s\n",error);
        assert(!error&&body&&n&&!sources_media_probe_validate(&cfg,body,n));
        const char *keys[]={"frames","duration_ms","source_rate","source_channels","stream","sample_rate","origin_us"};
        const char *bad[]={"0","14400001","7999","9","8","1","9223372036854775808"};
        for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++){
            char key[64];snprintf(key,sizeof(key),"\"%s\":",keys[i]);
            char *value=strstr(body,key);assert(value);value+=strlen(key);char *end=value;
            if(*end=='-')end++;
            while(*end>='0'&&*end<='9')end++;
            size_t prefix=(size_t)(value-body);char *copy=malloc(n+32);assert(copy);
            memcpy(copy,body,prefix);snprintf(copy+prefix,n+32-prefix,"%s%s",bad[i],end);
            assert(!strcmp(sources_media_probe_validate(&cfg,copy,strlen(copy)),"native_helper_protocol"));free(copy);
        }
        free(body);body=NULL;n=0;
        char wrong[65];memcpy(wrong,fingerprint,65);wrong[0]=wrong[0]=='0'?'1':'0';
        assert(!strcmp(sources_native_media_probe(&cfg,bytes,(size_t)length,wrong,&body,&n),"media_artifact_changed")&&!body&&!n);
    }
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    faults(cfg,bytes,(size_t)length,fingerprint);
    caller_death(cfg,bytes,(size_t)length,fingerprint);
    if(!visual){probing=1;faults(cfg,bytes,(size_t)length,fingerprint);caller_death(cfg,bytes,(size_t)length,fingerprint);probing=0;}
#endif
    error=decode(&cfg,bytes,(size_t)length,1000,1500,fingerprint,&body,&n);
    if (error) { fprintf(stderr,"media worker: %s\n",error); return 1; }
    assert(body && n);
    assert(!validate(&cfg,body,n,1000,1500));
    assert(!strcmp(validate(&cfg,body,n,1001,1501),"native_helper_protocol"));
    if(visual) {
        const char *keys[]={"width","height","source_width","source_height",
            "rgba_bytes","timestamp_us","stream","colorspace","color_range","source_width","orientation",
            "source_transfer","source_primaries","output_transfer","output_primaries","tone_mapped"};
        for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++) {
            char key[64];snprintf(key,sizeof(key),"\"%s\":",keys[i]);
            char *value=strstr(body,key);assert(value);value+=strlen(key);
            char *end=value;while(*end>='0'&&*end<='9')end++;
            const char *bad=i<4?"0":i==4?"1":i==5?"1500000":
                i==6||i==10?"8":i==9?"65":i==11||i==13?"19":i==12||i==14?"23":"2";
            size_t prefix=(size_t)(value-body);
            char *copy=malloc(n+32);assert(copy);
            memcpy(copy,body,prefix);
            snprintf(copy+prefix,n+32-prefix,"%s%s",bad,end);
            assert(!strcmp(validate(&cfg,copy,strlen(copy),1000,1500),"native_helper_protocol"));
            free(copy);
        }
    }
    char *data=strstr(body,"\"data\":\""); assert(data); data+=8; *data='!';
    assert(!strcmp(validate(&cfg,body,n,1000,1500),"native_helper_protocol"));
    free(body);
    fingerprint[0]=fingerprint[0]=='0'?'1':'0';
    assert(!strcmp(decode(&cfg,bytes,(size_t)length,1000,1500,fingerprint,&body,&n),"media_artifact_changed"));
    assert(!body && !n);
    cfg.cancelled=cancelled;
    assert(!strcmp(decode(&cfg,bytes,(size_t)length,1000,1500,fingerprint,&body,&n),"cancelled"));
    assert(!body && !n); free(bytes);
    puts(visual?"PASS actual visual worker, DTO dimensions/timestamp/color validation, fingerprint binding and cancelled ingress":"PASS actual audio worker decode, DTO validation, range/payload refusal and cancelled ingress");
    return 0;
}
