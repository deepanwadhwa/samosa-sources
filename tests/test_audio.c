#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "audio.h"
#include "sha256.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL audio %d: %s\n",__LINE__,#x);exit(1);}} while(0)
static void put32(unsigned char *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static unsigned char *fixture(size_t ms,size_t *length) {
    *length=44+ms*32;
    unsigned char *bytes=calloc(1,*length); CHECK(bytes);
    memcpy(bytes,"RIFF",4);put32(bytes+4,(uint32_t)*length-8);
    memcpy(bytes+8,"WAVEfmt ",8);put32(bytes+16,16);
    bytes[20]=1;bytes[22]=1;put32(bytes+24,16000);put32(bytes+28,32000);
    bytes[32]=2;bytes[34]=16;memcpy(bytes+36,"data",4);put32(bytes+40,(uint32_t)*length-44);
    for(size_t i=44;i<*length;i++) {
        size_t frame=(i-44)/2;
        uint16_t sample=(uint16_t)(frame*37u+frame/257u);
        bytes[i]=(unsigned char)(sample>>(((i-44)&1)*8));
    }
    return bytes;
}
typedef struct {unsigned calls, stop;} Cancel;
static int cancelled(void *context) {Cancel *c=context;return ++c->calls>=c->stop;}
typedef struct {SourcesConfig *cfg;unsigned calls;} Expiry;
static int expire_during_copy(void *context) {
    Expiry *e=context;if(++e->calls==8)e->cfg->deadline_monotonic=1;return 0;
}
static char *call(const SourcesConfig *cfg,const char *req,int expected) {
    char *body=NULL;size_t n=0;FILE *f=open_memstream(&body,&n);CHECK(f);
    int rc=sources_dispatch(cfg,req,strlen(req),f);CHECK(!fclose(f));
    if(rc!=expected)fprintf(stderr,"audio reply: %s\n",body);
    CHECK(rc==expected);Json j;CHECK(json_parse(&j,body,n));return body;
}
static char *field(const char *body,const char *name) {
    Json j;CHECK(json_parse(&j,body,strlen(body)));
    char *s=json_string(&j,json_get(&j,0,name));CHECK(s);return s;
}
static unsigned cache_count(const SourcesConfig *cfg) {
    char path[600];snprintf(path,sizeof(path),"%s/sources.sqlite3",cfg->state);
    sqlite3 *db=NULL;sqlite3_stmt *stmt=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db,"SELECT count(*) FROM prepared_artifacts",-1,&stmt,NULL)==SQLITE_OK);
    CHECK(sqlite3_step(stmt)==SQLITE_ROW);unsigned count=(unsigned)sqlite3_column_int(stmt,0);
    CHECK(sqlite3_finalize(stmt)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);return count;
}
static size_t cache_bytes(const SourcesConfig *cfg) {
    char path[600];snprintf(path,sizeof(path),"%s/sources.sqlite3",cfg->state);
    sqlite3 *db=NULL;sqlite3_stmt *stmt=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db,"SELECT sum(length(CAST(response AS BLOB))) FROM prepared_artifacts",-1,&stmt,NULL)==SQLITE_OK);
    CHECK(sqlite3_step(stmt)==SQLITE_ROW);size_t bytes=(size_t)sqlite3_column_int64(stmt,0);
    CHECK(sqlite3_finalize(stmt)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);return bytes;
}
static void cached_body(sqlite3 *db,const char *key,const char *body,int corrupt_digest) {
    char digest[65];hash_bytes(body,strlen(body),digest);sqlite3_stmt *stmt=NULL;
    CHECK(sqlite3_prepare_v2(db,"UPDATE prepared_artifacts SET response=?,digest=? WHERE key=?",-1,&stmt,NULL)==SQLITE_OK);
    CHECK(sqlite3_bind_text(stmt,1,body,-1,SQLITE_TRANSIENT)==SQLITE_OK);
    CHECK(sqlite3_bind_text(stmt,2,corrupt_digest?"broken":digest,-1,SQLITE_TRANSIENT)==SQLITE_OK);
    CHECK(sqlite3_bind_text(stmt,3,key,-1,SQLITE_TRANSIENT)==SQLITE_OK);
    CHECK(sqlite3_step(stmt)==SQLITE_DONE&&sqlite3_changes(db)==1);
    CHECK(sqlite3_finalize(stmt)==SQLITE_OK);
}
static unsigned char *decode(const char *s,size_t *length) {
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n=strlen(s);CHECK(n&&!(n%4));
    unsigned char *out=malloc(n/4*3);CHECK(out);*length=0;
    for(size_t i=0;i<n;i+=4) {
        uint32_t value=0;unsigned count=3;
        for(unsigned k=0;k<4;k++) {
            if(s[i+k]=='='){CHECK(i+4==n&&k>=2);value<<=6;count--;}
            else {const char *p=strchr(alphabet,s[i+k]);CHECK(p);value=(value<<6)|(unsigned)(p-alphabet);}
        }
        out[(*length)++]=(unsigned char)(value>>16);
        if(count>1)out[(*length)++]=(unsigned char)(value>>8);
        if(count>2)out[(*length)++]=(unsigned char)value;
    }
    return out;
}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"--fixture")) {
        size_t length;unsigned char *wave=fixture(31000,&length);
        FILE *f=fopen(argv[2],"wb");CHECK(f);
        CHECK(fwrite(wave,1,length,f)==length);CHECK(!fclose(f));free(wave);return 0;
    }
    if(argc!=1)return 2;
    size_t n;unsigned char *bytes=fixture(31000,&n);SourcesWave info;
    CHECK(!sources_wave_inspect(NULL,bytes,n,&info));
    CHECK(info.frames==496000&&info.pcm_bytes==992000);
    CHECK(memcmp(bytes+44,bytes+44+32000,16000));
    unsigned char *window=NULL;size_t window_bytes=0;
    CHECK(!sources_wave_window(NULL,bytes,n,1000,1500,&window,&window_bytes));
    CHECK(window_bytes==16044&&!memcmp(window+44,bytes+44+32000,16000));
    SourcesWave selected;CHECK(!sources_wave_inspect(NULL,window,window_bytes,&selected));
    CHECK(selected.frames==8000);free(window);
    CHECK(!strcmp(sources_wave_window(NULL,bytes,n,0,30001,&window,&window_bytes),"invalid_range"));
    CHECK(!window&&!window_bytes);
    CHECK(!strcmp(sources_wave_window(NULL,bytes,n,1,SIZE_MAX,&window,&window_bytes),"invalid_range"));
    CHECK(!strcmp(sources_wave_inspect(NULL,bytes,n-1,&info),"invalid_audio"));
    bytes[22]=2;CHECK(!strcmp(sources_wave_inspect(NULL,bytes,n,&info),"unsupported_audio_format"));bytes[22]=1;
    put32(bytes+40,UINT32_MAX);CHECK(!strcmp(sources_wave_inspect(NULL,bytes,n,&info),"invalid_audio"));put32(bytes+40,(uint32_t)n-44);
    unsigned char chunks[12+129*8];memset(chunks,0,sizeof(chunks));
    memcpy(chunks,"RIFF",4);put32(chunks+4,sizeof(chunks)-8);memcpy(chunks+8,"WAVE",4);
    for(size_t i=12;i<sizeof(chunks);i+=8)memcpy(chunks+i,"JUNK",4);
    CHECK(!strcmp(sources_wave_inspect(NULL,chunks,sizeof(chunks),&info),"audio_chunk_limit"));
    size_t small_n;unsigned char *small=fixture(1,&small_n);
    unsigned char padded[86];CHECK(small_n==76);
    memcpy(padded,small,small_n);memcpy(padded+76,"JUNK",4);
    put32(padded+80,1);padded[84]=99;padded[85]=0;put32(padded+4,sizeof(padded)-8);
    CHECK(!sources_wave_inspect(NULL,padded,sizeof(padded),&info));
    CHECK(info.frames==16&&!memcmp(info.pcm,small+44,32));
    unsigned char duplicate[100];memcpy(duplicate,small,small_n);
    memcpy(duplicate+76,small+12,24);put32(duplicate+4,sizeof(duplicate)-8);
    CHECK(!strcmp(sources_wave_inspect(NULL,duplicate,sizeof(duplicate),&info),"invalid_audio"));
    unsigned char reordered[76];memcpy(reordered,small,12);
    memcpy(reordered+12,small+36,40);memcpy(reordered+52,small+12,24);
    CHECK(!sources_wave_inspect(NULL,reordered,sizeof(reordered),&info));
    CHECK(!memcmp(info.pcm,small+44,32));free(small);
    Cancel stop={.stop=8};SourcesConfig active={.cancelled=cancelled,.cancellation=&stop};
    CHECK(!strcmp(sources_wave_window(&active,bytes,n,0,25000,&window,&window_bytes),"cancelled"));
    CHECK(stop.calls==8&&!window&&!window_bytes);
    SourcesConfig deadline={0};Expiry expiry={.cfg=&deadline};
    deadline.cancelled=expire_during_copy;deadline.cancellation=&expiry;
    CHECK(!strcmp(sources_wave_window(&deadline,bytes,n,0,25000,&window,&window_bytes),"deadline_exceeded"));
    CHECK(expiry.calls==8&&!window&&!window_bytes);

    char root[]="/tmp/samosa-audio.XXXXXX";CHECK(mkdtemp(root));
    char state[512],path[512],request[1024];
    snprintf(state,sizeof(state),"%s/state",root);snprintf(path,sizeof(path),"%s/audio.wav",root);
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(bytes,1,n,f)==n);CHECK(!fclose(f));
    SourcesConfig cfg={.root=root,.state=state,.owner="alice"};
    char *out=call(&cfg,"{\"contract_version\":1,\"op\":\"register_audio\",\"path\":\"audio.wav\"}",0);
    char *id=field(out,"source_id");CHECK(strstr(out,"audio/wav"));free(out);
    char actual[65];hash_bytes(bytes,n,actual);CHECK(!strcmp(actual,id));
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"inspect_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"operation_id\":\"audio-inspection\",\"timeout_ms\":10000}",id,id);
    char *inspection=call(&cfg,request,0);CHECK(strstr(inspection,"audio_timeline")&&strstr(inspection,"pcm_samples_floor_ms")&&!strstr(inspection,"evidence_id"));
    Json inspected;CHECK(json_parse(&inspected,inspection,strlen(inspection)));size_t duration=0,inspected_frames=0;
    CHECK(json_uint(&inspected,json_get(&inspected,0,"frames"),&inspected_frames)&&inspected_frames==(n-44)/2);
    CHECK(json_uint(&inspected,json_get(&inspected,0,"duration_ms"),&duration)&&duration==inspected_frames/16);
    CHECK(cache_count(&cfg)==0);out=call(&cfg,request,0);CHECK(!strcmp(out,inspection));free(out);free(inspection);
    SourcesConfig other_owner=cfg;other_owner.owner="inspection-other";out=call(&other_owner,request,1);CHECK(strstr(out,"unknown_source"));free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500,\"operation_id\":\"audio-window\",\"parent_operation_id\":\"audio-parent\",\"timeout_ms\":10000}",id,id);
    out=call(&cfg,request,0);
    CHECK(cache_count(&cfg)==1);
    CHECK(strstr(out,"\"kind\":\"derived\"")&&strstr(out,"prepared_audio")&&
          strstr(out,"source_time_ms")&&strstr(out,"media_preparation_only")&&
          !strstr(out,"evidence_id")&&!strstr(out,"\"text\""));
    char *encoded=field(out,"data");size_t decoded_bytes;
    unsigned char *decoded=decode(encoded,&decoded_bytes);
    CHECK(decoded_bytes==16044&&!memcmp(decoded+44,bytes+44+32000,16000));
    CHECK(!sources_wave_inspect(NULL,decoded,decoded_bytes,&info));
    free(decoded);free(encoded);
    CHECK(!unlink(path));SourcesConfig offline=cfg;offline.root=NULL;
    char *again=call(&offline,request,0);CHECK(!strcmp(out,again));free(again);
    char *start_field=strstr(request,"\"start_ms\":1000");CHECK(start_field);
    memcpy(start_field+11,"1001",4);
    again=call(&offline,request,1);CHECK(strstr(again,"idempotency_conflict"));free(again);
    memcpy(start_field+11,"1000",4);
    SourcesConfig foreign=cfg;foreign.owner="bob";
    again=call(&foreign,request,1);CHECK(strstr(again,"unknown_source"));free(again);
    free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":25000,\"operation_id\":\"cancel-audio\"}",id,id);
    stop=(Cancel){.stop=8};active=cfg;active.cancelled=cancelled;active.cancellation=&stop;
    out=call(&active,request,1);CHECK(strstr(out,"cancelled")&&!strstr(out,"artifact_id"));
    CHECK(cache_count(&cfg)==1);
    again=call(&offline,request,1);CHECK(!strcmp(out,again));free(out);free(again);
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"cancel-audio\"}",0);
    CHECK(strstr(out,"\"status\":\"cancelled\""));free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500}",id,id);
    out=call(&offline,request,0);CHECK(strstr(out,"prepared_audio"));free(out);
    stop=(Cancel){.stop=8};active=offline;active.cancelled=cancelled;active.cancellation=&stop;
    out=call(&active,request,0);CHECK(stop.calls<8&&strstr(out,"prepared_audio"));free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":31000,\"operation_id\":\"audio-max\"}",id,id);
    out=call(&offline,request,0);
    CHECK(strlen(out)>1200000&&strlen(out)<2097152);
    encoded=field(out,"data");decoded=decode(encoded,&decoded_bytes);
    CHECK(decoded_bytes==960044&&!memcmp(decoded+44,bytes+44+32000,960000));
    free(encoded);free(decoded);
    again=call(&offline,request,0);CHECK(!strcmp(out,again));free(out);free(again);
    CHECK(cache_count(&cfg)==2);
    char peer_request[1024];
    snprintf(peer_request,sizeof(peer_request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":31000}",id,id);
    stop=(Cancel){.stop=8};active=offline;active.cancelled=cancelled;active.cancellation=&stop;
    out=call(&active,peer_request,1);CHECK(strstr(out,"cancelled")&&!strstr(out,"artifact_id"));free(out);
    CHECK(stop.calls==8&&cache_count(&cfg)==2);
    out=call(&offline,peer_request,0);CHECK(strlen(out)>1200000);free(out);
    char selected_request[1024];snprintf(selected_request,sizeof(selected_request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500}",id,id);
    char *good=call(&offline,selected_request,0);
    SourcesConfig limited=offline;limited.cache_limit=2;
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":500}",id,id);
    out=call(&limited,request,1);CHECK(strstr(out,"cache_limit")&&!strstr(out,"artifact_id"));free(out);
    out=call(&limited,selected_request,0);CHECK(!strcmp(out,good));free(out);
    limited=offline;limited.cache_byte_limit=cache_bytes(&cfg)+1;
    stop=(Cancel){.stop=1000};limited.cancelled=cancelled;limited.cancellation=&stop;
    out=call(&limited,request,1);CHECK(strstr(out,"cache_limit"));free(out);
    CHECK(stop.calls>=8);
    CHECK(cache_count(&cfg)==2);
    out=call(&limited,selected_request,0);CHECK(!strcmp(out,good));free(out);
    snprintf(path,sizeof(path),"%s/note.txt",root);f=fopen(path,"w");CHECK(f);CHECK(fputs("hello",f)>=0);CHECK(!fclose(f));
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"note.txt\"}",0);
    char *text_id=field(out,"source_id");free(out);char text_request[1024];
    snprintf(text_request,sizeof(text_request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":5}",text_id,text_id);
    limited=offline;limited.cache_limit=2;
    out=call(&limited,text_request,1);CHECK(strstr(out,"cache_limit"));free(out);
    limited.cache_limit=3;char *literal=call(&limited,text_request,0);CHECK(strstr(literal,"hello"));
    out=call(&limited,request,1);CHECK(strstr(out,"cache_limit"));free(out);
    out=call(&limited,text_request,0);CHECK(!strcmp(out,literal));free(out);free(literal);free(text_id);
    char database[600];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);
    sqlite3 *db=NULL;CHECK(sqlite3_open(database,&db)==SQLITE_OK);
    sqlite3_stmt *cache_row=NULL;
    CHECK(sqlite3_prepare_v2(db,"SELECT key FROM prepared_artifacts WHERE response=?",-1,&cache_row,NULL)==SQLITE_OK);
    CHECK(sqlite3_bind_text(cache_row,1,good,-1,SQLITE_TRANSIENT)==SQLITE_OK);
    CHECK(sqlite3_step(cache_row)==SQLITE_ROW);char *cache_key=strdup((const char*)sqlite3_column_text(cache_row,0));CHECK(cache_key);
    CHECK(sqlite3_finalize(cache_row)==SQLITE_OK);
    cached_body(db,cache_key,good,1);
    out=call(&offline,selected_request,1);CHECK(strstr(out,"corrupt_cache")&&!strstr(out,"artifact_id"));free(out);
    out=call(&offline,peer_request,0);CHECK(strlen(out)>1200000);free(out);
    cached_body(db,cache_key,good,0);
    char *tampered=strdup(good);CHECK(tampered);char *kind=strstr(tampered,"\"kind\":\"derived\"");CHECK(kind);memcpy(kind+8,"literal",7);
    cached_body(db,cache_key,tampered,0);
    out=call(&offline,selected_request,1);CHECK(strstr(out,"corrupt_cache")&&!strstr(out,"artifact_id"));free(out);free(tampered);
    cached_body(db,cache_key,good,0);free(cache_key);
    out=call(&offline,selected_request,0);CHECK(!strcmp(out,good));free(out);free(good);
    CHECK(sqlite3_exec(db,"UPDATE snapshots SET bytes=zeroblob(length(bytes))",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500}",id,id);
    out=call(&offline,request,1);CHECK(strstr(out,"corrupt_snapshot")&&!strstr(out,"artifact_id"));free(out);
    free(id);free(bytes);
    printf("PASS audio preparation: %u checks; exact selected PCM, absolute time, derived identity, owner/immutable snapshot, durable retry, active cancellation and survivor\n",checks);
    return 0;
}
