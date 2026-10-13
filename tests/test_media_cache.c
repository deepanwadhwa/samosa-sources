#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include "sha256.h"
#include <assert.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SAMOSA_SOURCES_TEST_FAULTS
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif
static int visual;
static char *call(SourcesConfig *cfg,const char *request,int status) {
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);assert(out);
    int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
    if(rc!=status)fprintf(stderr,"media cache: %s\n",body);
    assert(rc==status);return body;
}
static void store(sqlite3 *db,const char *body,int bad_digest) {
    char digest[65];hash_bytes(body,strlen(body),digest);
    sqlite3_stmt *s=NULL;
    assert(sqlite3_prepare_v2(db,"UPDATE prepared_artifacts SET response=?,digest=?",-1,&s,NULL)==SQLITE_OK);
    sqlite3_bind_text(s,1,body,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(s,2,bad_digest?"wrong":digest,-1,SQLITE_TRANSIENT);
    assert(sqlite3_step(s)==SQLITE_DONE && sqlite3_changes(db)==1);sqlite3_finalize(s);
}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
typedef struct {SourcesConfig *cfg;const char *request,*good,*operation;int pid,ready,started,descendant,expire;double expires_at;} ControlProbe;
static void control_event(void *context,int pid,const char *name,SourcesConfig *work) {
    ControlProbe *p=context;
    if(!strcmp(name,"descendant")){p->descendant=pid;return;}
    p->pid=pid;
    if(!strcmp(name,"started")){p->started++;return;}
    assert(!strcmp(name,"fault_ready"));p->ready=1;
    char cancel[512],get[512];
    snprintf(cancel,sizeof(cancel),"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"%s\"}",p->operation);
    snprintf(get,sizeof(get),"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"%s\"}",p->operation);
    SourcesConfig foreign=*p->cfg;foreign.owner="bob";
    char *body=call(&foreign,cancel,1);assert(strstr(body,"unknown_operation"));free(body);
    body=call(p->cfg,get,0);
    assert(strstr(body,"\"status\":\"running\"")&&strstr(body,"\"cancel_requested\":false"));free(body);
    body=call(p->cfg,p->request,0);assert(!strcmp(body,p->good));free(body);
    assert(!kill(pid,0));
    if(p->expire) {
        struct timespec now;assert(!clock_gettime(CLOCK_MONOTONIC,&now));
        p->expires_at=now.tv_sec+now.tv_nsec/1e9+0.05;
        work->deadline_monotonic=p->expires_at;
        return;
    }
    body=call(p->cfg,cancel,0);assert(strstr(body,"\"cancel_requested\":true"));free(body);
}
static void durable_control(SourcesConfig *cfg,const char *request,const char *other,const char *good,int decoder) {
    char tracked[1024];size_t n=strlen(other);
    const char *operation=decoder==3?"media-controlled-deadline":decoder==2?"media-controlled-descendant":decoder?"media-controlled-decoder":"media-controlled-fingerprint";
    snprintf(tracked,sizeof(tracked),"%.*s,\"operation_id\":\"%s\",\"parent_operation_id\":\"media-parent\",\"timeout_ms\":20000}",(int)n-1,other,operation);
    ControlProbe probe={.cfg=cfg,.request=request,.good=good,.operation=operation,.expire=decoder==3};
    SourcesConfig work=*cfg;work.native_test_fault=decoder==2?(visual?"media_frame_descendant_stall":"media_audio_descendant_stall"):decoder?(visual?"media_frame_stall":"media_audio_stall"):"stall";
    work.native_test_event=control_event;work.native_test_context=&probe;
    char *failed=call(&work,tracked,1);
    assert(probe.started==(decoder?2:1));
    assert(probe.ready&&strstr(failed,decoder==3?"deadline_exceeded":"cancelled")&&strstr(failed,"media-parent")&&!strstr(failed,"artifact_id"));
    if(probe.expire) {
        struct timespec now;assert(!clock_gettime(CLOCK_MONOTONIC,&now));
        assert(now.tv_sec+now.tv_nsec/1e9-probe.expires_at<1);
    }
    int status;errno=0;assert(waitpid(probe.pid,&status,WNOHANG)==-1&&errno==ECHILD);
    assert(kill(probe.pid,0)==-1&&errno==ESRCH);
    if(decoder==2) {
        assert(probe.descendant>0);int gone=0;
        for(unsigned i=0;i<200;i++) {
#ifdef __linux__
            pid_t reaped=waitpid(probe.descendant,&status,WNOHANG);
            if(reaped==probe.descendant)assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
#endif
            if(kill(probe.descendant,0)==-1&&errno==ESRCH){gone=1;break;}
            struct timespec delay={0,10000000};nanosleep(&delay,NULL);
        }
        assert(gone);
    }
    SourcesConfig offline=*cfg;offline.media_helper=NULL;
    char *retry=call(&offline,tracked,1);assert(!strcmp(failed,retry));free(retry);free(failed);
    char *hit=call(cfg,request,0);assert(!strcmp(hit,good));free(hit);
    puts("PASS public durable media cancellation: owner refusal, active status, healthy cache peer, exact reap, offline retry and survivor");
}
#endif
int main(int argc,char **argv) {
    assert(argc>=3&&argc<=5);
    int uploaded=0;for(int i=3;i<argc;i++){if(!strcmp(argv[i],"--visual"))visual=1;else{assert(!strcmp(argv[i],"--upload"));uploaded=1;}}
#if defined(SAMOSA_SOURCES_TEST_FAULTS) && defined(__linux__)
    assert(!prctl(PR_SET_CHILD_SUBREAPER,1));
#endif
    char scratch[]="/tmp/samosa-media-cache-XXXXXX";assert(mkdtemp(scratch));
    char state[512],input[512],database[512];
    snprintf(state,sizeof(state),"%s/state",scratch);
    snprintf(input,sizeof(input),"%s/media.bin",scratch);
    snprintf(database,sizeof(database),"%s/state/sources.sqlite3",scratch);
    FILE *in=fopen(argv[2],"rb"),*out=fopen(input,"wb");assert(in&&out);
    unsigned char buffer[4096];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),in)))assert(fwrite(buffer,1,n,out)==n);
    assert(!ferror(in));fclose(in);assert(!fclose(out));
    SourcesConfig cfg={.root=scratch,.state=state,.owner="alice",.media_helper=argv[1]};
    char *registered=NULL;
    if(uploaded){cfg.root=NULL;in=fopen(input,"rb");assert(in&&!fseek(in,0,SEEK_END));long size=ftell(in);assert(size>0&&(size_t)size<=SOURCES_INPUT_MAX&&!fseek(in,0,SEEK_SET));unsigned char *bytes=malloc((size_t)size);assert(bytes&&fread(bytes,1,(size_t)size,in)==(size_t)size&&!fclose(in));size_t response_length;out=open_memstream(&registered,&response_length);assert(out);assert(!sources_upload(&cfg,SOURCES_UPLOAD_MEDIA,bytes,(size_t)size,out)&&!fclose(out));free(bytes);}
    else registered=call(&cfg,"{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"media.bin\"}",0);
    Json j;assert(json_parse(&j,registered,strlen(registered)));
    char *id=json_string(&j,json_get(&j,0,"source_id"));assert(id);free(registered);
    char request[512],other[512],inspection_request[512],*inspection=NULL;
    if(!visual){
        snprintf(inspection_request,sizeof(inspection_request),"{\"contract_version\":1,\"op\":\"inspect_audio\",\"source_id\":\"%s\",\"version\":\"%s\"}",id,id);
        inspection=call(&cfg,inspection_request,0);assert(json_parse(&j,inspection,strlen(inspection)));
        size_t duration,frames;assert(json_uint(&j,json_get(&j,0,"duration_ms"),&duration)&&duration>=2000&&duration<=31000);
        assert(json_uint(&j,json_get(&j,0,"frames"),&frames)&&duration==frames/16);
        assert(strstr(inspection,"audio_timeline")&&strstr(inspection,"decoded_samples_floor_ms")&&!strstr(inspection,"evidence_id")&&!strstr(inspection,"\"data\""));
        SourcesConfig foreign=cfg;foreign.owner="inspection-other";char *bad=call(&foreign,inspection_request,1);assert(strstr(bad,"unknown_source"));free(bad);
        SourcesConfig expired=cfg;expired.deadline_monotonic=1;bad=call(&expired,inspection_request,1);assert(strstr(bad,"deadline_exceeded"));free(bad);
        char tracked[768];snprintf(tracked,sizeof(tracked),"%.*s,\"operation_id\":\"media-inspection\",\"timeout_ms\":20000}",(int)strlen(inspection_request)-1,inspection_request);
        char *saved=call(&cfg,tracked,0);SourcesConfig offline=cfg;offline.media_helper=NULL;
        bad=call(&offline,tracked,0);assert(!strcmp(saved,bad));free(saved);free(bad);
    }
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500}",visual?"prepare_visual":"prepare_audio",id,id);
    snprintf(other,sizeof(other),"{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1500,\"end_ms\":2000}",visual?"prepare_visual":"prepare_audio",id,id);
    char *good=call(&cfg,request,0);assert(!unlink(input));cfg.root=NULL;
    if(inspection){char *saved=call(&cfg,inspection_request,0);assert(!strcmp(saved,inspection));free(saved);free(inspection);}
    if(visual) {
        assert(json_parse(&j,good,strlen(good)));
        int decoder=json_get(&j,0,"decoder");size_t width,height,timestamp;
        assert(json_uint(&j,json_get(&j,decoder,"width"),&width)&&width==64);
        assert(json_uint(&j,json_get(&j,decoder,"height"),&height)&&height==64);
        assert(json_uint(&j,json_get(&j,decoder,"timestamp_us"),&timestamp)&&timestamp==1000000);
        char *data=json_string(&j,json_get(&j,0,"data"));assert(data);
        const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        unsigned bits=0,value=0;size_t pixels=0;
        for(const char *p=data;*p&&*p!='=';p++) {
            const char *digit=strchr(alphabet,*p);assert(digit);
            value=(value<<6)|(unsigned)(digit-alphabet);bits+=6;
            if(bits>=8) {
                bits-=8;unsigned byte=(value>>bits)&255;
                assert(byte==(pixels%4<2?0:255));pixels++;
            }
        }
        assert(pixels==64*64*4);free(data);
    }
    cfg.cache_limit=1;char *failed=call(&cfg,other,1);assert(strstr(failed,"cache_limit"));free(failed);
    char *hit=call(&cfg,request,0);assert(!strcmp(hit,good));free(hit);
    cfg.cache_limit=0;cfg.cache_byte_limit=strlen(good)+1;
    failed=call(&cfg,other,1);assert(strstr(failed,"cache_limit"));free(failed);cfg.cache_byte_limit=0;
    sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);
    store(db,good,1);failed=call(&cfg,request,1);assert(strstr(failed,"corrupt_cache"));free(failed);
    char *tampered=strdup(good);assert(tampered);
    char *data=strstr(tampered,"\"data\":\"");assert(data);data[8]=visual?'!':'A';
    store(db,tampered,0);failed=call(&cfg,request,1);assert(strstr(failed,"corrupt_cache"));free(failed);
    strcpy(tampered,good);const char *field=visual?"\"width\":" : "\"source_channels\":";
    char *channels=strstr(tampered,field);assert(channels);channels[strlen(field)]='0';
    store(db,tampered,0);failed=call(&cfg,request,1);assert(strstr(failed,"corrupt_cache"));free(failed);
    store(db,good,0);hit=call(&cfg,request,0);assert(!strcmp(hit,good));free(hit);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    durable_control(&cfg,request,other,good,0);
    durable_control(&cfg,request,other,good,1);
    durable_control(&cfg,request,other,good,2);
    durable_control(&cfg,request,other,good,3);
#endif
    if(visual) {
        char audio_request[512];
        snprintf(audio_request,sizeof(audio_request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500}",id,id);
        cfg.cache_limit=1;failed=call(&cfg,audio_request,1);
        assert(strstr(failed,"cache_limit"));free(failed);cfg.cache_limit=0;
        char *audio=call(&cfg,audio_request,0);
        Json visual_json,audio_json;
        assert(json_parse(&visual_json,good,strlen(good))&&json_parse(&audio_json,audio,strlen(audio)));
        char *visual_id=json_string(&visual_json,json_get(&visual_json,0,"artifact_id"));
        char *audio_id=json_string(&audio_json,json_get(&audio_json,0,"artifact_id"));
        assert(visual_id&&audio_id&&strcmp(visual_id,audio_id));
        free(visual_id);free(audio_id);free(audio);
        hit=call(&cfg,request,0);assert(!strcmp(hit,good));free(hit);
    }
    sqlite3_stmt *count=NULL;assert(sqlite3_prepare_v2(db,"SELECT count(*) FROM prepared_artifacts",-1,&count,NULL)==SQLITE_OK);
    assert(sqlite3_step(count)==SQLITE_ROW && sqlite3_column_int(count,0)==(visual?2:1));sqlite3_finalize(count);sqlite3_close(db);
    free(tampered);free(good);free(id);
    assert(!unlink(database));
    const char *names[]={"sources.sqlite3-wal","sources.sqlite3-shm","operations.lock"};
    for(size_t i=0;i<sizeof(names)/sizeof(*names);i++){char path[1024];snprintf(path,sizeof(path),"%s/%s",state,names[i]);unlink(path);}
    /* Retain any additional private files for inspection instead of broad cleanup. */
    rmdir(state);rmdir(scratch);
    puts(visual?"PASS visual cache: exact color pixels, shared audio/visual limits, distinct artifacts, digest/payload/decoder tamper, controls and restored survivor":"PASS compressed cache: count/byte refusal, old hits, digest/payload/decoder tamper and restored survivor");
    return 0;
}
