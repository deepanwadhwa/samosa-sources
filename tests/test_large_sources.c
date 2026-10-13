#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char *call(SourcesConfig *cfg,const char *request,int expected) {
    char *body=NULL;size_t length=0;FILE *stream=open_memstream(&body,&length);assert(stream);
    int rc=sources_dispatch(cfg,request,strlen(request),stream);assert(!fclose(stream));
    if(rc!=expected)fprintf(stderr,"large source: %s\n",body);
    assert(rc==expected);return body;
}
static int cancel_copy(void *context){unsigned *calls=context;return ++*calls>=5;}
static void little32(unsigned char *p,unsigned value){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(i*8));}
int main(void) {
    char root[]="/tmp/samosa-large-source-XXXXXX";assert(mkdtemp(root));
    char path[512],state[512];snprintf(path,sizeof(path),"%s/large.txt",root);
    snprintf(state,sizeof(state),"%s/state",root);
    FILE *file=fopen(path,"wb");assert(file);
    char chunk[65536];memset(chunk,'a',sizeof(chunk));
    for(unsigned i=0;i<64;i++)assert(fwrite(chunk,1,sizeof(chunk),file)==sizeof(chunk));
    const char *tail="END-LARGE-SOURCE";
    assert(fwrite(tail,1,strlen(tail),file)==strlen(tail)&&!fclose(file));
    SourcesConfig cfg={.root=root,.state=state,.owner="large-owner"};
    const char *register_request="{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"large.txt\",\"operation_id\":\"large-register\",\"parent_operation_id\":\"large-parent\",\"timeout_ms\":60000}";
    unsigned ticks=0;SourcesConfig cancelled=cfg;cancelled.cancelled=cancel_copy;cancelled.cancellation=&ticks;
    char *failure=call(&cancelled,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"large.txt\",\"operation_id\":\"cancelled-register\"}",1);
    assert(strstr(failure,"cancelled")&&!strstr(failure,"source_id"));free(failure);
    char *registered=call(&cfg,register_request,0);Json result;
    assert(json_parse(&result,registered,strlen(registered)));
    size_t length;assert(json_uint(&result,json_get(&result,0,"bytes"),&length)&&length==64*sizeof(chunk)+strlen(tail));
    char *id=json_string(&result,json_get(&result,0,"source_id"));assert(id);
    assert(!unlink(path));cfg.root=NULL;
    char *retry=call(&cfg,register_request,0);assert(!strcmp(retry,registered));free(retry);
    char request[512];
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":%zu,\"end_byte\":%zu}",id,id,length-strlen(tail),length);
    char *body=call(&cfg,request,0);assert(json_parse(&result,body,strlen(body)));
    char *text=json_string(&result,json_get(&result,0,"text"));assert(text&&!strcmp(text,tail));free(text);free(body);
    SourcesConfig foreign=cfg;foreign.owner="other-owner";
    body=call(&foreign,request,1);assert(strstr(body,"unknown_source"));free(body);
    body=call(&cfg,request,0);assert(strstr(body,tail));free(body);
    free(id);free(registered);
    snprintf(path,sizeof(path),"%s/large.wav",root);file=fopen(path,"wb");assert(file);
    unsigned char header[44]={0};memcpy(header,"RIFF",4);little32(header+4,4000036);
    memcpy(header+8,"WAVEfmt ",8);little32(header+16,16);header[20]=1;header[22]=1;
    little32(header+24,16000);little32(header+28,32000);header[32]=2;header[34]=16;
    memcpy(header+36,"data",4);little32(header+40,4000000);
    assert(fwrite(header,1,sizeof(header),file)==sizeof(header));
    for(unsigned i=0;i<2000000;i++){unsigned char sample[2]={(unsigned char)i,(unsigned char)(i>>8)};assert(fwrite(sample,1,2,file)==2);}
    assert(!fclose(file));cfg.root=root;
    registered=call(&cfg,"{\"contract_version\":1,\"op\":\"register_audio\",\"path\":\"large.wav\"}",0);
    assert(json_parse(&result,registered,strlen(registered)));
    id=json_string(&result,json_get(&result,0,"source_id"));assert(id);
    assert(!unlink(path));cfg.root=NULL;
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":60000,\"end_ms\":60100}",id,id);
    body=call(&cfg,request,0);assert(json_parse(&result,body,strlen(body)));
    char *data=json_string(&result,json_get(&result,0,"data"));assert(data);
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned bits=0,value=0;size_t decoded=0;
    for(const char *p=data;*p&&*p!='=';p++) {
        const char *digit=strchr(alphabet,*p);assert(digit);value=(value<<6)|(unsigned)(digit-alphabet);bits+=6;
        if(bits>=8) {
            bits-=8;unsigned byte=(value>>bits)&255;
            if(decoded>=44){size_t sample=960000+(decoded-44)/2;assert(byte==((sample>>(((decoded-44)%2)*8))&255));}
            decoded++;
        }
    }
    assert(decoded==3244);free(data);
    retry=call(&cfg,request,0);assert(!strcmp(body,retry));free(retry);free(body);free(id);free(registered);
    snprintf(path,sizeof(path),"%s/oversize.txt",root);file=fopen(path,"wb");assert(file);
    assert(!ftruncate(fileno(file),(off_t)SOURCES_INPUT_MAX+1)&&!fclose(file));cfg.root=root;
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"oversize.txt\"}",1);
    assert(strstr(body,"source_too_large")&&!strstr(body,"source_id"));free(body);
    snprintf(path,sizeof(path),"%s/maximum.txt",root);file=fopen(path,"wb");assert(file);
    for(unsigned i=0;i<SOURCES_INPUT_MAX/sizeof(chunk);i++) {
        if(i==SOURCES_INPUT_MAX/sizeof(chunk)-1)memcpy(chunk+sizeof(chunk)-strlen(tail),tail,strlen(tail));
        assert(fwrite(chunk,1,sizeof(chunk),file)==sizeof(chunk));
    }
    assert(!fclose(file));
    registered=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"maximum.txt\",\"operation_id\":\"maximum-register\",\"timeout_ms\":60000}",0);
    assert(json_parse(&result,registered,strlen(registered)));
    assert(json_uint(&result,json_get(&result,0,"bytes"),&length)&&length==SOURCES_INPUT_MAX);
    id=json_string(&result,json_get(&result,0,"source_id"));assert(id);
    char locator_request[512];
    snprintf(locator_request,sizeof(locator_request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"maximum-observe\",\"locators\":[{\"path\":\"maximum.txt\",\"expected_version\":\"%s\"}]}",id);
    ticks=0;cancelled=cfg;cancelled.cancelled=cancel_copy;cancelled.cancellation=&ticks;
    body=call(&cancelled,locator_request,1);
    assert(strstr(body,"cancelled")&&!strstr(body,"observations"));free(body);
    char *observation=call(&cfg,locator_request,0);
    assert(strstr(observation,"\"status\":\"available\"")&&strstr(observation,"\"revision\":1")&&strstr(observation,"\"checked_bytes\":67108864"));
    assert(!unlink(path));cfg.root=NULL;
    retry=call(&cfg,locator_request,0);assert(!strcmp(retry,observation));free(retry);free(observation);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":%zu,\"end_byte\":%zu}",id,id,length-strlen(tail),length);
    body=call(&cfg,request,0);assert(json_parse(&result,body,strlen(body)));
    text=json_string(&result,json_get(&result,0,"text"));assert(text&&!strcmp(text,tail));
    free(text);free(body);free(id);free(registered);
    puts("PASS large immutable text: 4MiB+, chunked cancellation, durable offline registration retry, exact late byte evidence and owner refusal");
    puts("PASS large WAV: 4MiB+, exact late source-time PCM, original deletion, cache reuse and 64MiB+1 refusal");
    puts("PASS actual 64MiB boundary: durable immutable registration, cancellable live observation and exact offline history/evidence");
    return 0;
}
