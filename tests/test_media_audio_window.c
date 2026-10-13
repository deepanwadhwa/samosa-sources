#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "media_decode.h"
#include "audio.h"
#include "native.h"
#include "json.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned char *read_bytes(const char *path,size_t limit,size_t *length) {
    FILE *file=fopen(path,"rb");assert(file);unsigned char *bytes=malloc(limit+1);assert(bytes);
    *length=fread(bytes,1,limit+1,file);assert(!ferror(file)&&*length&&*length<=limit);
    assert(!fclose(file));return bytes;
}
static char *call(const SourcesConfig *cfg,const char *request) {
    char *body=NULL;size_t size=0;FILE *out=open_memstream(&body,&size);assert(out);
    int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
    if(rc)fprintf(stderr,"public audio window: %s\n",body);
    assert(!rc);return body;
}
static void compare(const char *encoded,const unsigned char *expected,size_t length) {
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned value=0,bits=0;size_t written=0;
    for(const char *p=encoded;*p&&*p!='=';p++) {
        const char *digit=strchr(alphabet,*p);assert(digit);value=value<<6|(unsigned)(digit-alphabet);bits+=6;
        if(bits>=8){bits-=8;assert(written<length&&((value>>bits)&255)==expected[written]);written++;}
    }
    assert(written==length);
}
int main(int argc,char **argv) {
    assert(argc==6||argc==7);char *tail;size_t start=strtoul(argv[4],&tail,10);assert(!*tail);
    size_t end=strtoul(argv[5],&tail,10);assert(!*tail&&end>start&&end-start<=30000&&end<=14400000);
    size_t length,reference_length;
    unsigned char *input=read_bytes(argv[1],SOURCES_INPUT_MAX,&length);
    unsigned char *reference=read_bytes(argv[2],(end-start)*32,&reference_length);
    assert(reference_length==(end-start)*32);
    SourcesConfig cfg={.media_helper=argv[3]};SourcesMediaAudio audio={0};
    const char *error=sources_media_audio_decode(&cfg,input,length,start,end,&audio);
    if(error)fprintf(stderr,"audio window primitive: %s\n",error);
    assert(!error&&audio.wave_bytes==reference_length+44);
    SourcesWave wave;assert(!sources_wave_inspect(NULL,audio.wave,audio.wave_bytes,&wave));
    if(memcmp(wave.pcm,reference,reference_length)) {
        size_t different=0;for(size_t i=0;i<reference_length;i++)different+=wave.pcm[i]!=reference[i];
        fprintf(stderr,"audio reference mismatch: %zu bytes, rate %u\n",different,audio.source_rate);
        assert(!memcmp(wave.pcm,reference,reference_length));
    }
    char *body=NULL;size_t size=0;char fingerprint[65];
    error=sources_native_media_fingerprint(&cfg,fingerprint);assert(!error);
    error=sources_native_media_audio(&cfg,input,length,start,end,fingerprint,&body,&size);
    if(error)fprintf(stderr,"audio window worker: %s\n",error);
    assert(!error);Json result;assert(json_parse(&result,body,size));
    char *data=json_string(&result,json_get(&result,0,"data"));assert(data);compare(data,audio.wave,audio.wave_bytes);
    free(data);free(body);
    char state[]="/tmp/samosa-audio-window-XXXXXX";assert(mkdtemp(state));
    char *root=strdup(argv[1]);assert(root);char *name=strrchr(root,'/');assert(name);*name++=0;
    cfg.root=*root?root:"/";cfg.state=state;cfg.owner="audio-window";
    char *registration=NULL;size_t request_size=0;FILE *out=open_memstream(&registration,&request_size);assert(out);
    fputs("{\"contract_version\":1,\"op\":\"register_media\",\"path\":",out);json_quote(out,name,strlen(name));fputc('}',out);assert(!fclose(out));
    body=call(&cfg,registration);free(registration);assert(json_parse(&result,body,strlen(body)));
    char *id=json_string(&result,json_get(&result,0,"source_id"));assert(id);free(body);
    char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu,\"operation_id\":\"window\"}",id,id,start,end);
    body=call(&cfg,request);assert(json_parse(&result,body,strlen(body)));
    data=json_string(&result,json_get(&result,0,"data"));assert(data);compare(data,audio.wave,audio.wave_bytes);free(data);
    /* A bare request proves a true prepared-cache hit; tracked retries prove
     * separate durable replay with the original/helper absent. */
    char bare[512];snprintf(bare,sizeof(bare),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu}",id,id,start,end);
    char *hit=call(&cfg,bare);assert(json_parse(&result,hit,strlen(hit)));
    data=json_string(&result,json_get(&result,0,"data"));assert(data);compare(data,audio.wave,audio.wave_bytes);free(data);free(hit);
    cfg.media_helper=NULL;cfg.root=NULL;hit=call(&cfg,request);assert(!strcmp(body,hit));free(hit);free(body);
    if(argc==7) {
        size_t expected=strtoul(argv[6],&tail,10);assert(!*tail&&expected&&expected<=230400000);
        SourcesMediaAudio timeline={0};error=sources_media_audio_probe(&cfg,input,length,&timeline);
        if(error)fprintf(stderr,"audio full probe: %s\n",error);
        assert(!error&&!timeline.wave&&timeline.frames==expected);sources_media_audio_free(&timeline);
        cfg.media_helper=argv[3];
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"inspect_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"operation_id\":\"timeline\"}",id,id);
        body=call(&cfg,request);assert(json_parse(&result,body,strlen(body)));size_t frames;
        assert(json_uint(&result,json_get(&result,0,"frames"),&frames)&&frames==expected);
        cfg.media_helper=NULL;hit=call(&cfg,request);assert(!strcmp(body,hit));free(hit);free(body);
        puts("PASS full audio inspection: exact reference frame count, no retained PCM, public worker and offline durable retry");
    }
    free(root);free(id);free(reference);free(input);sources_media_audio_free(&audio);
    puts("PASS audio window: exact reference PCM through native primitive, worker, public prepare/cache and offline durable replay");
    return 0;
}
