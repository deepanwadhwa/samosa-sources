#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "ocr.h"
#include "json.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the real reader and its retained cache, not a mocked extractor.
 * Arguments: fixture register-op read-op selector-json native-helper media-helper
 * pdfium tesseract leptonica traineddata. A dash disables an optional artifact. */
static const char *optional(const char *s){return strcmp(s,"-")?s:NULL;}
static char *call(SourcesConfig *cfg,const char *request,int expected){
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);assert(out);
    int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
    if(rc!=expected)fprintf(stderr,"live modality: %s\n",body);
    assert(rc==expected);return body;
}
static void copy(const char *from,const char *to){
    FILE *in=fopen(from,"rb"),*out=fopen(to,"wb");assert(in&&out);
    unsigned char buffer[65536];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),in)))assert(fwrite(buffer,1,n,out)==n);
    assert(!ferror(in));assert(!fclose(in));assert(!fclose(out));
}
typedef struct {const char *path;unsigned calls;int changed;} Mutation;
static void change(Mutation *m){
    FILE *f=fopen(m->path,"r+b");assert(f);int byte=fgetc(f);assert(byte!=EOF);
    assert(!fseek(f,0,SEEK_SET));assert(fputc(byte^1,f)!=EOF);assert(!fclose(f));m->changed=1;
}
static int mutate(void *context){Mutation *m=context;if(++m->calls==4)change(m);return 0;}
static void native_started(void *context,int pid,const char *event,SourcesConfig *cfg){
    (void)pid;(void)cfg;Mutation *m=context;
    if(!m->changed&&!strcmp(event,"started"))change(m);
}
static void refusal(const char *body,const char *code){
    assert(strstr(body,code));assert(!strstr(body,"\"evidence\""));
    assert(!strstr(body,"\"data\""));assert(!strstr(body,"\"text\""));
}
static char *payload(const char *body){
    Json j;assert(json_parse(&j,body,strlen(body)));
    int field=json_get(&j,0,"data");if(field<0)field=json_get(&j,0,"text");
    if(field<0)field=json_get(&j,0,"pages");
    assert(field>=0);
    return strndup(body+j.tokens[field].begin,j.tokens[field].end-j.tokens[field].begin);
}
typedef struct {const unsigned char *bytes;size_t length,offset;} Stream;
static int stream_read(void *context,void *out,size_t capacity,size_t *got){
    Stream *s=context;size_t n=s->length-s->offset;
    if(n>4096)n=4096;
    if(n>capacity)n=capacity;
    memcpy(out,s->bytes+s->offset,n);s->offset+=n;*got=n;return 0;
}
static void upload_modalities(SourcesConfig cfg,const char *fixture,const char *scratch,
    const char *registration,const char *read,const char *version,const char *reference){
    const char *format=!strcmp(registration,"register_audio")?"wav":registration+9;
    SourcesUploadFormat kind=!strcmp(format,"pdf")?SOURCES_UPLOAD_PDF:
        !strcmp(format,"html")?SOURCES_UPLOAD_HTML:!strcmp(format,"docx")?SOURCES_UPLOAD_DOCX:
        !strcmp(format,"image")?SOURCES_UPLOAD_IMAGE:!strcmp(format,"wav")?SOURCES_UPLOAD_WAV:
        !strcmp(format,"media")?SOURCES_UPLOAD_MEDIA:SOURCES_UPLOAD_TEXT;
    FILE *in=fopen(fixture,"rb");assert(in);assert(!fseek(in,0,SEEK_END));
    long total=ftell(in);assert(total>0&&(unsigned long)total<=SOURCES_INPUT_MAX);rewind(in);
    unsigned char *bytes=malloc((size_t)total);assert(bytes);
    assert(fread(bytes,1,(size_t)total,in)==(size_t)total);assert(!fclose(in));
    char state[512],request[12000];cfg.root=NULL;snprintf(state,sizeof(state),"%s/direct",scratch);cfg.state=state;
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);
    assert(!sources_upload(&cfg,kind,bytes,(size_t)total,out));assert(!fclose(out));
    assert(strstr(body,version));free(body);
    body=call(&cfg,read,0);char *expected=payload(reference),*actual=payload(body);
    assert(!strcmp(expected,actual));free(actual);free(body);
    snprintf(state,sizeof(state),"%s/stream",scratch);Stream stream={bytes,(size_t)total,0};
    out=open_memstream(&body,&n);assert(out);
    assert(!sources_upload_stream(&cfg,kind,(size_t)total,stream_read,&stream,out));assert(!fclose(out));
    assert(stream.offset==(size_t)total&&strstr(body,version));free(body);
    body=call(&cfg,read,0);actual=payload(body);assert(!strcmp(expected,actual));free(actual);free(body);
    snprintf(state,sizeof(state),"%s/session",scratch);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"format-lifecycle\",\"format\":\"%s\",\"bytes\":%ld}",format,total);
    body=call(&cfg,request,0);Json j;assert(json_parse(&j,body,strlen(body)));
    char *id=json_string(&j,json_get(&j,0,"upload_id"));assert(id);free(body);
    const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(size_t offset=0;offset<(size_t)total;offset+=8192){
        size_t length=(size_t)total-offset;if(length>8192)length=8192;
        char encoded[10925];size_t used=0;
        for(size_t i=0;i<length;i+=3){
            unsigned value=(unsigned)bytes[offset+i]<<16;
            if(i+1<length)value|=(unsigned)bytes[offset+i+1]<<8;
            if(i+2<length)value|=bytes[offset+i+2];
            encoded[used++]=alphabet[value>>18];encoded[used++]=alphabet[(value>>12)&63];
            encoded[used++]=i+1<length?alphabet[(value>>6)&63]:'=';
            encoded[used++]=i+2<length?alphabet[value&63]:'=';
        }
        encoded[used]=0;
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":%zu,\"data\":\"%s\"}",id,offset,encoded);
        body=call(&cfg,request,0);char *retry=call(&cfg,request,0);assert(!strcmp(body,retry));free(retry);free(body);
    }
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\",\"operation_id\":\"publish-format\"}",id);
    body=call(&cfg,request,0);assert(strstr(body,version));char *retry=call(&cfg,request,0);
    assert(!strcmp(body,retry));free(retry);free(body);
    body=call(&cfg,read,0);actual=payload(body);assert(!strcmp(expected,actual));free(actual);
    char *hit=call(&cfg,read,0);assert(!strcmp(body,hit));free(hit);free(body);
    snprintf(request,sizeof(request),"{\"operation_id\":\"uploaded-read\",%s",read+1);
    body=call(&cfg,request,0);cfg.native_helper=NULL;cfg.media_helper=NULL;
    hit=call(&cfg,request,0);assert(!strcmp(body,hit));
    free(hit);free(body);free(expected);free(id);free(bytes);
    printf("PASS upload %s: fresh direct/stream/session publication, exact chunk/commit retry, real selected payload/cache and offline durable replay\n",format);
}
int main(int argc,char **argv){
    assert(argc==11);
    char scratch[]="/tmp/samosa-live-modality-XXXXXX";assert(mkdtemp(scratch));
    char root[256],state[256],path[512],registration[256],read[2048],request[2600];
    snprintf(root,sizeof(root),"%s/root",scratch);assert(!mkdir(root,0700));
    snprintf(state,sizeof(state),"%s/state",scratch);snprintf(path,sizeof(path),"%s/input",root);
    copy(argv[1],path);
    SourcesOcrConfig ocr={.tesseract_library=optional(argv[8]),.leptonica_library=optional(argv[9]),
        .traineddata_file=optional(argv[10]),.language="eng"};
    SourcesConfig cfg={.root=root,.state=state,.native_helper=optional(argv[5]),
        .media_helper=optional(argv[6]),.pdfium_library=optional(argv[7]),
        .ocr=ocr.traineddata_file?&ocr:NULL};
    snprintf(registration,sizeof(registration),"{\"contract_version\":1,\"op\":\"%s\",\"path\":\"input\"}",argv[2]);
    char *body=call(&cfg,registration,0);Json j;assert(json_parse(&j,body,strlen(body)));
    char *version=json_string(&j,json_get(&j,0,"version"));assert(version);free(body);
    snprintf(read,sizeof(read),"{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",%s}",argv[3],version,version,argv[4]);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_read\",\"path\":\"input\",\"read\":%s}",read);
    /* For native readers, mutate after the real child starts, with a cold cache. */
    int native=!strcmp(argv[3],"read_pdf")||!strcmp(argv[3],"read_pdf_ocr")||
        !strcmp(argv[3],"read_image")||!strcmp(argv[2],"register_media");
    if(native){
        Mutation mutation={.path=path};SourcesConfig racing=cfg;
        racing.native_test_event=native_started;racing.native_test_context=&mutation;
        body=call(&racing,request,1);
        if(!mutation.changed)fprintf(stderr,"no native start: %s\n",body);
        assert(mutation.changed);refusal(body,"source_changed");free(body);
        copy(argv[1],path);
    }
    body=call(&cfg,request,0);assert(strstr(body,"verified_live_read")&&strstr(body,"\"evidence\""));free(body);
    char *retained=call(&cfg,read,0);
    Mutation cached={.path=path};SourcesConfig racing=cfg;
    racing.cancelled=mutate;racing.cancellation=&cached;
    body=call(&racing,request,1);assert(cached.changed);refusal(body,"source_changed");free(body);
    body=call(&cfg,request,1);refusal(body,"live_source_unavailable");free(body);
    copy(argv[1],path);body=call(&cfg,request,0);assert(strstr(body,"verified_live_read"));free(body);
    SourcesConfig foreign=cfg;foreign.owner="foreign";
    body=call(&foreign,request,1);refusal(body,"unknown_source");free(body);
    assert(!unlink(path));body=call(&cfg,request,1);refusal(body,"live_source_unavailable");free(body);
    cfg.root=NULL;body=call(&cfg,read,0);assert(!strcmp(body,retained));free(body);
    upload_modalities(cfg,argv[1],scratch,argv[2],read,version,retained);
    free(retained);free(version);
    printf("PASS live %s: %s warm-cache race, changed/missing/owner refusal and exact retained immutable result\n",argv[3],native?"actual native-child race,":"");
    return 0;
}
