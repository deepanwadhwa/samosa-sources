#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#define CHECK(x) assert(x)
#include "docx_fixture.h"
static char *upload(SourcesConfig *cfg,SourcesUploadFormat format,const void *bytes,size_t n,int expected){char *body=NULL;size_t length;FILE *out=open_memstream(&body,&length);assert(out);int result=sources_upload(cfg,format,bytes,n,out);assert(!fclose(out));if(result!=expected)fprintf(stderr,"upload format %d: %s\n",format,body);assert(result==expected);return body;}
static char *read_text(SourcesConfig *cfg,const char *id,int expected){char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":5}",id,id);char *body=NULL;size_t length;FILE *out=open_memstream(&body,&length);assert(out);assert(sources_dispatch(cfg,request,strlen(request),out)==expected);assert(!fclose(out));return body;}
static int cancelled(void *context){(void)context;return 1;}
static void put32(unsigned char *p,uint32_t value){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));}
static char *dispatch_request(SourcesConfig *cfg,const char *text){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);assert(!sources_dispatch(cfg,text,strlen(text),out));assert(!fclose(out));return body;}
static int cancel_hash(void *context){unsigned *ticks=context;return ++*ticks>=2300;}
typedef struct {const char *bytes;size_t length,offset;int mode;} Stream;
static int stream_read(void *context,void *output,size_t capacity,size_t *got){
    Stream *stream=context;assert(capacity&&capacity<=65536);
    if(stream->mode==1)return 1;
    if(stream->mode==2){*got=capacity+1;return 0;}
    *got=stream->offset<stream->length?1:0;
    if(*got){memcpy(output,stream->bytes+stream->offset,*got);stream->offset+=*got;}
    return 0;
}
static char *stream_upload(SourcesConfig *cfg,Stream *stream,size_t declared,int expected){
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);
    assert(sources_upload_stream(cfg,SOURCES_UPLOAD_TEXT,declared,stream_read,stream,out)==expected);assert(!fclose(out));return body;
}
int main(void){
    char state[]="/tmp/samosa-upload-XXXXXX";assert(mkdtemp(state));SourcesConfig cfg={.state=state,.owner="alice",.snapshot_limit=1,.byte_limit=5};
    char bytes[]="hello";char *body=upload(&cfg,SOURCES_UPLOAD_TEXT,bytes,5,0);Json j;assert(json_parse(&j,body,strlen(body)));char *id=json_string(&j,json_get(&j,0,"source_id"));assert(id);free(body);
    memset(bytes,'x',5);body=read_text(&cfg,id,0);assert(strstr(body,"hello"));free(body);
    body=upload(&cfg,SOURCES_UPLOAD_TEXT,"hello",5,0);assert(strstr(body,id));free(body);
    Stream stream={.bytes="hello",.length=5};body=stream_upload(&cfg,&stream,5,0);assert(strstr(body,id)&&stream.offset==5);free(body);
    stream=(Stream){.bytes="hell",.length=4};body=stream_upload(&cfg,&stream,5,1);assert(strstr(body,"upload_length_mismatch"));free(body);
    stream=(Stream){.bytes="hello!",.length=6};body=stream_upload(&cfg,&stream,5,1);assert(strstr(body,"upload_length_mismatch"));free(body);
    stream=(Stream){.mode=1};body=stream_upload(&cfg,&stream,5,1);assert(strstr(body,"upload_read_failed"));free(body);
    stream=(Stream){.mode=2};body=stream_upload(&cfg,&stream,5,1);assert(strstr(body,"invalid_upload_chunk"));free(body);
    SourcesConfig foreign=cfg;foreign.owner="bob";body=read_text(&foreign,id,1);assert(strstr(body,"unknown_source"));free(body);
    body=upload(&foreign,SOURCES_UPLOAD_TEXT,"hello",5,0);assert(strstr(body,id));free(body);body=read_text(&foreign,id,0);assert(strstr(body,"hello"));free(body);
    body=upload(&cfg,SOURCES_UPLOAD_TEXT,"world",5,1);assert(strstr(body,"storage_limit"));free(body);
    body=upload(&cfg,SOURCES_UPLOAD_PDF,"hello",5,1);assert(strstr(body,"unsupported_source"));free(body);
    body=upload(&cfg,SOURCES_UPLOAD_TEXT,"\xff",1,1);assert(strstr(body,"invalid_utf8"));free(body);
    body=upload(&cfg,SOURCES_UPLOAD_TEXT,NULL,1,1);assert(strstr(body,"invalid_request"));free(body);
    body=upload(&cfg,SOURCES_UPLOAD_TEXT,"x",SOURCES_INPUT_MAX+1u,1);assert(strstr(body,"source_too_large"));free(body);
    cfg.cancelled=cancelled;body=upload(&cfg,SOURCES_UPLOAD_TEXT,"hello",5,1);assert(strstr(body,"cancelled"));free(body);free(id);
    char large_state[]="/tmp/samosa-upload-large-XXXXXX";assert(mkdtemp(large_state));
    SourcesConfig large={.state=large_state,.owner="alice",.snapshot_limit=1,.byte_limit=SOURCES_INPUT_MAX};
    unsigned char *input=malloc(SOURCES_INPUT_MAX);assert(input);memset(input,'A',SOURCES_INPUT_MAX);memcpy(input+SOURCES_INPUT_MAX-5,"tail!",5);
    unsigned ticks=0;large.cancelled=cancel_hash;large.cancellation=&ticks;
    body=upload(&large,SOURCES_UPLOAD_TEXT,input,SOURCES_INPUT_MAX,1);assert(strstr(body,"cancelled")&&!strstr(body,"source_id")&&ticks==2300);free(body);
    char database[256];snprintf(database,sizeof(database),"%s/sources.sqlite3",large_state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);sqlite3_stmt *statement=NULL;
    assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots)+(SELECT count(*) FROM source_claims)+(SELECT count(*) FROM formats)",-1,&statement,NULL)==SQLITE_OK);
    assert(sqlite3_step(statement)==SQLITE_ROW&&sqlite3_column_int(statement,0)==0);assert(sqlite3_finalize(statement)==SQLITE_OK);assert(sqlite3_close(db)==SQLITE_OK);
    large.cancelled=NULL;large.cancellation=NULL;body=upload(&large,SOURCES_UPLOAD_TEXT,input,SOURCES_INPUT_MAX,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"source_id"));assert(id);free(body);free(input);
    char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":%u,\"end_byte\":%u}",id,id,SOURCES_INPUT_MAX-5,SOURCES_INPUT_MAX);
    size_t n;FILE *out=open_memstream(&body,&n);assert(out);assert(!sources_dispatch(&large,request,strlen(request),out));assert(!fclose(out));assert(strstr(body,"tail!"));free(body);free(id);
    char media_state[]="/tmp/samosa-upload-readers-XXXXXX";assert(mkdtemp(media_state));SourcesConfig media={.state=media_state,.owner="alice"};
    const char html[]="<!DOCTYPE html><html><body><p>uploaded</p></body></html>";body=upload(&media,SOURCES_UPLOAD_HTML,html,sizeof(html)-1,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"source_id"));assert(id);free(body);
    char selection[512];snprintf(selection,sizeof(selection),"{\"contract_version\":1,\"op\":\"read_html\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":8}",id,id);body=dispatch_request(&media,selection);assert(json_parse(&j,body,strlen(body)));char *literal=json_string(&j,json_get(&j,0,"text"));assert(literal&&!strcmp(literal,"uploaded"));free(literal);free(body);free(id);
    size_t wave_length=32044;unsigned char *wave=calloc(1,wave_length);assert(wave);memcpy(wave,"RIFF",4);put32(wave+4,(uint32_t)wave_length-8);memcpy(wave+8,"WAVEfmt ",8);put32(wave+16,16);wave[20]=1;wave[22]=1;put32(wave+24,16000);put32(wave+28,32000);wave[32]=2;wave[34]=16;memcpy(wave+36,"data",4);put32(wave+40,32000);
    body=upload(&media,SOURCES_UPLOAD_WAV,wave,wave_length,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"source_id"));assert(id);free(body);free(wave);
    snprintf(selection,sizeof(selection),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":1}",id,id);body=dispatch_request(&media,selection);assert(strstr(body,"prepared_audio")&&strstr(body,"media_preparation_only"));assert(json_parse(&j,body,strlen(body)));char *encoded=json_string(&j,json_get(&j,0,"data"));assert(encoded&&strlen(encoded)==104);
    const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";unsigned char decoded[76];size_t used=0;unsigned accumulator=0,bits=0;
    for(const char *p=encoded;*p&&*p!='=';p++){const char *digit=strchr(alphabet,*p);assert(digit);accumulator=(accumulator<<6)|(unsigned)(digit-alphabet);bits+=6;if(bits>=8){bits-=8;assert(used<sizeof(decoded));decoded[used++]=(unsigned char)(accumulator>>bits);}}
    assert(used==76&&!memcmp(decoded,"RIFF",4)&&decoded[4]==68&&!memcmp(decoded+8,"WAVE",4)&&decoded[40]==32);for(size_t i=44;i<76;i++)assert(!decoded[i]);
    char *again=dispatch_request(&media,selection);assert(!strcmp(body,again));free(again);free(encoded);free(body);free(id);
    for(unsigned method=0;method<=8;method+=8){
        size_t document_length;unsigned char *document=docx_package("<w:document xmlns:w='http://schemas.openxmlformats.org/wordprocessingml/2006/main'><w:body><w:p><w:r><w:t>uploaded</w:t></w:r></w:p></w:body></w:document>",NULL,method,&document_length);
        body=upload(&media,SOURCES_UPLOAD_DOCX,document,document_length,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"source_id"));assert(id);free(body);free(document);
        snprintf(selection,sizeof(selection),"{\"contract_version\":1,\"op\":\"read_docx\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":8}",id,id);body=dispatch_request(&media,selection);assert(strstr(body,"uploaded"));again=dispatch_request(&media,selection);assert(!strcmp(body,again));free(body);free(again);free(id);
    }
    puts("PASS immutable upload: root-independent text/HTML/DOCX/WAV evidence and prepared cache, copied bytes, ownership, quotas,64MiB and stream/control refusal");return 0;
}
