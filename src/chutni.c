#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 8192
#include "chutni.h"
#include "semantic.h"
#include "embedding.h"
#include "transcript.h"
#include "summary.h"
#include "visual.h"
#include "index.h"
#include "samosa_html.h"
#include "samosa_docx.h"
#include "pdf.h"
#include "ocr.h"
#include "native.h"
#include "json.h"
#include "uploads.h"
#include "control.h"
#include "sha256.h"
#include <dlfcn.h>
#include <math.h>
#include <locale.h>
#include <time.h>
static int fail(FILE *out,const char *error){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",error);return 1;}
static int uuid(const char *s){if(!s||strlen(s)!=36)return 0;for(size_t i=0;i<36;i++)if(i==8||i==13||i==18||i==23){if(s[i]!='-')return 0;}else if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static int sha(const char *s){if(!s||strlen(s)!=64)return 0;for(size_t i=0;i<64;i++)if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static int equal_field(const Json *a,int object,const char *key,const char *expected){char *s=json_string(a,json_get(a,object,key));int good=s&&expected&&!strcmp(s,expected);free(s);return good;}
static int string_field(const Json *j,int object,const char *key,size_t maximum,int nonempty){char *s=json_string(j,json_get(j,object,key));int good=s&&strlen(s)<=maximum&&(!nonempty||*s);free(s);return good;}
static int score_valid(const Json *j,int object){int t=json_get(j,object,"score");if(t<0||j->tokens[t].type!=JT_NUMBER)return 0;char number[128];size_t n=j->tokens[t].end-j->tokens[t].begin;if(!n||n>=sizeof(number))return 0;memcpy(number,j->text+j->tokens[t].begin,n);number[n]=0;char *end;double score=strtod(number,&end);return *end==0&&isfinite(score);}
static const char *search_validate(const char *body,size_t n,const char *query,const char *expected_store,int semantic){
    Json j;if(!body||!n||n>65536||!json_parse(&j,body,n)||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count!=3)return "native_helper_protocol";
    char *store=json_string(&j,json_get(&j,0,"store_id")),*returned=NULL;int q=json_get(&j,0,"query"),good=uuid(store)&&(!expected_store||!strcmp(store,expected_store));
    if(semantic)good=good&&q>=0&&j.tokens[q].type==JT_OBJECT&&j.tokens[q].end-j.tokens[q].begin==strlen(query)&&!memcmp(j.text+j.tokens[q].begin,query,strlen(query));
    else {returned=json_string(&j,q);good=good&&returned&&!strcmp(returned,query);}
    free(store);free(returned);double previous=2;
    int items=json_get(&j,0,"items");if(!good||items<0||j.tokens[items].type!=JT_ARRAY||j.tokens[items].count>8)return "native_helper_protocol";
    for(int t=items+1;t<j.tokens[items].next;t=j.tokens[t].next){
        char *source=json_string(&j,json_get(&j,t,"external_source_id")),*artifact=json_string(&j,json_get(&j,t,"external_artifact_id")),*fresh=json_string(&j,json_get(&j,t,"freshness")),*type=json_string(&j,json_get(&j,t,"score_type"));
        good=j.tokens[t].type==JT_OBJECT&&j.tokens[t].count==7&&uuid(source)&&uuid(artifact)&&fresh&&!strcmp(fresh,"current")&&type&&!strcmp(type,semantic?"cosine_bruteforce":"bm25_fts5_negated")&&score_valid(&j,t)&&string_field(&j,t,"artifact_kind",64,1)&&string_field(&j,t,"snippet",4096,0);
        if(good&&semantic){int score_token=json_get(&j,t,"score");char number[128];size_t bytes=j.tokens[score_token].end-j.tokens[score_token].begin;memcpy(number,j.text+j.tokens[score_token].begin,bytes);number[bytes]=0;double score=strtod(number,NULL);good=score>=-1.00001&&score<=1.00001&&score<=previous;previous=score;}
        free(source);free(artifact);free(fresh);free(type);if(!good)return "native_helper_protocol";
    }
    return NULL;
}
const char *sources_chutni_validate(const char *body,size_t n,const char *query){return search_validate(body,n,query,NULL,0);}
const char *sources_chutni_semantic_validate(const char *body,size_t n,const char *arguments,const char *store){
    if(!uuid(store)||sources_semantic_arguments_validate(arguments))return "native_helper_protocol";
    locale_t numeric=newlocale(LC_NUMERIC_MASK,"C",(locale_t)0);if(!numeric)return "resource_limit";locale_t previous=uselocale(numeric);
    const char *error=search_validate(body,n,arguments,store,1);uselocale(previous);freelocale(numeric);return error;
}
const char *sources_chutni_publish_validate(const char *metadata,const char *body,size_t n){
    Json input,result;if(!metadata||!body||n>8192||!json_parse(&input,metadata,strlen(metadata))||!json_parse(&result,body,n)||result.tokens[0].type!=JT_OBJECT||result.tokens[0].count!=9)return "native_helper_protocol";
    int derived=equal_field(&input,json_get(&input,0,"evidence"),"kind","derived");
    const char *keys[]={"store_id","publication_id","version","evidence_id"};int good=1;
    for(size_t i=0;i<4;i++){char *s=json_string(&input,json_get(&input,0,keys[i]));good=good&&(i?sha(s):uuid(s))&&equal_field(&result,0,i==3&&derived?"artifact_id":keys[i],s);free(s);}
    char *source=json_string(&result,json_get(&result,0,"external_source_id")),*artifact=json_string(&result,json_get(&result,0,"external_artifact_id")),*hash=json_string(&result,json_get(&result,0,"source_content_hash")),*status=json_string(&result,json_get(&result,0,"artifact_status"));int recovered=json_get(&result,0,"recovered");
    good=good&&uuid(source)&&uuid(artifact)&&hash&&!strncmp(hash,"blake3:",7)&&sha(hash+7)&&status&&(!strcmp(status,"active")||!strcmp(status,"stale")||!strcmp(status,"superseded"))&&recovered>=0&&result.tokens[recovered].type==JT_BOOL;
    free(source);free(artifact);free(hash);free(status);return good?NULL:"native_helper_protocol";
}
static int read_request(const Json *j,char **store,char **source,char **artifact,size_t *start,size_t *end){
    *store=json_string(j,json_get(j,0,"store_id"));*source=json_string(j,json_get(j,0,"external_source_id"));*artifact=json_string(j,json_get(j,0,"external_artifact_id"));
    *start=0;*end=65536;int a=json_get(j,0,"start_byte"),b=json_get(j,0,"end_byte");
    return uuid(*store)&&uuid(*source)&&uuid(*artifact)&&(a<0||json_uint(j,a,start))&&(b<0||json_uint(j,b,end))&&*start<=*end&&*end<=SOURCES_INPUT_MAX&&*end-*start<=65536;
}
const char *sources_chutni_read_validate(const char *request,const char *body,size_t n){
    Json r,j;char *store=NULL,*source=NULL,*artifact=NULL,*text=NULL;size_t start=0,end=0,a=0,b=0,total=0;int good=request&&body&&n&&n<=262144&&json_parse(&r,request,strlen(request))&&json_parse(&j,body,n);
    if(good)good=read_request(&r,&store,&source,&artifact,&start,&end);
    if(good){int range=json_get(&j,0,"range"),coverage=json_get(&j,0,"coverage"),proof=json_get(&j,0,"provenance");text=json_string(&j,json_get(&j,0,"text"));
        good=j.tokens[0].type==JT_OBJECT&&j.tokens[0].count==14&&equal_field(&j,0,"store_id",store)&&equal_field(&j,0,"external_source_id",source)&&equal_field(&j,0,"external_artifact_id",artifact)&&text&&strlen(text)<=65536&&utf8_valid((const unsigned char *)text,strlen(text))&&range>=0&&j.tokens[range].type==JT_OBJECT&&j.tokens[range].count==3&&json_uint(&j,json_get(&j,range,"start_byte"),&a)&&json_uint(&j,json_get(&j,range,"end_byte"),&b)&&json_uint(&j,json_get(&j,range,"total_bytes"),&total)&&total<=SOURCES_INPUT_MAX&&a==(start<total?start:total)&&a<=b&&b<=total&&b<=end&&b-a==strlen(text)&&((end<total?end:total)-b<=3)&&coverage>=0&&j.tokens[coverage].type==JT_OBJECT&&j.tokens[coverage].count==2&&equal_field(&j,coverage,"status",a==0&&b==total?"complete":"partial")&&equal_field(&j,coverage,"reason","artifact_content")&&proof>=0&&j.tokens[proof].type==JT_OBJECT&&json_get(&j,proof,"producer")>=0&&json_get(&j,proof,"derivation")>=0&&j.tokens[json_get(&j,proof,"producer")].type==JT_OBJECT&&j.tokens[json_get(&j,proof,"derivation")].type==JT_OBJECT&&equal_field(&j,0,"semantic_validation","not_performed")&&string_field(&j,0,"artifact_kind",64,1)&&string_field(&j,0,"artifact_origin",64,1)&&string_field(&j,0,"status",64,1)&&string_field(&j,0,"freshness",64,1)&&json_get(&j,0,"selector")>=0&&json_get(&j,0,"source_content_hash")>=0;
    }
    if(good){char *hash=json_string(&j,json_get(&j,0,"source_content_hash")),*status=json_string(&j,json_get(&j,0,"status"));int selector=json_get(&j,0,"selector");good=hash&&!strncmp(hash,"blake3:",7)&&sha(hash+7)&&status&&(!strcmp(status,"active")||!strcmp(status,"stale")||!strcmp(status,"superseded"))&&selector>=0&&(j.tokens[selector].type==JT_OBJECT||j.tokens[selector].type==JT_NULL);free(hash);free(status);}
    free(store);free(source);free(artifact);free(text);return good?NULL:"native_helper_protocol";
}
static int embedding_request(const Json *j,char **library,char **store,char **id,char **artifact,char **text,char **hash,char **arguments){
    *library=json_string(j,json_get(j,0,"library"));*store=json_string(j,json_get(j,0,"store"));
    *id=json_string(j,json_get(j,0,"store_id"));*artifact=json_string(j,json_get(j,0,"artifact_id"));
    *text=json_string(j,json_get(j,0,"text"));*hash=json_string(j,json_get(j,0,"payload_hash"));
    int a=json_get(j,0,"arguments"),h=json_get(j,0,"payload_hash");
    *arguments=a>=0&&j->tokens[a].type==JT_OBJECT?strndup(j->text+j->tokens[a].begin,j->tokens[a].end-j->tokens[a].begin):NULL;
    return j->tokens[0].type==JT_OBJECT&&j->tokens[0].count==7&&*library&&(*library)[0]=='/'&&strlen(*library)<=4096&&
        *store&&(*store)[0]=='/'&&strlen(*store)<=4096&&uuid(*id)&&uuid(*artifact)&&*text&&**text&&strlen(*text)<=60000&&
        utf8_valid((const unsigned char *)*text,strlen(*text))&&a>=0&&h>=0&&
        (*arguments?(*hash&&!strncmp(*hash,"blake3:",7)&&sha(*hash+7)&&!sources_semantic_arguments_validate(*arguments)):
                    (j->tokens[a].type==JT_NULL&&j->tokens[h].type==JT_NULL));
}
const char *sources_chutni_embedding_validate(const char *metadata,const char *body,size_t n){
    Json m,r;char *library=NULL,*store=NULL,*id=NULL,*artifact=NULL,*text=NULL,*hash=NULL,*arguments=NULL,*actual=NULL,*representation=NULL;
    int good=metadata&&strlen(metadata)<=524288&&body&&n&&n<=65536&&json_parse(&m,metadata,strlen(metadata))&&
        embedding_request(&m,&library,&store,&id,&artifact,&text,&hash,&arguments)&&json_parse(&r,body,n)&&r.tokens[0].type==JT_OBJECT&&r.tokens[0].count==8;
    if(good){char digest[65];hash_bytes(text,strlen(text),digest);size_t bytes=0;
        actual=json_string(&r,json_get(&r,0,"payload_hash"));representation=json_string(&r,json_get(&r,0,"representation_id"));
        int a=json_get(&r,0,"arguments"),recovered=json_get(&r,0,"recovered"),rid=json_get(&r,0,"representation_id");
        good=equal_field(&r,0,"store_id",id)&&equal_field(&r,0,"artifact_id",artifact)&&actual&&!strncmp(actual,"blake3:",7)&&sha(actual+7)&&
            equal_field(&r,0,"text_sha256",digest)&&json_uint(&r,json_get(&r,0,"text_bytes"),&bytes)&&bytes==strlen(text)&&a>=0&&recovered>=0&&rid>=0;
        if(good&&arguments)good=!strcmp(actual,hash)&&uuid(representation)&&r.tokens[recovered].type==JT_BOOL&&r.tokens[a].type==JT_OBJECT&&
            r.tokens[a].end-r.tokens[a].begin==strlen(arguments)&&!memcmp(r.text+r.tokens[a].begin,arguments,strlen(arguments));
        else if(good)good=r.tokens[a].type==JT_NULL&&r.tokens[rid].type==JT_NULL&&r.tokens[recovered].type==JT_NULL;
    }
    free(library);free(store);free(id);free(artifact);free(text);free(hash);free(arguments);free(actual);free(representation);
    return good?NULL:"native_helper_protocol";
}
#ifdef SAMOSA_SOURCES_CHUTNI_WORKER
/* The documented four-function public SDK ABI, not a private storage schema. */
typedef struct ChutniStore ChutniStore;
typedef struct {void *library;int (*open)(const char *,int,ChutniStore **);int (*call)(ChutniStore *,const char *,const char *,char **);void (*free)(void *);void (*close)(ChutniStore *);} Sdk;
static int symbol(void *library,const char *name,void *target,size_t n){void *address=dlsym(library,name);if(!address||n!=sizeof(address))return 0;memcpy(target,&address,n);return 1;}
static const char *status_error(int status){return status==-11?"chutni_busy":status==-4?"chutni_not_found":status==-3?"chutni_version_unsupported":status==-2?"chutni_corrupt_store":"chutni_failed";}
static void copy_field(FILE *out,const Json *j,int item,const char *from,const char *to){int t=json_get(j,item,from);json_quote(out,to,strlen(to));fputc(':',out);if(t<0)fputs("null",out);else fwrite(j->text+j->tokens[t].begin,1,j->tokens[t].end-j->tokens[t].begin,out);}
const char *sources_chutni_worker(const SourcesConfig *cfg,const void *bytes,size_t n,FILE *out){
    Json input;char *path=NULL,*library=NULL,*query=NULL,*expected_store=NULL;const char *error=NULL;
    if(n>32768||!json_parse(&input,bytes,n)||input.tokens[0].type!=JT_OBJECT||(input.tokens[0].count!=3&&input.tokens[0].count!=4))return "invalid_request";
    path=json_string(&input,json_get(&input,0,"store"));library=json_string(&input,json_get(&input,0,"library"));int token=json_get(&input,0,"query"),semantic=token>=0&&input.tokens[token].type==JT_OBJECT;
    if(semantic){query=strndup(input.text+input.tokens[token].begin,input.tokens[token].end-input.tokens[token].begin);expected_store=json_string(&input,json_get(&input,0,"store_id"));}else query=json_string(&input,token);
    if(!path||path[0]!='/'||strlen(path)>4096||!library||library[0]!='/'||strlen(library)>4096||!query||!*query||(semantic?(input.tokens[0].count!=4||!uuid(expected_store)||sources_semantic_arguments_validate(query)!=NULL):(input.tokens[0].count!=3||strlen(query)>512)))error="invalid_request";
    Sdk sdk={0};ChutniStore *store=NULL;char *info=NULL,*response=NULL,*arguments=NULL,*normalized=NULL;size_t length=0;
    if(!error){sdk.library=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!sdk.library||!symbol(sdk.library,"chutni_open",&sdk.open,sizeof(sdk.open))||!symbol(sdk.library,"chutni_call",&sdk.call,sizeof(sdk.call))||!symbol(sdk.library,"chutni_free",&sdk.free,sizeof(sdk.free))||!symbol(sdk.library,"chutni_close",&sdk.close,sizeof(sdk.close)))error="chutni_sdk_unavailable";}
    if(!error){int status=sdk.open(path,1,&store);if(status)error=status_error(status);}
    if(!error)error=sources_interrupted(cfg);
    if(!error){int status=sdk.call(store,"store_info","{}",&info);if(status)error=status_error(status);}
    Json metadata,results;char *id=NULL;
    if(!error&&(!info||strlen(info)>1048576||!json_parse(&metadata,info,strlen(info))||!(id=json_string(&metadata,json_get(&metadata,0,"store_id")))||!uuid(id)))error="chutni_response_invalid";
    if(!error&&semantic&&strcmp(id,expected_store))error="chutni_store_changed";
    if(!error&&semantic){arguments=strdup(query);if(!arguments)error="resource_limit";}
    if(!error&&!semantic){FILE *f=open_memstream(&arguments,&length);if(!f)error="resource_limit";else{fputs("{\"query\":",f);json_quote(f,query,strlen(query));fputs(",\"limit\":8,\"include_stale\":false}",f);if(fclose(f))error="resource_limit";}}
    if(!error){int status=sdk.call(store,semantic?"search_semantic":"search",arguments,&response);if(status)error=status_error(status);}
    int items=-1;if(!error&&(!response||strlen(response)>1048576||!json_parse(&results,response,strlen(response))||(items=json_get(&results,0,"results"))<0||results.tokens[items].type!=JT_ARRAY||results.tokens[items].count>8))error="chutni_response_invalid";
    if(!error){FILE *f=open_memstream(&normalized,&length);if(!f)error="resource_limit";else{
        fputs("{\"store_id\":",f);json_quote(f,id,strlen(id));fputs(",\"query\":",f);if(semantic)fputs(query,f);else json_quote(f,query,strlen(query));fputs(",\"items\":[",f);int first=1;
        const char *from[]={"source_id","artifact_id","artifact_kind","snippet","freshness","score","score_type"};
        const char *to[]={"external_source_id","external_artifact_id","artifact_kind","snippet","freshness","score","score_type"};
        for(int t=items+1;t<results.tokens[items].next;t=results.tokens[t].next){if(!first)fputc(',',f);first=0;fputc('{',f);for(size_t k=0;k<7;k++){if(k)fputc(',',f);copy_field(f,&results,t,from[k],to[k]);}fputc('}',f);}fputs("]}",f);if(fclose(f))error="resource_limit";
    }}
    if(!error&&(semantic?sources_chutni_semantic_validate(normalized,length,query,expected_store):sources_chutni_validate(normalized,length,query)))error="chutni_response_invalid";
    if(!error)error=sources_interrupted(cfg);
    if(!error)fwrite(normalized,1,length,out);
    free(id);free(normalized);free(arguments);if(info&&sdk.free)sdk.free(info);if(response&&sdk.free)sdk.free(response);if(store&&sdk.close)sdk.close(store);if(sdk.library)dlclose(sdk.library);free(path);free(library);free(query);free(expected_store);return error;
}
static const char *sdk_json(Sdk *sdk,ChutniStore *store,const char *operation,const char *arguments,char **body,Json *j){
    *body=NULL;int status=sdk->call(store,operation,arguments,body);if(status)return status_error(status);
    if(!*body||strlen(*body)>1048576||!json_parse(j,*body,strlen(*body)))return "chutni_response_invalid";
    return NULL;
}
const char *sources_chutni_embedding_worker(const SourcesConfig *cfg,const void *input,size_t n,FILE *out){
    Json request,result,query;char *library=NULL,*path=NULL,*id=NULL,*artifact=NULL,*text=NULL,*expected=NULL,*arguments=NULL;
    const char *error=NULL;Sdk sdk={0};ChutniStore *store=NULL;
    char *info=NULL,*record=NULL,*context=NULL,*receipt=NULL,*source=NULL,*payload=NULL,*sdk_args=NULL,*normalized=NULL,*metadata=NULL;
    size_t length=0;
    if(n>524288||!json_parse(&request,input,n)||!embedding_request(&request,&library,&path,&id,&artifact,&text,&expected,&arguments))error="invalid_request";
    if(!error&&!(metadata=strndup(input,n)))error="resource_limit";
    if(!error){sdk.library=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!sdk.library||!symbol(sdk.library,"chutni_open",&sdk.open,sizeof(sdk.open))||!symbol(sdk.library,"chutni_call",&sdk.call,sizeof(sdk.call))||!symbol(sdk.library,"chutni_free",&sdk.free,sizeof(sdk.free))||!symbol(sdk.library,"chutni_close",&sdk.close,sizeof(sdk.close)))error="chutni_sdk_unavailable";}
    if(!error){int status=sdk.open(path,arguments?0:1,&store);if(status)error=status_error(status);}
    if(!error)error=sources_interrupted(cfg);
    if(!error)error=sdk_json(&sdk,store,"store_info","{}",&info,&result);
    if(!error&&!equal_field(&result,0,"store_id",id))error="chutni_store_changed";
    char get[128];if(!error)snprintf(get,sizeof(get),"{\"artifact_id\":\"%s\"}",artifact);
    if(!error)error=sdk_json(&sdk,store,"get_artifact",get,&record,&result);
    if(!error){source=json_string(&result,json_get(&result,0,"source_id"));payload=json_string(&result,json_get(&result,0,"payload_hash"));
        if(!equal_field(&result,0,"artifact_id",artifact)||!uuid(source)||!payload||strncmp(payload,"blake3:",7)||!sha(payload+7))error="chutni_response_invalid";
        else if(expected&&strcmp(expected,payload))error="chutni_source_changed";
    }
    char read[256];if(!error)snprintf(read,sizeof(read),"{\"source_id\":\"%s\",\"artifact_id\":\"%s\",\"include_stale\":true,\"start_byte\":0,\"end_byte\":60000,\"max_text_chars\":60000}",source,artifact);
    if(!error)error=sdk_json(&sdk,store,"source_context",read,&context,&result);
    if(!error){int items=json_get(&result,0,"artifacts"),selected=-1;size_t total=0,a=0,b=0;
        if(items<0||result.tokens[items].type!=JT_ARRAY||result.tokens[items].count!=1)error="chutni_response_invalid";
        else {selected=items+1;char *actual=json_string(&result,json_get(&result,selected,"content"));
            if(!equal_field(&result,selected,"artifact_id",artifact)||!actual||strcmp(actual,text)||
                !json_uint(&result,json_get(&result,selected,"content_total_bytes"),&total)||total!=strlen(text)||
                !json_uint(&result,json_get(&result,selected,"start_byte"),&a)||a||
                !json_uint(&result,json_get(&result,selected,"end_byte"),&b)||b!=total)error="chutni_source_changed";
            free(actual);
        }
    }
    if(!error)error=sources_interrupted(cfg);
    if(!error&&arguments){
        if(!json_parse(&query,arguments,strlen(arguments)))error="invalid_request";
        FILE *f=NULL;if(!error){f=open_memstream(&sdk_args,&length);if(!f)error="resource_limit";else{
            fprintf(f,"{\"artifact_id\":\"%s\",\"expected_payload_hash\":\"%s\",",artifact,payload);
            copy_field(f,&query,0,"profile","profile");fputc(',',f);copy_field(f,&query,0,"vector","vector");fputc('}',f);
            if(fclose(f))error="resource_limit";
        }}
        if(!error)error=sdk_json(&sdk,store,"put_representation_bound",sdk_args,&receipt,&result);
        if(!error){char *rid=json_string(&result,json_get(&result,0,"representation_id"));size_t dimensions=0,expected_dims=0;
            int recovered=json_get(&result,0,"recovered");
            if(!uuid(rid)||!equal_field(&result,0,"artifact_id",artifact)||!equal_field(&result,0,"payload_hash",payload)||
                !json_uint(&result,json_get(&result,0,"dimensions"),&dimensions)||
                !json_uint(&query,json_get(&query,json_get(&query,0,"profile"),"dimensions"),&expected_dims)||dimensions!=expected_dims||
                recovered<0||result.tokens[recovered].type!=JT_BOOL)error="chutni_response_invalid";
            free(rid);
        }
    }
    if(!error){FILE *f=open_memstream(&normalized,&length);if(!f)error="resource_limit";else{
        char digest[65];hash_bytes(text,strlen(text),digest);
        fprintf(f,"{\"store_id\":\"%s\",\"artifact_id\":\"%s\",\"payload_hash\":\"%s\",\"text_sha256\":\"%s\",\"text_bytes\":%zu,\"arguments\":",id,artifact,payload,digest,strlen(text));
        fputs(arguments?arguments:"null",f);fputc(',',f);
        if(arguments){copy_field(f,&result,0,"representation_id","representation_id");fputc(',',f);copy_field(f,&result,0,"recovered","recovered");}
        else fputs("\"representation_id\":null,\"recovered\":null",f);
        fputc('}',f);if(fclose(f))error="resource_limit";
    }}
    if(!error)error=sources_chutni_embedding_validate(metadata,normalized,length);
    if(!error)error=sources_interrupted(cfg);
    if(!error)fwrite(normalized,1,length,out);
    if(info)sdk.free(info);
    if(record)sdk.free(record);
    if(context)sdk.free(context);
    if(receipt)sdk.free(receipt);
    if(store)sdk.close(store);
    if(sdk.library)dlclose(sdk.library);
    free(library);free(path);free(id);free(artifact);free(text);free(expected);free(arguments);free(source);free(payload);free(sdk_args);free(normalized);free(metadata);
    return error;
}
static char *source_arguments(const char *path,int context){char *s=NULL;size_t n;FILE *f=open_memstream(&s,&n);if(!f)return NULL;fputs("{\"source_path\":",f);json_quote(f,path,strlen(path));if(context)fputs(",\"include_stale\":true,\"max_text_chars\":65536",f);fputc('}',f);if(fclose(f)){free(s);return NULL;}return s;}
static const char *publication_refresh(Sdk *sdk,ChutniStore *store,const Json *info,const char *root,const char *path,int (*root_add)(ChutniStore *,const char *,const char *,const void *,char *)){
    int roots=json_get(info,0,"roots");if(roots<0||info->tokens[roots].type!=JT_ARRAY||info->tokens[roots].count>512)return "chutni_response_invalid";
    char root_id[37]={0};size_t best=0;
    for(int t=roots+1;t<info->tokens[roots].next;t=info->tokens[t].next){char *known=json_string(info,json_get(info,t,"path")),*id=json_string(info,json_get(info,t,"root_id"));if(!known||known[0]!='/'||!uuid(id)){free(known);free(id);return "chutni_response_invalid";}size_t n=strlen(known),size=strlen(path);if(n>best&&n<size&&!memcmp(path,known,n)&&(known[n-1]=='/'||path[n]=='/')){memcpy(root_id,id,37);best=n;}free(known);free(id);}
    if(!best){int status=root_add(store,root,NULL,NULL,root_id);if(status)return status_error(status);if(!uuid(root_id))return "chutni_response_invalid";}
    char *arguments=NULL,*response=NULL;size_t n;FILE *f=open_memstream(&arguments,&n);if(!f)return "resource_limit";fprintf(f,"{\"root_id\":\"%s\",\"path\":",root_id);json_quote(f,path,strlen(path));fputc('}',f);if(fclose(f)){free(arguments);return "resource_limit";}Json result;const char *error=sdk_json(sdk,store,"add_source",arguments,&response,&result);if(response)sdk->free(response);free(arguments);return error;
}
static int signed_number(const Json *j,int token,long long *value){
    if(token<0||j->tokens[token].type!=JT_NUMBER)return 0;
    size_t n=j->tokens[token].end-j->tokens[token].begin;
    if(!n||n>20)return 0;
    char number[21];memcpy(number,j->text+j->tokens[token].begin,n);number[n]=0;
    char *tail;errno=0;*value=strtoll(number,&tail,10);return !errno&&!*tail;
}
static int evidence_equal(const Json *a,int x,const Json *b,int y){
    if(x<0||y<0||a->tokens[x].type!=b->tokens[y].type)return 0;
    JsonType type=a->tokens[x].type;
    if(type==JT_STRING){char *left=json_string(a,x),*right=json_string(b,y);int equal=left&&right&&!strcmp(left,right);free(left);free(right);return equal;}
    if(type==JT_NUMBER){size_t left=0,right=0;if(json_uint(a,x,&left)&&json_uint(b,y,&right))return left==right;long long l=0,r=0;return signed_number(a,x,&l)&&signed_number(b,y,&r)&&l==r;}
    if(type==JT_OBJECT){if(a->tokens[x].count!=b->tokens[y].count)return 0;for(int t=x+1;t<a->tokens[x].next;t=a->tokens[a->tokens[t].next].next){char *key=json_string(a,t);int equal=key&&evidence_equal(a,a->tokens[t].next,b,json_get(b,y,key));free(key);if(!equal)return 0;}return 1;}
    if(type==JT_ARRAY){if(a->tokens[x].count!=b->tokens[y].count)return 0;int t=x+1,u=y+1;for(;t<a->tokens[x].next;t=a->tokens[t].next,u=b->tokens[u].next)if(!evidence_equal(a,t,b,u))return 0;return 1;}
    size_t n=a->tokens[x].end-a->tokens[x].begin;return n==b->tokens[y].end-b->tokens[y].begin&&!memcmp(a->text+a->tokens[x].begin,b->text+b->tokens[y].begin,n);
}
/* A committed SDK artifact carries this stable operation and complete Sources
 * parameters. Read it before writing, including stale retained versions. */
static const char *publication_find(const Json *j,const char *operation,const char *hash,const char *version,const char *evidence,const char *digest,const char *text,const char *selector_type,int extracted,size_t start,size_t end,const Json *proof,int proof_token,char **artifact,char **status){
    *artifact=*status=NULL;int items=json_get(j,0,"artifacts");if(items<0||j->tokens[items].type!=JT_ARRAY||j->tokens[items].count>512)return "chutni_response_invalid";
    int summary=equal_field(proof,proof_token,"artifact_type","summary");
    int visual=equal_field(proof,proof_token,"artifact_type","visual_observation");
    int derived=equal_field(proof,proof_token,"kind","derived"),capture_provenance=json_get(proof,proof_token,"provenance");
    for(int t=items+1;t<j->tokens[items].next;t=j->tokens[t].next){
        if(!equal_field(j,t,"operation",operation))continue;
        int provenance=json_get(j,t,"provenance"),producer=json_get(j,provenance,"producer"),derivation=json_get(j,provenance,"derivation"),parameters=json_get(j,derivation,"parameters"),selector=json_get(j,t,"selector");size_t a=0,b=0;
        int truncated=json_get(j,t,"content_truncated");
        if(derived&&(!evidence_equal(j,json_get(j,producer,"model_id"),proof,json_get(proof,capture_provenance,"provider_id"))||!evidence_equal(j,json_get(j,producer,"model_revision"),proof,json_get(proof,capture_provenance,"model_sha256"))||(!summary&&!equal_field(j,selector,"coordinate_space",visual?"source_time_us":"source_time_ms"))))return "chutni_publication_conflict";
        if(!evidence_equal(j,json_get(j,parameters,derived?"sources_capture":"sources_evidence"),proof,proof_token))return "chutni_publication_conflict";
        if(*artifact||!equal_field(j,t,"content",text)||truncated<0||j->tokens[truncated].type!=JT_BOOL||(j->tokens[truncated].end-j->tokens[truncated].begin!=5||memcmp(j->text+j->tokens[truncated].begin,"false",5))||!equal_field(j,t,"artifact_kind",summary?"summary_short":visual?"image_caption":derived?"transcript":"extracted_text")||!equal_field(j,t,"artifact_origin",derived?"model_generated":"deterministic_transform")||!equal_field(j,t,"source_content_hash",hash)||!equal_field(j,producer,"name",summary?"samosa-sources-summary-v1":visual?"samosa-sources-visual-v1":derived?"samosa-sources-transcript-v1":"samosa-sources-text-v1")||!equal_field(j,producer,"producer_kind",derived?"model":"parser")||!equal_field(j,parameters,"source_version",version)||!equal_field(j,parameters,derived?"artifact_id":"evidence_id",evidence)||!equal_field(j,parameters,derived?"capture_digest":"evidence_digest",digest)||!equal_field(j,selector,"type",selector_type)||(extracted&&!equal_field(j,selector,"coordinate_space","extracted_utf8"))||!json_uint(j,json_get(j,selector,"start"),&a)||!json_uint(j,json_get(j,selector,"end"),&b)||a!=start||b!=end)return "chutni_publication_conflict";
        *artifact=json_string(j,json_get(j,t,"artifact_id"));*status=json_string(j,json_get(j,t,"status"));if(!uuid(*artifact)||!*status)return "chutni_response_invalid";
    }
    return NULL;
}
const char *sources_chutni_publish_worker(const SourcesConfig *cfg,const void *input,size_t n,FILE *out){
    if(n<4)return "invalid_request";
    uint32_t length;memcpy(&length,input,4);if(!length||length>1048572||length>n-4||n-4-length>SOURCES_INPUT_MAX)return "invalid_request";
    const unsigned char *snapshot=(const unsigned char *)input+4+length;size_t bytes=n-4-length;
    char *metadata=strndup((const char *)input+4,length);if(!metadata)return "resource_limit";Json j;
    if(!json_parse(&j,metadata,length)||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count!=11){free(metadata);return "invalid_request";}
    char *library=json_string(&j,json_get(&j,0,"library")),*path=json_string(&j,json_get(&j,0,"store")),*store_id=json_string(&j,json_get(&j,0,"store_id")),*root=json_string(&j,json_get(&j,0,"root")),*source_path=json_string(&j,json_get(&j,0,"path")),*key=json_string(&j,json_get(&j,0,"publication_id")),*version=json_string(&j,json_get(&j,0,"version")),*evidence_id=json_string(&j,json_get(&j,0,"evidence_id")),*digest=json_string(&j,json_get(&j,0,"evidence_digest")),*text=json_string(&j,json_get(&j,0,"text"));int evidence=json_get(&j,0,"evidence");const char *error=NULL;
    size_t start=0,end=0,total=0,inspected=0;int range=evidence>=0?json_get(&j,evidence,"range"):-1,coverage=evidence>=0?json_get(&j,evidence,"coverage"):-1;
    char actual[65];hash_bytes(snapshot,bytes,actual);
    if(!library||library[0]!='/'||strlen(library)>4096||!path||path[0]!='/'||strlen(path)>4096||!source_path||source_path[0]!='/'||strlen(source_path)>4096||!uuid(store_id)||!sha(key)||!sha(version)||!sha(evidence_id)||!sha(digest)||!text||strlen(text)>SOURCES_READ_MAX||evidence<0||j.tokens[evidence].type!=JT_OBJECT)error="invalid_request";
    if(!error&&(!root||root[0]!='/'||strlen(root)>4096||strlen(source_path)<=strlen(root)||strncmp(source_path,root,strlen(root))||(root[strlen(root)-1]!='/'&&source_path[strlen(root)]!='/')))error="invalid_request";
    int provenance=evidence>=0?json_get(&j,evidence,"provenance"):-1;
    int summary=equal_field(&j,evidence,"artifact_type","summary")&&equal_field(&j,provenance,"reader","public-inference-summary-v1");
    if(summary)range=json_get(&j,evidence,"input_selector");
    int visual=equal_field(&j,evidence,"artifact_type","visual_observation")&&equal_field(&j,provenance,"reader","public-inference-visual-v1");
    int derived=equal_field(&j,evidence,"kind","derived")&&(summary||visual||equal_field(&j,provenance,"reader","public-inference-transcript-v1"));
    int format=equal_field(&j,provenance,summary?"input_reader":"reader","html-readable-v1")?1:equal_field(&j,provenance,summary?"input_reader":"reader","docx-readable-v1")?2:(equal_field(&j,provenance,summary?"input_reader":"reader","pdfium-text-v1")||equal_field(&j,provenance,summary?"input_reader":"reader","pdfium-index-fallback-v1")||(summary&&equal_field(&j,provenance,"input_reader","pdfium-ocr-v1")))?3:equal_field(&j,provenance,summary?"input_reader":"reader","image-ocr-v1")?4:0;
    const char *selector_type=derived&&!summary?"time_range":format==3?"pages":"byte_range";int extracted=format==1||format==2||format==4;
    if(!error){char proof[65];hash_bytes(j.text+j.tokens[evidence].begin,j.tokens[evidence].end-j.tokens[evidence].begin,proof);if(strcmp(actual,version)||strcmp(proof,digest)||!equal_field(&j,evidence,"source_id",version)||!equal_field(&j,evidence,"version",version)||!equal_field(&j,evidence,derived?"artifact_id":"evidence_id",evidence_id)||!equal_field(&j,evidence,"kind",derived?"derived":"literal"))error="corrupt_index";}
    char *provider=NULL,*model=NULL;
    if(!error&&derived){
        provider=json_string(&j,json_get(&j,provenance,"provider_id"));model=json_string(&j,json_get(&j,provenance,"model_sha256"));char *engine=json_string(&j,json_get(&j,provenance,"engine_sha256")),*capture=strndup(j.text+j.tokens[evidence].begin,j.tokens[evidence].end-j.tokens[evidence].begin);
        SourcesInferenceConfig identity={.provider_id=provider,.engine_sha256=engine,.model_sha256=model};SourcesConfig direct=*cfg;direct.inference=&identity;direct.visual_inference=&identity;direct.summary_inference=&identity;
        error=capture?(summary?sources_summary_capture_validate(&direct,capture,strlen(capture)):visual?sources_visual_capture_validate(&direct,capture,strlen(capture)):sources_transcript_capture_validate(&direct,capture,strlen(capture))):"resource_limit";
        if(error&&!strcmp(error,"corrupt_cache"))error="corrupt_index";
        if(!error&&(!equal_field(&j,evidence,"text",text)||!json_uint(&j,json_get(&j,range,summary?(format==3?"start_page":"start_byte"):"start_ms"),&start)||!json_uint(&j,json_get(&j,range,summary?(format==3?"end_page":"end_byte"):"end_ms"),&end)))error="corrupt_index";
        if(!error&&visual){int frame=json_get(&j,evidence,"frame");if(!json_uint(&j,json_get(&j,frame,"timestamp_us"),&start)||start>=14400000000ULL)error="corrupt_index";else end=start+1;}
        free(engine);free(capture);
    }else if(!error&&format==3){
        if(!json_uint(&j,json_get(&j,range,"start_page"),&start)||!json_uint(&j,json_get(&j,range,"end_page"),&end))error="corrupt_index";
        char *locator=json_string(&j,json_get(&j,provenance,"locator")),*regenerated=NULL;size_t size=0;FILE *f=NULL;
        if(!error){f=open_memstream(&regenerated,&size);if(!f)error="resource_limit";else{SourcesConfig direct=*cfg;direct.native_helper=NULL;if(equal_field(&j,provenance,"reader","pdfium-index-fallback-v1")){char *failure=NULL,*combined=source_index_pdf_regenerate(&direct,snapshot,bytes,version,start,end,locator?locator:"",&failure);if(failure){error=sources_native_error_code(failure);free(failure);}else if(combined)fputs(combined,f);else error="resource_limit";free(combined);}else error=pdf_extract(&direct,cfg->pdfium_library,snapshot,bytes,version,version,start,end,locator?locator:"",f);if(fclose(f))error="resource_limit";}}
        if(!error){Json proof;char *decoded=source_index_literal(regenerated,size);if(!decoded||strcmp(decoded,text)||!json_parse(&proof,regenerated,size)||proof.tokens[0].end!=j.tokens[evidence].end-j.tokens[evidence].begin||memcmp(regenerated,j.text+j.tokens[evidence].begin,proof.tokens[0].end))error="corrupt_index";free(decoded);}
        free(locator);free(regenerated);
    }else if(!error){
        SamosaHtmlResult html={0};SamosaDocxResult docx={0};SourcesOcrResult image={0};const unsigned char *readable=snapshot;size_t readable_bytes=bytes;char fingerprint[65];
        if(format==1){if(!samosa_html_extract(snapshot,bytes,SOURCES_READ_MAX,&html,&error)&&!error)error="corrupt_index";readable=(const unsigned char*)html.text;readable_bytes=html.text_bytes;sources_html_fingerprint(fingerprint);}
        else if(format==2){if(!samosa_docx_extract(snapshot,bytes,SOURCES_READ_MAX,&docx,&error)&&!error)error="corrupt_index";readable=(const unsigned char*)docx.text;readable_bytes=docx.text_bytes;sources_docx_fingerprint(fingerprint);}
        else if(format==4){SourcesConfig direct=*cfg;direct.native_helper=NULL;error=sources_ocr_extract(&direct,snapshot,bytes,&image);readable=(const unsigned char*)image.text;readable_bytes=image.text_bytes;memcpy(fingerprint,image.fingerprint,65);}
        if(!error&&(!utf8_valid(readable,readable_bytes)||!equal_field(&j,evidence,"text",text)||!json_uint(&j,json_get(&j,range,"start_byte"),&start)||!json_uint(&j,json_get(&j,range,"end_byte"),&end)||start>end||end>readable_bytes||end-start!=strlen(text)||(end>start&&memcmp(readable+start,text,end-start))||!json_uint(&j,json_get(&j,coverage,"total_bytes"),&total)||total!=readable_bytes||!json_uint(&j,json_get(&j,coverage,"inspected_bytes"),&inspected)||inspected!=end-start||!equal_field(&j,coverage,"status",format?"partial":start==0&&end==bytes?"complete":"partial")))error="corrupt_index";
        if(!error&&format){char material[256],id[65];int n=snprintf(material,sizeof(material),"%s:%s:%s:%zu:%zu",format==1?"html-readable-v1":format==2?"docx-readable-v1":"image-ocr-v1",fingerprint,version,start,end);hash_bytes(material,(size_t)n,id);if(!equal_field(&j,range,"coordinate_space","extracted_utf8")||!equal_field(&j,provenance,"extractor_fingerprint",fingerprint)||strcmp(id,evidence_id)||!equal_field(&j,coverage,"reason",format==1?"static_readable_text_only":format==2?"ooxml_readable_text_only":"ocr_recognition_uncertain"))error="corrupt_index";}
        else if(!error&&!equal_field(&j,provenance,"reader","text-utf8-v1"))error="corrupt_index";
        if(!error&&format==4){int recognition=json_get(&j,provenance,"recognition"),raster=json_get(&j,recognition,"raster");size_t confidence=0,width=0,height=0;if(!equal_field(&j,recognition,"method","tesseract")||!json_uint(&j,json_get(&j,recognition,"confidence"),&confidence)||confidence!=(size_t)image.confidence||!json_uint(&j,json_get(&j,raster,"width"),&width)||width!=image.width||!json_uint(&j,json_get(&j,raster,"height"),&height)||height!=image.height||!equal_field(&j,raster,"coordinate_space","original_pixels"))error="corrupt_index";}
        samosa_html_result_free(&html);samosa_docx_result_free(&docx);sources_ocr_free(&image);
    }
    Sdk sdk={0};ChutniStore *store=NULL;int (*hash_bytes_sdk)(const void *,size_t,char *)=NULL;int (*hash_file_sdk)(const char *,char *)=NULL;int (*root_add_sdk)(ChutniStore *,const char *,const char *,const void *,char *)=NULL;int (*source_find_sdk)(ChutniStore *,const char *,char *)=NULL;char expected[72]={0};
    if(!error){sdk.library=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!sdk.library||!symbol(sdk.library,"chutni_open",&sdk.open,sizeof(sdk.open))||!symbol(sdk.library,"chutni_call",&sdk.call,sizeof(sdk.call))||!symbol(sdk.library,"chutni_free",&sdk.free,sizeof(sdk.free))||!symbol(sdk.library,"chutni_close",&sdk.close,sizeof(sdk.close))||!symbol(sdk.library,"chutni_hash_bytes",&hash_bytes_sdk,sizeof(hash_bytes_sdk))||!symbol(sdk.library,"chutni_hash_file",&hash_file_sdk,sizeof(hash_file_sdk))||!symbol(sdk.library,"chutni_root_add",&root_add_sdk,sizeof(root_add_sdk))||!symbol(sdk.library,"chutni_source_find",&source_find_sdk,sizeof(source_find_sdk)))error="chutni_sdk_unavailable";}
    if(!error){int status=sdk.open(path,1,&store);if(status)error=status_error(status);}
    if(!error&&(hash_bytes_sdk(snapshot,bytes,expected)||strncmp(expected,"blake3:",7)||!sha(expected+7)))error="chutni_response_invalid";
    char *info=NULL,*source=NULL,*context=NULL,*written=NULL,*arguments=NULL,*submission=NULL,*artifact=NULL,*artifact_status=NULL,*source_id=NULL;Json info_json,source_json,context_json,written_json;int recovered=0;
    if(!error)error=sources_interrupted(cfg);
    if(!error)error=sdk_json(&sdk,store,"store_info","{}",&info,&info_json);
    if(!error&&!equal_field(&info_json,0,"store_id",store_id))error="chutni_store_changed";
    if(!error){sdk.close(store);store=NULL;int status=sdk.open(path,0,&store);if(status)error=status_error(status);}
    if(!error){sdk.free(info);info=NULL;error=sdk_json(&sdk,store,"store_info","{}",&info,&info_json);}
    if(!error&&!equal_field(&info_json,0,"store_id",store_id))error="chutni_store_changed";
    if(!error){char known[37];int status=source_find_sdk(store,source_path,known);if(status==-4){char observed[72];if(hash_file_sdk(source_path,observed)||strcmp(observed,expected))error="chutni_source_changed";if(!error)error=publication_refresh(&sdk,store,&info_json,root,source_path,root_add_sdk);}else if(status)error=status_error(status);}
    if(!error){arguments=source_arguments(source_path,0);if(!arguments)error="resource_limit";}
    if(!error)error=sdk_json(&sdk,store,"get_source",arguments,&source,&source_json);
    if(!error){source_id=json_string(&source_json,json_get(&source_json,0,"source_id"));if(!uuid(source_id))error="chutni_response_invalid";}
    char operation[96];snprintf(operation,sizeof(operation),"%s:%s",summary?"sources-summary-v1":visual?"sources-visual-v1":derived?"sources-transcript-v1":"sources-index-v1",key?key:"");
    if(!error){free(arguments);arguments=source_arguments(source_path,1);if(!arguments)error="resource_limit";}
    if(!error)error=sdk_json(&sdk,store,"source_context",arguments,&context,&context_json);
    if(!error)error=publication_find(&context_json,operation,expected,version,evidence_id,digest,text,selector_type,extracted,start,end,&j,evidence,&artifact,&artifact_status);
    if(!error&&artifact)recovered=1;
    if(!error&&!artifact){
        if(!equal_field(&source_json,0,"content_hash",expected)){
            char observed[72];if(hash_file_sdk(source_path,observed)||strcmp(observed,expected))error="chutni_source_changed";
            if(!error)error=publication_refresh(&sdk,store,&info_json,root,source_path,root_add_sdk);
            if(!error){free(arguments);arguments=source_arguments(source_path,0);if(!arguments)error="resource_limit";}
            if(!error){sdk.free(source);source=NULL;error=sdk_json(&sdk,store,"get_source",arguments,&source,&source_json);}
            if(!error){free(source_id);source_id=json_string(&source_json,json_get(&source_json,0,"source_id"));if(!uuid(source_id))error="chutni_response_invalid";}
            if(!error){free(arguments);arguments=source_arguments(source_path,1);if(!arguments)error="resource_limit";}
        }
        if(!error&&!equal_field(&source_json,0,"content_hash",expected))error="chutni_source_changed";
        FILE *f=NULL;size_t size=0;if(!error){f=open_memstream(&submission,&size);if(!f)error="resource_limit";}
        if(!error&&derived){
            fputs("{\"confirmed\":true,\"source_path\":",f);json_quote(f,source_path,strlen(source_path));fputs(",\"source_content_hash\":",f);json_quote(f,expected,strlen(expected));
            fprintf(f,",\"outputs\":[{\"artifact_kind\":\"%s\",\"producer_name\":\"%s\",\"model_id\":",summary?"summary_short":visual?"image_caption":"transcript",summary?"samosa-sources-summary-v1":visual?"samosa-sources-visual-v1":"samosa-sources-transcript-v1");
            json_quote(f,provider,strlen(provider));fputs(",\"model_revision\":",f);json_quote(f,model,strlen(model));
            fputs(",\"app_name\":\"samosa-sources\",\"app_version\":\"" SOURCES_VERSION "\",\"runtime\":\"Inference\",\"operation\":",f);json_quote(f,operation,strlen(operation));
            fputs(",\"recipe_hash\":",f);json_quote(f,digest,strlen(digest));fputs(",\"text\":",f);json_quote(f,text,strlen(text));
if(summary)fprintf(f,",\"selector\":{\"type\":\"%s\",\"start\":%zu,\"end\":%zu%s},\"parameters\":{\"source_version\":",selector_type,start,end,extracted?",\"coordinate_space\":\"extracted_utf8\"":"");
else fprintf(f,",\"selector\":{\"type\":\"time_range\",\"start\":%zu,\"end\":%zu,\"coordinate_space\":\"%s\"},\"parameters\":{\"source_version\":",start,end,visual?"source_time_us":"source_time_ms");
json_quote(f,version,strlen(version));fputs(",\"artifact_id\":",f);json_quote(f,evidence_id,strlen(evidence_id));fputs(",\"capture_digest\":",f);json_quote(f,digest,strlen(digest));fputs(",\"sources_capture\":",f);fwrite(j.text+j.tokens[evidence].begin,1,j.tokens[evidence].end-j.tokens[evidence].begin,f);fputs("}}]}",f);if(fclose(f))error="resource_limit";
        }else
        if(!error){fputs("{\"confirmed\":true,\"source_path\":",f);json_quote(f,source_path,strlen(source_path));fputs(",\"source_content_hash\":",f);json_quote(f,expected,strlen(expected));fputs(",\"outputs\":[{\"artifact_kind\":\"extracted_text\",\"producer_name\":\"samosa-sources-text-v1\",\"producer_version\":\"" SOURCES_VERSION "\",\"app_name\":\"samosa-sources\",\"app_version\":\"" SOURCES_VERSION "\",\"runtime\":\"Sources\",\"operation\":",f);json_quote(f,operation,strlen(operation));fputs(",\"recipe_hash\":",f);json_quote(f,digest,strlen(digest));fputs(",\"text\":",f);json_quote(f,text,strlen(text));fprintf(f,",\"selector\":{\"type\":\"%s\",\"start\":%zu,\"end\":%zu%s},\"parameters\":{\"source_version\":",selector_type,start,end,extracted?",\"coordinate_space\":\"extracted_utf8\"":"");json_quote(f,version,strlen(version));fputs(",\"evidence_id\":",f);json_quote(f,evidence_id,strlen(evidence_id));fputs(",\"evidence_digest\":",f);json_quote(f,digest,strlen(digest));fputs(",\"sources_evidence\":",f);fwrite(j.text+j.tokens[evidence].begin,1,j.tokens[evidence].end-j.tokens[evidence].begin,f);fputs("}}]}",f);if(fclose(f))error="resource_limit";}
        if(!error)error=sources_interrupted(cfg);
        if(!error){int status=sdk.call(store,"put_file_outputs",submission,&written);if(status)error=status==-8?"chutni_source_changed":status_error(status);else if(!written||strlen(written)>8192||!json_parse(&written_json,written,strlen(written)))error="chutni_response_invalid";}
        if(!error){sdk.free(context);context=NULL;error=sdk_json(&sdk,store,"source_context",arguments,&context,&context_json);}
        if(!error)error=publication_find(&context_json,operation,expected,version,evidence_id,digest,text,selector_type,extracted,start,end,&j,evidence,&artifact,&artifact_status);
        if(!error&&!artifact)error="chutni_response_invalid";
    }
    if(!error)error=sources_interrupted(cfg);
    if(!error){fputs("{\"store_id\":",out);json_quote(out,store_id,strlen(store_id));fputs(",\"external_source_id\":",out);json_quote(out,source_id,strlen(source_id));fputs(",\"external_artifact_id\":",out);json_quote(out,artifact,strlen(artifact));fputs(",\"publication_id\":",out);json_quote(out,key,strlen(key));fputs(",\"version\":",out);json_quote(out,version,strlen(version));fputs(derived?",\"artifact_id\":" : ",\"evidence_id\":",out);json_quote(out,evidence_id,strlen(evidence_id));fputs(",\"source_content_hash\":",out);json_quote(out,expected,strlen(expected));fputs(",\"artifact_status\":",out);json_quote(out,artifact_status,strlen(artifact_status));fprintf(out,",\"recovered\":%s}",recovered?"true":"false");}
    free(source_id);free(artifact);free(artifact_status);free(arguments);free(submission);if(sdk.free){if(info)sdk.free(info);if(source)sdk.free(source);if(context)sdk.free(context);if(written)sdk.free(written);}if(store&&sdk.close)sdk.close(store);if(sdk.library)dlclose(sdk.library);
    free(library);free(path);free(store_id);free(root);free(source_path);free(key);free(version);free(evidence_id);free(digest);free(text);free(provider);free(model);free(metadata);return error;
}
const char *sources_chutni_read_worker(const SourcesConfig *cfg,const void *input,size_t n,FILE *out){
    Json j,r;if(n>16384||!json_parse(&j,input,n)||j.tokens[0].count!=3)return "invalid_request";
    char *library=json_string(&j,json_get(&j,0,"library")),*path=json_string(&j,json_get(&j,0,"store")),*request=NULL,*expected=NULL,*source=NULL,*artifact=NULL,*info=NULL,*context=NULL,*arguments=NULL,*normalized=NULL;size_t start=0,end=0,length=0;const char *error=NULL;Sdk sdk={0};ChutniStore *store=NULL;
    int t=json_get(&j,0,"request");if(t>=0)request=strndup(j.text+j.tokens[t].begin,j.tokens[t].end-j.tokens[t].begin);
    if(!library||library[0]!='/'||strlen(library)>4096||!path||path[0]!='/'||strlen(path)>4096||!request||!json_parse(&r,request,strlen(request))||!read_request(&r,&expected,&source,&artifact,&start,&end))error="invalid_request";
    if(!error){sdk.library=dlopen(library,RTLD_NOW|RTLD_LOCAL);if(!sdk.library||!symbol(sdk.library,"chutni_open",&sdk.open,sizeof(sdk.open))||!symbol(sdk.library,"chutni_call",&sdk.call,sizeof(sdk.call))||!symbol(sdk.library,"chutni_free",&sdk.free,sizeof(sdk.free))||!symbol(sdk.library,"chutni_close",&sdk.close,sizeof(sdk.close)))error="chutni_sdk_unavailable";}
    if(!error){int status=sdk.open(path,1,&store);if(status)error=status_error(status);}
    Json metadata,result;if(!error)error=sources_interrupted(cfg);
    if(!error)error=sdk_json(&sdk,store,"store_info","{}",&info,&metadata);
    if(!error&&!equal_field(&metadata,0,"store_id",expected))error="chutni_store_changed";
    FILE *f=NULL;if(!error){f=open_memstream(&arguments,&length);if(!f)error="resource_limit";else{fprintf(f,"{\"source_id\":\"%s\",\"artifact_id\":\"%s\",\"include_stale\":true,\"start_byte\":%zu,\"end_byte\":%zu}",source,artifact,start,end);if(fclose(f))error="resource_limit";}}
    if(!error)error=sdk_json(&sdk,store,"source_context",arguments,&context,&result);
    int selected=-1;if(!error){int source_object=json_get(&result,0,"source"),items=json_get(&result,0,"artifacts");if(!equal_field(&result,source_object,"source_id",source)||items<0||result.tokens[items].type!=JT_ARRAY)error="chutni_response_invalid";else for(int i=items+1;i<result.tokens[items].next;i=result.tokens[i].next)if(equal_field(&result,i,"artifact_id",artifact)){if(selected>=0){error="chutni_response_invalid";break;}selected=i;}if(!error&&selected<0)error="chutni_not_found";}
    if(!error){size_t a=0,b=0,total=0;if(!json_uint(&result,json_get(&result,selected,"start_byte"),&a)||!json_uint(&result,json_get(&result,selected,"end_byte"),&b)||!json_uint(&result,json_get(&result,selected,"content_total_bytes"),&total))error="chutni_response_invalid";else{f=open_memstream(&normalized,&length);if(!f)error="resource_limit";else{fprintf(f,"{\"store_id\":\"%s\",\"external_source_id\":\"%s\",\"external_artifact_id\":\"%s\",",expected,source,artifact);const char *from[]={"artifact_kind","artifact_origin","status","freshness","source_content_hash","selector","provenance","semantic_validation","content"};const char *to[]={"artifact_kind","artifact_origin","status","freshness","source_content_hash","selector","provenance","semantic_validation","text"};for(size_t i=0;i<9;i++){if(i)fputc(',',f);copy_field(f,&result,selected,from[i],to[i]);}fprintf(f,",\"range\":{\"start_byte\":%zu,\"end_byte\":%zu,\"total_bytes\":%zu},\"coverage\":{\"status\":\"%s\",\"reason\":\"artifact_content\"}}",a,b,total,a==0&&b==total?"complete":"partial");if(fclose(f))error="resource_limit";}}}
    if(!error&&sources_chutni_read_validate(request,normalized,length))error="chutni_response_invalid";
    if(!error)error=sources_interrupted(cfg);
    if(!error)fwrite(normalized,1,length,out);
    if(context)sdk.free(context);
    if(info)sdk.free(info);
    if(store)sdk.close(store);
    if(sdk.library)dlclose(sdk.library);
    free(library);free(path);free(request);free(expected);free(source);free(artifact);free(arguments);free(normalized);return error;
}

#endif
int source_chutni_dispatch(const SourcesConfig *cfg,const char *input,size_t length,sqlite3 *db,FILE *out){
    Json request;char *query=NULL;if(!json_parse(&request,input,length))return fail(out,"invalid_request");
    int reading=equal_field(&request,0,"op","memory_read"),text_query=equal_field(&request,0,"op","memory_search_text"),semantic=text_query||equal_field(&request,0,"op","memory_search_semantic");
    char *embedding=NULL,*query_text=NULL;size_t embedding_bytes=0;
    if(text_query){query_text=json_string(&request,json_get(&request,0,"query"));const char *error=sources_embedding_search_arguments(cfg,query_text,&query,&embedding,&embedding_bytes);if(error){free(query_text);return fail(out,error);}}
    if(semantic&&!text_query){const char *error=sources_semantic_arguments(cfg->chutni_semantic_profile,&request,json_get(&request,0,"vector"),&query);if(error)return fail(out,error);}
    if(!reading&&!semantic&&(!(query=json_string(&request,json_get(&request,0,"query")))||!*query||strlen(query)>512)){free(query);return fail(out,"invalid_request");}
    if(reading){char *a=NULL,*b=NULL,*c=NULL;size_t start,end;int good=read_request(&request,&a,&b,&c,&start,&end);free(a);free(b);free(c);if(!good)return fail(out,"invalid_request");}
    char *body=NULL;size_t n=0;const char *error=reading?sources_native_chutni_read(cfg,input,&body,&n):semantic?sources_native_chutni_semantic(cfg,query,&body,&n):sources_native_chutni(cfg,query,&body,&n);free(query);
    char *payload=NULL;size_t size=0;FILE *f=NULL;
    if(!error){f=open_memstream(&payload,&size);if(!f)error="resource_limit";else{struct timespec time;if(clock_gettime(CLOCK_REALTIME,&time)||time.tv_sec<0)error="state_unavailable";else{fprintf(f,"{\"provider\":\"chutni\",\"observed_at_unix_ms\":%llu,\"coverage\":{\"status\":\"partial\",\"reason\":\"%s\"},\"result\":",(unsigned long long)time.tv_sec*1000+(unsigned long long)time.tv_nsec/1000000,reading?"artifact_content":"retrieval_metadata");fwrite(body,1,n,f);if(text_query){fputs(",\"text_query\":",f);json_quote(f,query_text,strlen(query_text));fputs(",\"query_embedding\":",f);fwrite(embedding,1,embedding_bytes,f);}fputc('}',f);}if(fclose(f))error="resource_limit";}}free(body);free(embedding);free(query_text);
    int transaction=0;char *admission=NULL,*response=NULL;size_t admitted=0,response_length=0;
    if(!error){if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)error="store_busy";else transaction=1;}
    if(!error){f=open_memstream(&admission,&admitted);if(!f)error="resource_limit";else{int failed=source_snapshot_accept(cfg,db,f,(const unsigned char *)payload,size,"chutni:retrieval",0);if(fclose(f))error="resource_limit";else if(failed){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);fputs(admission,out);free(admission);free(payload);return 1;}}}
    if(!error){Json a;if(!json_parse(&a,admission,admitted))error="invalid_response";else{char digest[65],evidence[65],material[96];hash_bytes(payload,size,digest);snprintf(material,sizeof(material),"chutni-retrieval-v1:%s",digest);hash_bytes(material,strlen(material),evidence);f=open_memstream(&response,&response_length);if(!f)error="resource_limit";else{fwrite(admission,1,a.tokens[0].end-1,f);fprintf(f,",\"evidence_id\":\"%s\",\"memory\":",evidence);fwrite(payload,1,size,f);fputs("}\n",f);if(fclose(f))error="resource_limit";}}}
    if(!error)error=sources_interrupted(cfg);
    if(transaction){if(error)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);error="store_failed";}}
    if(!error)fwrite(response,1,response_length,out);
    free(response);free(admission);free(payload);return error?fail(out,error):0;
}
