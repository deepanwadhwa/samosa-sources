#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "transcript.h"
#include "audio.h"
#include "cache.h"
#include "control.h"
#include "sha256.h"
#include <time.h>
#define RESULT_MAX 262144u
static int equal(const Json *j,int object,const char *name,const char *expected){char *value=json_string(j,json_get(j,object,name));int same=value&&expected&&!strcmp(value,expected);free(value);return same;}
static int number(const Json *j,int object,const char *name,size_t expected){size_t value=0;return json_uint(j,json_get(j,object,name),&value)&&value==expected;}
static int timing_policy(const Json *j,int provenance){return json_get(j,provenance,"timestamp_policy")<0||equal(j,provenance,"timestamp_policy","whisper_20ms_end_clipped_to_input");}
static int sha(const char *s){if(!s||strlen(s)!=64)return 0;for(size_t i=0;i<64;i++)if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static int configured(const SourcesConfig *cfg){const SourcesInferenceConfig *i=cfg?cfg->inference:NULL;if(!i||!i->provider_id||!*i->provider_id||strlen(i->provider_id)>128||!sha(i->engine_sha256)||!sha(i->model_sha256))return 0;for(const unsigned char *p=(const unsigned char*)i->provider_id;*p;p++)if(*p>127||(!isalnum(*p)&&!strchr("-_:.",*p)))return 0;return 1;}
const char *sources_transcript_recipe(const SourcesConfig *cfg,char identity[65]){
    if(!configured(cfg))return "inference_unconfigured";
    Sha256 hash;sha256_init(&hash);const SourcesInferenceConfig *i=cfg->inference;
    sha256_update(&hash,"transcript-recipe-v1:",sizeof("transcript-recipe-v1:")-1);sha256_update(&hash,i->provider_id,strlen(i->provider_id));sha256_update(&hash,i->engine_sha256,64);sha256_update(&hash,i->model_sha256,64);unsigned char digest[32];sha256_final(&hash,digest);sha256_hex(digest,identity);return NULL;
}
static const char *provider_error(const char *body,size_t bytes){Json j;if(!json_parse(&j,body,bytes))return "inference_response_invalid";int token=json_get(&j,0,"error");if(token<0)return NULL;char *code=json_string(&j,json_get(&j,token,"code"));const char *error=!code?"inference_response_invalid":!strcmp(code,"cancelled")?"cancelled":!strcmp(code,"deadline_exceeded")?"deadline_exceeded":!strcmp(code,"busy")?"inference_busy":"inference_failed";free(code);return error;}
static void artifact_id(const SourcesConfig *cfg,const char *source,const char *prepared,size_t start,size_t end,char artifact[65]){char material[768];int n=snprintf(material,sizeof(material),"source-transcript-v1:%s:%s:%zu:%zu:%s:%s:%s",source,prepared,start,end,cfg->inference->provider_id,cfg->inference->engine_sha256,cfg->inference->model_sha256);hash_bytes(material,(size_t)n,artifact);}
static const char *segments(const SourcesConfig *cfg,const Json *j,int token,const char *text,size_t start,size_t end,FILE *out){
    if(token<0||j->tokens[token].type!=JT_ARRAY||j->tokens[token].count<1||j->tokens[token].count>128||!text||strlen(text)>16384||!utf8_valid((const unsigned char*)text,strlen(text)))return "inference_response_invalid";
    size_t previous=0,matched=0;int first=1;if(out)fputc('[',out);
    for(int t=token+1;t<j->tokens[token].next;t=j->tokens[t].next){const char *error=sources_interrupted(cfg);if(error)return error;size_t from=0,to=0;char *cue=json_string(j,json_get(j,t,"text"));
        if(j->tokens[t].type!=JT_OBJECT||j->tokens[t].count!=3||!json_uint(j,json_get(j,t,"start_ms"),&from)||!json_uint(j,json_get(j,t,"end_ms"),&to)||from>=to||to>end-start||from<previous||!cue||!*cue||!utf8_valid((unsigned char*)cue,strlen(cue))){free(cue);return "inference_response_invalid";}
        size_t n=strlen(cue);if(!first){if(matched>=strlen(text)||text[matched++]!='\n'){free(cue);return "inference_response_invalid";}}
        if(n>strlen(text)-matched||memcmp(text+matched,cue,n)){free(cue);return "inference_response_invalid";}matched+=n;previous=from;
        if(out){if(!first)fputc(',',out);fprintf(out,"{\"start_ms\":%zu,\"end_ms\":%zu,\"text\":",start+from,start+to);json_quote(out,cue,n);fputc('}',out);}first=0;free(cue);
    }
    if(matched!=strlen(text))return "inference_response_invalid";
    if(out)fputc(']',out);
    return NULL;
}
const char *sources_transcript_project(const SourcesConfig *cfg,const char *input,size_t length,const char *source,const char *prepared,const char *input_sha,size_t start,size_t end,char **body,size_t *body_bytes){
    *body=NULL;*body_bytes=0;if(!configured(cfg)||!sha(source)||!sha(prepared)||!sha(input_sha)||start>=end||end-start>30000||end>14400000)return "invalid_configuration";
    Json j;const char *error=sources_interrupted(cfg);
    if(!error&&(length>RESULT_MAX||!json_parse(&j,input,length)||j.tokens[0].type!=JT_OBJECT||json_get(&j,0,"error")>=0||!number(&j,0,"contract_version",1)||!equal(&j,0,"provider_id",cfg->inference->provider_id)||!equal(&j,0,"kind","derived")||!equal(&j,0,"artifact_type","transcript")||!equal(&j,0,"language","en")||!equal(&j,0,"input_sha256",input_sha)||!number(&j,0,"duration_ms",end-start)))error="inference_response_invalid";
    int coverage=!error?json_get(&j,0,"coverage"):-1,provenance=!error?json_get(&j,0,"provenance"):-1;
    if(!error&&(!equal(&j,coverage,"status","partial")||!equal(&j,coverage,"reason","speech_recognition_uncertain")||!equal(&j,provenance,"engine","whisper.cpp")||!equal(&j,provenance,"engine_sha256",cfg->inference->engine_sha256)||!equal(&j,provenance,"model_sha256",cfg->inference->model_sha256)||!equal(&j,provenance,"coordinate_space","input_audio_milliseconds")))error="inference_response_invalid";
    if(!error&&!timing_policy(&j,provenance))error="inference_response_invalid";
    int timing=!error&&json_get(&j,provenance,"timestamp_policy")>=0;
    char *text=error?NULL:json_string(&j,json_get(&j,0,"text"));char artifact[65];artifact_id(cfg,source,prepared,start,end,artifact);FILE *out=error?NULL:open_memstream(body,body_bytes);if(!error&&!out)error="resource_limit";
    if(out){fprintf(out,"{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\",\"artifact_id\":\"%s\",\"kind\":\"derived\",\"artifact_type\":\"transcript\",\"language\":\"en\",\"text\":",source,source,artifact);if(text)json_quote(out,text,strlen(text));else fputs("null",out);fputs(",\"segments\":",out);error=segments(cfg,&j,json_get(&j,0,"segments"),text,start,end,out);
        fprintf(out,",\"range\":{\"start_ms\":%zu,\"end_ms\":%zu,\"coordinate_space\":\"source_time_ms\"},\"coverage\":{\"status\":\"partial\",\"reason\":\"speech_recognition_uncertain\"},\"provenance\":{\"reader\":\"public-inference-transcript-v1\",\"prepared_artifact_id\":\"%s\",\"input_sha256\":\"%s\",\"provider_id\":\"%s\",\"engine\":\"whisper.cpp\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"derivation\":\"speech_recognition\"",start,end,prepared,input_sha,cfg->inference->provider_id,cfg->inference->engine_sha256,cfg->inference->model_sha256);
        if(timing)fputs(",\"timestamp_policy\":\"whisper_20ms_end_clipped_to_input\"",out);
        fputs("}}\n",out);
        if(fclose(out))error="resource_limit";
        if(!error&&*body_bytes>RESULT_MAX)error="inference_response_invalid";}
    free(text);if(!error)error=sources_interrupted(cfg);if(error){free(*body);*body=NULL;*body_bytes=0;}return error;
}
static int b64_value(unsigned char c){if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;return c=='+'?62:c=='/'?63:-1;}
static const char *input_digest(const SourcesConfig *cfg,const char *text,size_t duration,char digest[65]){
    size_t n=text?strlen(text):0;if(!n||n%4||n>((960044u+2)/3)*4)return "corrupt_cache";unsigned char *wave=malloc(n/4*3);if(!wave)return "resource_limit";size_t used=0;const char *error=NULL;
    for(size_t i=0;i<n&&!error;i+=4){if(!(i%4096))error=sources_interrupted(cfg);int a=b64_value((unsigned char)text[i]),b=b64_value((unsigned char)text[i+1]),c=b64_value((unsigned char)text[i+2]),d=b64_value((unsigned char)text[i+3]);int pc=text[i+2]=='=',pd=text[i+3]=='=';
        if(a<0||b<0||(!pc&&c<0)||(!pd&&d<0)||(pc&&!pd)||((pc||pd)&&i+4!=n)||(pc&&(b&15))||(!pc&&pd&&(c&3)))error="corrupt_cache";
        if(!error){wave[used++]=(unsigned char)((a<<2)|(b>>4));if(!pc)wave[used++]=(unsigned char)((b<<4)|(c>>2));if(!pd)wave[used++]=(unsigned char)((c<<6)|d);}}
    SourcesWave parsed;if(!error)error=sources_wave_inspect(cfg,wave,used,&parsed);if(!error&&(used!=44+duration*32||parsed.frames!=duration*16))error="corrupt_cache";if(!error)hash_bytes(wave,used,digest);free(wave);return error;
}
static const char *cached_valid(const SourcesConfig *cfg,const char *body,size_t length,const char *source,const char *prepared,const char *input_sha,size_t start,size_t end){
    Json j;char artifact[65];artifact_id(cfg,source,prepared,start,end,artifact);
    if(length>RESULT_MAX||!json_parse(&j,body,length)||j.tokens[0].type!=JT_OBJECT||json_get(&j,0,"evidence_id")>=0||json_get(&j,0,"error")>=0||json_get(&j,0,"operation_id")>=0||json_get(&j,0,"parent_operation_id")>=0||!number(&j,0,"contract_version",1)||!equal(&j,0,"source_id",source)||!equal(&j,0,"version",source)||!equal(&j,0,"artifact_id",artifact)||!equal(&j,0,"kind","derived")||!equal(&j,0,"artifact_type","transcript")||!equal(&j,0,"language","en"))return "corrupt_cache";
    int range=json_get(&j,0,"range"),coverage=json_get(&j,0,"coverage"),provenance=json_get(&j,0,"provenance");
    if(!timing_policy(&j,provenance))return "corrupt_cache";
    if(!number(&j,range,"start_ms",start)||!number(&j,range,"end_ms",end)||!equal(&j,range,"coordinate_space","source_time_ms")||!equal(&j,coverage,"status","partial")||!equal(&j,coverage,"reason","speech_recognition_uncertain")||!equal(&j,provenance,"reader","public-inference-transcript-v1")||!equal(&j,provenance,"prepared_artifact_id",prepared)||!equal(&j,provenance,"input_sha256",input_sha)||!equal(&j,provenance,"provider_id",cfg->inference->provider_id)||!equal(&j,provenance,"engine","whisper.cpp")||!equal(&j,provenance,"engine_sha256",cfg->inference->engine_sha256)||!equal(&j,provenance,"model_sha256",cfg->inference->model_sha256)||!equal(&j,provenance,"derivation","speech_recognition"))return "corrupt_cache";
    /* Validate saved absolute times through the same local-time validator. */
    char *relative=NULL;size_t bytes=0;FILE *out=open_memstream(&relative,&bytes);if(!out)return "resource_limit";int list=json_get(&j,0,"segments");const char *error=NULL;
    if(list<0||j.tokens[list].type!=JT_ARRAY||j.tokens[list].count<1||j.tokens[list].count>128)error="corrupt_cache";
    fputc('[',out);int first=1;for(int t=list+1;!error&&t<j.tokens[list].next;t=j.tokens[t].next){size_t from=0,to=0;char *text=json_string(&j,json_get(&j,t,"text"));if(j.tokens[t].count!=3||!json_uint(&j,json_get(&j,t,"start_ms"),&from)||!json_uint(&j,json_get(&j,t,"end_ms"),&to)||from<start||to<start||from>=to||to>end||!text)error="corrupt_cache";else{if(!first)fputc(',',out);fprintf(out,"{\"start_ms\":%zu,\"end_ms\":%zu,\"text\":",from-start,to-start);json_quote(out,text,strlen(text));fputc('}',out);first=0;}free(text);}fputc(']',out);if(fclose(out))error="resource_limit";
    if(!error){Json local;char *text=json_string(&j,json_get(&j,0,"text"));if(!json_parse(&local,relative,bytes))error="corrupt_cache";else{error=segments(cfg,&local,0,text,start,end,NULL);if(error&&!strcmp(error,"inference_response_invalid"))error="corrupt_cache";}free(text);}free(relative);return error;
}
const char *sources_transcript_capture_validate(const SourcesConfig *cfg,const char *body,size_t length){
    Json j;if(!configured(cfg)||length>RESULT_MAX||!json_parse(&j,body,length))return "corrupt_cache";
    int provenance=json_get(&j,0,"provenance"),range=json_get(&j,0,"range");size_t start=0,end=0;
    char *source=json_string(&j,json_get(&j,0,"source_id")),*prepared=json_string(&j,json_get(&j,provenance,"prepared_artifact_id")),*input=json_string(&j,json_get(&j,provenance,"input_sha256"));
    const char *error=NULL;
    if(!sha(source)||!sha(prepared)||!sha(input)||!json_uint(&j,json_get(&j,range,"start_ms"),&start)||!json_uint(&j,json_get(&j,range,"end_ms"),&end)||start>=end||end-start>30000||end>14400000)error="corrupt_cache";
    if(!error)error=cached_valid(cfg,body,length,source,prepared,input,start,end);
    free(source);free(prepared);free(input);return error;
}
static const char *cache_get(sqlite3 *db,const SourcesConfig *cfg,const char *key,const char *source,const char *prepared,const char *input_sha,size_t start,size_t end,char **body,size_t *length){
    *body=NULL;*length=0;sqlite3_stmt *stmt=NULL;if(sqlite3_prepare_v2(db,"SELECT response,digest FROM prepared_artifacts WHERE key=?",-1,&stmt,NULL)!=SQLITE_OK)return "store_failed";sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(stmt);const char *error=NULL;
    if(rc==SQLITE_ROW){int n=sqlite3_column_bytes(stmt,0);const char *response=(const char*)sqlite3_column_text(stmt,0),*expected=(const char*)sqlite3_column_text(stmt,1);if(!response||!expected||n<=0||(size_t)n>RESULT_MAX||strlen(response)!=(size_t)n)error="corrupt_cache";else{char actual[65];hash_bytes(response,(size_t)n,actual);if(strcmp(actual,expected))error="corrupt_cache";else{*body=strdup(response);if(!*body)error="resource_limit";else *length=(size_t)n;}}}else if(rc!=SQLITE_DONE)error="store_failed";sqlite3_finalize(stmt);
    if(!error&&*body)error=cached_valid(cfg,*body,*length,source,prepared,input_sha,start,end);
    if(error){free(*body);*body=NULL;*length=0;}
    return error;
}
static int fail(FILE *out,const char *error){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",error);return 1;}
static int transcript_dispatch(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out,int persistent){
    if(!configured(cfg))return fail(out,"inference_unconfigured");
    SourcesConfig scoped=*cfg;struct timespec now;if(!scoped.deadline_monotonic){if(clock_gettime(CLOCK_MONOTONIC,&now))return fail(out,"state_unavailable");scoped.deadline_monotonic=(double)now.tv_sec+(double)now.tv_nsec/1e9+60;}cfg=&scoped;
    char *source=json_string(request,json_get(request,0,"source_id")),*version=json_string(request,json_get(request,0,"version"));size_t start=0,end=0;const char *error=NULL;
    if(!sha(source)||!sha(version)||!json_uint(request,json_get(request,0,"start_ms"),&start)||!json_uint(request,json_get(request,0,"end_ms"),&end))error="invalid_request";else if(strcmp(source,version))error="stale_source";else if(start>=end||end-start>30000||end>14400000)error="invalid_range";
    char *prepared_body=NULL;size_t prepared_bytes=0;FILE *stream=error?NULL:open_memstream(&prepared_body,&prepared_bytes);if(!error&&!stream)error="resource_limit";
    if(stream){char prepare[512];snprintf(prepare,sizeof(prepare),"{\"contract_version\":1,\"op\":\"prepare_audio\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu}",source,version,start,end);Json j;json_parse(&j,prepare,strlen(prepare));int rc=persistent?sources_media_prepare(cfg,&j,db,stream):sources_media_prepare_transient(cfg,&j,db,stream);if(fclose(stream))error="resource_limit";else if(rc){fwrite(prepared_body,1,prepared_bytes,out);free(prepared_body);free(source);free(version);return 1;}}
    Json prepared;char *data=NULL,*prepared_id=NULL;if(!error){if(!json_parse(&prepared,prepared_body,prepared_bytes))error="corrupt_cache";else{data=json_string(&prepared,json_get(&prepared,0,"data"));prepared_id=json_string(&prepared,json_get(&prepared,0,"artifact_id"));if(!sha(prepared_id)||!data)error="corrupt_cache";}}
    char input_sha[65]={0},artifact[65]={0},key[65]={0};if(!error)error=input_digest(cfg,data,end-start,input_sha);
    if(!error){artifact_id(cfg,source,prepared_id,start,end,artifact);Sha256 hash;sha256_init(&hash);const char *owner=cfg->owner?cfg->owner:"local-owner";sha256_update(&hash,"transcript-owner-cache-v1:",sizeof("transcript-owner-cache-v1:")-1);sha256_update(&hash,owner,strlen(owner));sha256_update(&hash,artifact,64);unsigned char digest[32];sha256_final(&hash,digest);sha256_hex(digest,key);}
    char *body=NULL;size_t bytes=0;if(!error)error=cache_get(db,cfg,key,source,prepared_id,input_sha,start,end,&body,&bytes);int cached=body!=NULL;
    if(!error&&!cached&&persistent)error=sources_cache_capacity(db,cfg,0);
    char *invocation=NULL,*response=NULL;size_t invocation_bytes=0,response_bytes=0;
    if(!error&&!cached){if(!cfg->inference->call)error="inference_unavailable";else{FILE *f=open_memstream(&invocation,&invocation_bytes);if(!f)error="resource_limit";else{fprintf(f,"{\"contract_version\":1,\"op\":\"transcribe\",\"provider_id\":\"%s\",\"audio_wav_base64\":",cfg->inference->provider_id);json_quote(f,data,strlen(data));fputs(",\"timeout_ms\":60000}",f);if(fclose(f))error="resource_limit";}}}
    if(!error&&!cached){error=sources_interrupted(cfg);SourcesInferenceControl control={cfg->deadline_monotonic,cfg->cancelled,cfg->cancellation};if(!error)error=cfg->inference->call(cfg->inference->context,&control,invocation,invocation_bytes,&response,&response_bytes);if(error&&strcmp(error,"cancelled")&&strcmp(error,"deadline_exceeded"))error="inference_unavailable";if(!error)error=sources_interrupted(cfg);if(!error&&(!response||!response_bytes||response_bytes>RESULT_MAX||strlen(response)!=response_bytes))error="inference_response_invalid";if(!error)error=provider_error(response,response_bytes);if(!error)error=sources_transcript_project(cfg,response,response_bytes,source,prepared_id,input_sha,start,end,&body,&bytes);}
    if(!error&&!cached&&persistent){if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)error="store_busy";else{char *old=NULL;size_t old_bytes=0;error=cache_get(db,cfg,key,source,prepared_id,input_sha,start,end,&old,&old_bytes);if(!error&&old){if(old_bytes!=bytes||memcmp(old,body,bytes))error="cache_conflict";}else if(!error){error=sources_cache_capacity(db,cfg,bytes);sqlite3_stmt *stmt=NULL;if(!error&&sqlite3_prepare_v2(db,"INSERT INTO prepared_artifacts VALUES(?,?,?)",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";if(!error){char digest[65];hash_bytes(body,bytes,digest);sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,body,(int)bytes,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,digest,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);}free(old);if(!error)error=sources_interrupted(cfg);if(error)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);error="store_failed";}}}
    free(source);free(version);free(prepared_body);free(data);free(prepared_id);free(invocation);free(response);if(error){free(body);return fail(out,error);}fwrite(body,1,bytes,out);free(body);return 0;
}

int sources_transcript_dispatch(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out){return transcript_dispatch(cfg,request,db,out,1);}
int sources_transcript_window(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out){return transcript_dispatch(cfg,request,db,out,0);}
