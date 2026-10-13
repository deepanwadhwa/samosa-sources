#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "media_video.h"
#include "native.h"
#include "json.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char *public_call(const SourcesConfig *cfg,const char *request,int expected) {
    char *body=NULL;size_t length=0;FILE *stream=open_memstream(&body,&length);assert(stream);
    int rc=sources_dispatch(cfg,request,strlen(request),stream);assert(!fclose(stream));
    if(rc!=expected)fprintf(stderr,"public visual failure: %s\n",body);
    assert(rc==expected);return body;
}
static char *register_public(SourcesConfig *cfg,const char *path,char **root_out) {
    char *state=strdup("/tmp/samosa-visual-corpus-state-XXXXXX");assert(state&&mkdtemp(state));
    char *root=strdup(path);assert(root);char *separator=strrchr(root,'/');assert(separator);
    char *name=separator+1;*separator=0;*root_out=root;
    cfg->root=*root?root:"/";cfg->state=state;cfg->owner="visual-corpus";
    char *request=NULL;size_t request_length=0;
    FILE *stream=open_memstream(&request,&request_length);assert(stream);
    fputs("{\"contract_version\":1,\"op\":\"register_media\",\"path\":",stream);
    json_quote(stream,name,strlen(name));fputc('}',stream);assert(!fclose(stream));
    char *body=public_call(cfg,request,0);free(request);
    Json result;assert(json_parse(&result,body,strlen(body)));
    char *id=json_string(&result,json_get(&result,0,"source_id"));assert(id);free(body);
    return id;
}

static unsigned char *read_bytes(const char *path,size_t limit,size_t *length) {
    FILE *file=fopen(path,"rb");assert(file);
    unsigned char *bytes=malloc(limit+1);assert(bytes);
    *length=fread(bytes,1,limit+1,file);
    assert(!ferror(file)&&*length&&*length<=limit);assert(!fclose(file));
    return bytes;
}
static size_t number(const char *text) {
    char *end;unsigned long value=strtoul(text,&end,10);
    assert(*text&&!*end&&value<=14400000000ULL);return (size_t)value;
}
static void compare_payload(const char *encoded,const unsigned char *reference,size_t length) {
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned value=0,bits=0;size_t written=0;
    for(const char *p=encoded;*p&&*p!='=';p++) {
        const char *digit=strchr(alphabet,*p);assert(digit);
        value=(value<<6)|(unsigned)(digit-alphabet);bits+=6;
        if(bits>=8) {
            bits-=8;assert(written<length&&((value>>bits)&255)==reference[written]);
            written++;
        }
    }
    assert(written==length);
}
static void compare_color(const Json *json,int object,const SourcesMediaFrame *frame) {
    const char *names[]={"source_transfer","source_primaries","output_transfer","output_primaries","tone_mapped"};
    unsigned expected[]={frame->source_transfer,frame->source_primaries,frame->output_transfer,frame->output_primaries,frame->tone_mapped};
    for(size_t i=0;i<5;i++) {
        size_t value;
        assert(json_uint(json,json_get(json,object,names[i]),&value)&&value==expected[i]);
    }
    if(frame->source_transfer==16||frame->source_transfer==18)
        assert(frame->tone_mapped==1&&frame->source_primaries==9&&frame->output_transfer==13&&frame->output_primaries==1);
    else assert(!frame->tone_mapped&&frame->source_transfer==frame->output_transfer&&frame->source_primaries==frame->output_primaries);
}
int main(int argc,char **argv) {
    assert(argc==8||argc==9||argc==10||argc==5);
    size_t input_length,reference_length;
    unsigned char *input=read_bytes(argv[1],SOURCES_INPUT_MAX,&input_length);
    if(argc==5) {
        assert(!strcmp(argv[2],"--refuse"));
        SourcesConfig cfg={.media_helper=argv[4]};SourcesMediaFrame frame={0};
        const char *error=sources_media_video_frame(&cfg,input,input_length,1000,1500,&frame);
        assert(error&&!strcmp(error,argv[3])&&!frame.rgba);
        char fingerprint[65],*body=NULL;size_t length=0;
        assert(!sources_native_media_fingerprint(&cfg,fingerprint));
        error=sources_native_media_frame(&cfg,input,input_length,1000,1500,fingerprint,&body,&length);
        assert(error&&!strcmp(error,argv[3])&&!body&&!length);
        char *root=NULL,*id=register_public(&cfg,argv[1],&root),request[512];
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_visual\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500,\"operation_id\":\"orientation-refusal\"}",id,id);
        body=public_call(&cfg,request,1);Json result;
        assert(json_parse(&result,body,strlen(body)));
        int failure=json_get(&result,0,"error");
        char *code=json_string(&result,json_get(&result,failure,"code"));
        assert(code&&!strcmp(code,argv[3])&&json_get(&result,0,"artifact_id")<0&&json_get(&result,0,"data")<0);
        cfg.media_helper=NULL;cfg.root=NULL;
        char *retry=public_call(&cfg,request,1);assert(!strcmp(body,retry));
        free(code);free(retry);free(body);free(id);free(root);free((void*)cfg.state);free(input);
        puts("PASS visual corpus: typed refusal without artifacts, including public API and exact offline durable retry");
        return 0;
    }
    unsigned char *reference=read_bytes(argv[2],512*512*4,&reference_length);
    size_t width=number(argv[3]),height=number(argv[4]);
    size_t start=number(argv[5]),end=number(argv[6]);
    assert(width&&height&&width<=512&&height<=512&&reference_length==width*height*4);
    SourcesConfig cfg={.media_helper=argv[7]};SourcesMediaFrame frame={0};
    const char *error=sources_media_video_frame(&cfg,input,input_length,start,end,&frame);
    if(error){fprintf(stderr,"corpus primitive: %s\n",error);return 1;}
    if(argc>=9&&frame.timestamp_us!=(long long)number(argv[8])) {
        fprintf(stderr,"timestamp mismatch: %lld expected %s, origin %lld\n",
            frame.timestamp_us,argv[8],frame.origin_us);
        assert(frame.timestamp_us==(long long)number(argv[8]));
    }
    assert(frame.width==width&&frame.height==height&&frame.bytes==reference_length);
    if(argc==10)assert(frame.source_transfer==number(argv[9])&&frame.tone_mapped==1);
    if(memcmp(frame.rgba,reference,reference_length)) {
        size_t different=0;unsigned largest=0;
        for(size_t i=0;i<reference_length;i++) {
            unsigned delta=(unsigned)abs((int)frame.rgba[i]-(int)reference[i]);
            if(delta)different++;
            if(delta>largest)largest=delta;
        }
        fprintf(stderr,"reference mismatch: orientation %u, %zu bytes, maximum delta %u\n",
            frame.orientation,different,largest);
        assert(!memcmp(frame.rgba,reference,reference_length));
    }
    char fingerprint[65],*body=NULL;size_t length=0;
    error=sources_native_media_fingerprint(&cfg,fingerprint);
    if(!error)error=sources_native_media_frame(&cfg,input,input_length,start,end,fingerprint,&body,&length);
    if(error){fprintf(stderr,"corpus worker: %s\n",error);return 1;}
    Json result;assert(json_parse(&result,body,length));size_t actual;
    assert(json_uint(&result,json_get(&result,0,"width"),&actual)&&actual==width);
    assert(json_uint(&result,json_get(&result,0,"height"),&actual)&&actual==height);
    assert(json_uint(&result,json_get(&result,0,"timestamp_us"),&actual)&&actual==(size_t)frame.timestamp_us);
    assert(json_uint(&result,json_get(&result,0,"source_width"),&actual)&&actual==frame.source_width);
    assert(json_uint(&result,json_get(&result,0,"source_height"),&actual)&&actual==frame.source_height);
    assert(json_uint(&result,json_get(&result,0,"orientation"),&actual)&&actual==frame.orientation);
    compare_color(&result,0,&frame);
    char *data=json_string(&result,json_get(&result,0,"data"));assert(data);
    compare_payload(data,reference,reference_length);
    free(data);free(body);
    char *root=NULL,*id=register_public(&cfg,argv[1],&root);
    char prepare[512];
    snprintf(prepare,sizeof(prepare),"{\"contract_version\":1,\"op\":\"prepare_visual\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu}",id,id,start,end);
    body=public_call(&cfg,prepare,0);assert(json_parse(&result,body,strlen(body)));
    int decoder=json_get(&result,0,"decoder");assert(decoder>=0);
    compare_color(&result,decoder,&frame);
    assert(json_uint(&result,json_get(&result,decoder,"width"),&actual)&&actual==width);
    assert(json_uint(&result,json_get(&result,decoder,"height"),&actual)&&actual==height);
    assert(json_uint(&result,json_get(&result,decoder,"orientation"),&actual)&&actual==frame.orientation);
    assert(json_uint(&result,json_get(&result,decoder,"timestamp_us"),&actual)&&actual==(size_t)frame.timestamp_us);
    data=json_string(&result,json_get(&result,0,"data"));assert(data);
    compare_payload(data,reference,reference_length);free(data);
    char *hit=public_call(&cfg,prepare,0);assert(!strcmp(body,hit));free(hit);
    free(id);free(root);free((void*)cfg.state);free(body);free(input);free(reference);sources_media_frame_free(&frame);
    puts("PASS visual corpus: exact reference RGBA, source timestamp and orientation through primitive, worker, public API and cache");
    return 0;
}
