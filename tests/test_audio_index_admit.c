#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "audio_index.h"
#include "index.h"
#include "sha256.h"
#include <assert.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static void put32(unsigned char *p,uint32_t value){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));}
static sqlite3_int64 scalar(sqlite3 *db,const char *query){sqlite3_stmt *s=NULL;assert(sqlite3_prepare_v2(db,query,-1,&s,NULL)==SQLITE_OK&&sqlite3_step(s)==SQLITE_ROW);sqlite3_int64 value=sqlite3_column_int64(s,0);assert(sqlite3_finalize(s)==SQLITE_OK);return value;}
static void sql(sqlite3 *db,const char *query){assert(sqlite3_exec(db,query,NULL,NULL,NULL)==SQLITE_OK);}
static char *call(const SourcesConfig *cfg,const char *request,int expected){char *response=NULL;size_t length=0;FILE *out=open_memstream(&response,&length);assert(out);assert(sources_dispatch(cfg,request,strlen(request),out)==expected&&!fclose(out));return response;}
static unsigned model_calls;
static int b64(unsigned char c){const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",*p=strchr(alphabet,c);return p?(int)(p-alphabet):0;}
static const char *transport(void *context,const SourcesInferenceControl *control,const char *input,size_t length,char **result,size_t *bytes){
    assert(control->deadline_monotonic>0);model_calls++;SourcesInferenceConfig *identity=context;
    Json request;assert(json_parse(&request,input,length));char *data=json_string(&request,json_get(&request,0,"audio_wav_base64"));assert(data);size_t n=strlen(data),used=0;unsigned char *wave=malloc(n/4*3);assert(wave);for(size_t i=0;i<n;i+=4){int a=b64((unsigned char)data[i]),b=b64((unsigned char)data[i+1]),c=b64((unsigned char)data[i+2]),d=b64((unsigned char)data[i+3]);wave[used++]=(unsigned char)((a<<2)|(b>>4));if(data[i+2]!='=')wave[used++]=(unsigned char)((b<<4)|(c>>2));if(data[i+3]!='=')wave[used++]=(unsigned char)((c<<6)|d);}free(data);assert(used>=32044&&used<=960044&&!memcmp(wave,"RIFF",4));char digest[65];hash_bytes(wave,used,digest);size_t duration=(used-44)/32;free(wave);
    FILE *out=open_memstream(result,bytes);assert(out);fprintf(out,"{\"contract_version\":1,\"provider_id\":\"%s\",\"kind\":\"derived\",\"artifact_type\":\"transcript\",\"language\":\"en\",\"input_sha256\":\"%s\",\"duration_ms\":%zu,\"text\":\"Public transcript fixture.\",\"segments\":[{\"start_ms\":0,\"end_ms\":500,\"text\":\"Public transcript fixture.\"}],\"coverage\":{\"status\":\"partial\",\"reason\":\"speech_recognition_uncertain\"},\"provenance\":{\"engine\":\"whisper.cpp\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"coordinate_space\":\"input_audio_milliseconds\"}}",identity->provider_id,digest,duration,identity->engine_sha256,identity->model_sha256);assert(!fclose(out));return NULL;
}
int main(void){
    char base[]="/tmp/samosa-audio-admit-XXXXXX";assert(mkdtemp(base));char root[512],state[512],path[600],database[600];snprintf(root,sizeof(root),"%s/files",base);snprintf(state,sizeof(state),"%s/state",base);assert(!mkdir(root,0700));snprintf(path,sizeof(path),"%s/voice.wav",root);
    unsigned char wave[32044]={0};memcpy(wave,"RIFF",4);put32(wave+4,sizeof(wave)-8);memcpy(wave+8,"WAVEfmt ",8);put32(wave+16,16);wave[20]=1;wave[22]=1;put32(wave+24,16000);put32(wave+28,32000);wave[32]=2;wave[34]=16;memcpy(wave+36,"data",4);put32(wave+40,sizeof(wave)-44);
    FILE *file=fopen(path,"wb");assert(file&&fwrite(wave,1,sizeof(wave),file)==sizeof(wave)&&!fclose(file));char source[65],engine[65],model[65];hash_bytes(wave,sizeof(wave),source);memset(engine,'a',64);engine[64]=0;memset(model,'b',64);model[64]=0;
    SourcesInferenceConfig inference={.provider_id="fixture",.engine_sha256=engine,.model_sha256=model};SourcesConfig cfg={.root=root,.state=state,.owner="audio-owner",.inference=&inference};
    const char *request="{\"contract_version\":1,\"op\":\"register_audio\",\"path\":\"voice.wav\"}";char *response=NULL;size_t length=0;FILE *out=open_memstream(&response,&length);assert(out);assert(!sources_dispatch(&cfg,request,strlen(request),out)&&!fclose(out));assert(strstr(response,source));free(response);
    snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);int directory=open(state,O_RDONLY|O_DIRECTORY),lease=-1;assert(directory>=0&&!source_index_lease(directory,&lease));
    char id[65],error[65],rolled_back[65];assert(!strcmp(sources_audio_index_admit(&cfg,db,source,1000,id,error),"invalid_configuration")&&!id[0]);
    sql(db,"BEGIN IMMEDIATE");assert(!sources_audio_index_admit(&cfg,db,source,1000,id,error));assert(!sqlite3_get_autocommit(db)&&scalar(db,"SELECT count(*) FROM audio_index_jobs")==1);memcpy(rolled_back,id,65);sql(db,"ROLLBACK");assert(!scalar(db,"SELECT count(*) FROM sqlite_master WHERE name='audio_index_jobs'"));
    sql(db,"BEGIN IMMEDIATE");assert(!sources_audio_index_admit(&cfg,db,source,1000,id,error)&&!strcmp(id,rolled_back));assert(!sqlite3_get_autocommit(db));sql(db,"COMMIT");assert(scalar(db,"SELECT count(*) FROM audio_index_jobs")==1);
    sql(db,"UPDATE audio_index_jobs SET next_ms=1000,failures=1,last_error='inference_busy'");sql(db,"BEGIN IMMEDIATE");assert(!sources_audio_index_admit(&cfg,db,source,1000,id,error)&&!strcmp(id,rolled_back));sql(db,"COMMIT");assert(scalar(db,"SELECT count(*) FROM audio_index_jobs")==1&&scalar(db,"SELECT next_ms FROM audio_index_jobs")==1000&&scalar(db,"SELECT failures FROM audio_index_jobs")==1);
    sql(db,"BEGIN IMMEDIATE");assert(!strcmp(sources_audio_index_admit(&cfg,db,source,1001,id,error),"invalid_range")&&!id[0]&&!sqlite3_get_autocommit(db));sql(db,"ROLLBACK");assert(scalar(db,"SELECT count(*) FROM audio_index_jobs")==1);
    cfg.owner="other-owner";sql(db,"BEGIN IMMEDIATE");assert(!strcmp(sources_audio_index_admit(&cfg,db,source,1000,id,error),"unknown_source")&&!id[0]&&!sqlite3_get_autocommit(db));sql(db,"ROLLBACK");assert(scalar(db,"SELECT count(*) FROM audio_index_jobs")==1);
    close(lease);close(directory);assert(sqlite3_close(db)==SQLITE_OK);
    cfg.owner="audio-owner";
    response=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"discover-audio\"}",0);
    Json j;assert(json_parse(&j,response,strlen(response)));char *job=json_string(&j,json_get(&j,0,"job_id"));assert(job&&strlen(job)==64);free(response);
    char step[512];snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"queue-discovered-audio\",\"max_items\":1}",job);
    response=call(&cfg,step,0);assert(strstr(response,"\"status\":\"waiting_audio\"")&&strstr(response,"\"queued_audio_items\":1")&&strstr(response,"\"indexed_items\":0"));char *saved=strdup(response);assert(saved);free(response);
    char get[512];snprintf(get,sizeof(get),"{\"contract_version\":1,\"op\":\"index_get\",\"job_id\":\"%s\"}",job);
    response=call(&cfg,get,0);assert(strstr(response,"\"audio_jobs\":[{\"ordinal\":0,\"audio_job\":")&&strstr(response,rolled_back)&&strstr(response,"\"completed_audio_items\":0"));free(response);
    assert(sqlite3_open(database,&db)==SQLITE_OK);
    assert(scalar(db,"SELECT count(*) FROM index_audio a JOIN audio_index_jobs q ON q.id=a.audio_job WHERE a.source=q.source")==1);
    assert(scalar(db,"SELECT count(*) FROM index_items WHERE status='queued_audio' AND text IS NULL AND evidence IS NULL")==1);
    assert(scalar(db,"SELECT count(*) FROM audio_index_jobs")==1&&scalar(db,"SELECT next_ms FROM audio_index_jobs")==1000);
    assert(sqlite3_close(db)==SQLITE_OK);
    response=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"unconfigured-audio\"}",0);
    assert(json_parse(&j,response,strlen(response)));char *pending=json_string(&j,json_get(&j,0,"job_id"));assert(pending);free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"unconfigured-step\",\"max_items\":1}",pending);
    SourcesConfig unavailable=cfg;unavailable.inference=NULL;response=call(&unavailable,step,1);assert(strstr(response,"inference_unconfigured"));free(response);
    assert(sqlite3_open(database,&db)==SQLITE_OK);assert(scalar(db,"SELECT count(*) FROM index_audio")==1&&scalar(db,"SELECT count(*) FROM index_items WHERE status='pending'")==1);assert(sqlite3_close(db)==SQLITE_OK);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"configured-retry\",\"max_items\":1}",pending);
    response=call(&cfg,step,0);assert(strstr(response,"waiting_audio"));free(response);free(pending);
    inference.call=transport;inference.context=&inference;
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"audio_index_step\",\"job_id\":\"%s\"}",rolled_back);response=call(&cfg,step,0);assert(strstr(response,"captured")&&model_calls==1);free(response);inference.call=NULL;
    assert(!unlink(path));response=call(&cfg,get,0);assert(strstr(response,"\"status\":\"completed\"")&&strstr(response,"\"completed_audio_items\":1")&&strstr(response,"\"queued_audio_items\":0")&&strstr(response,"\"indexed_items\":0")&&strstr(response,"\"content_coverage\":\"partial\""));free(response);
    /* A live change queues a new owned version without replacing saved captures. */
    char watch_request[768];snprintf(watch_request,sizeof(watch_request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch-audio\",\"locators\":[{\"path\":\"voice.wav\",\"expected_version\":\"%s\"}]}",source);
    response=call(&cfg,watch_request,0);assert(json_parse(&j,response,strlen(response)));char *watch=json_string(&j,json_get(&j,0,"watch_id"));assert(watch);free(response);
    wave[44]=1;char next_source[65];hash_bytes(wave,sizeof(wave),next_source);file=fopen(path,"wb");assert(file&&fwrite(wave,1,sizeof(wave),file)==sizeof(wave)&&!fclose(file));
    cfg.discovery_interval_ms=100;response=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",0);assert(strstr(response,"\"observed_items\":1")&&strstr(response,"\"freshness_error\":null"));free(response);cfg.discovery_interval_ms=0;
    snprintf(watch_request,sizeof(watch_request),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":0}",watch);response=call(&cfg,watch_request,0);free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);SourcesConfig retained=cfg;retained.root=NULL;retained.inference=NULL;response=call(&retained,step,0);assert(strstr(response,"\"pending_items\":1")&&strstr(response,source));free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"reindex-unconfigured\",\"max_items\":1}",job);response=call(&unavailable,step,1);assert(strstr(response,"inference_unconfigured"));free(response);
    assert(sqlite3_open(database,&db)==SQLITE_OK);assert(!scalar(db,"SELECT count(*) FROM index_audio_history")&&scalar(db,"SELECT count(*) FROM audio_index_jobs")==1);assert(sqlite3_close(db)==SQLITE_OK);
    wave[44]=2;file=fopen(path,"wb");assert(file&&fwrite(wave,1,sizeof(wave),file)==sizeof(wave)&&!fclose(file));
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"stale-audio\",\"max_items\":1}",job);response=call(&cfg,step,1);assert(strstr(response,"stale_source"));free(response);
    wave[44]=1;file=fopen(path,"wb");assert(file&&fwrite(wave,1,sizeof(wave),file)==sizeof(wave)&&!fclose(file));
    snprintf(watch_request,sizeof(watch_request),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":1}",watch);response=call(&cfg,watch_request,0);free(response);
    response=call(&cfg,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",0);assert(strstr(response,"\"reindexed_items\":1"));free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"reindex-audio\",\"max_items\":1}",job);response=call(&cfg,step,0);assert(strstr(response,"\"reindexed_items\":1"));char *reindexed=strdup(response);assert(reindexed);free(response);response=call(&retained,step,0);assert(!strcmp(response,reindexed));free(response);free(reindexed);
    response=call(&cfg,get,0);assert(strstr(response,next_source)&&strstr(response,"waiting_audio")&&strstr(response,"\"indexed_items\":0"));assert(json_parse(&j,response,strlen(response)));int jobs=json_get(&j,0,"audio_jobs"),entry=jobs+1,audio=json_get(&j,entry,"audio_job");char *next_job=json_string(&j,json_get(&j,audio,"job_id"));assert(next_job);free(response);
    assert(sqlite3_open(database,&db)==SQLITE_OK);assert(scalar(db,"SELECT count(*) FROM index_audio_history")==2&&scalar(db,"SELECT count(*) FROM audio_index_windows")==1&&scalar(db,"SELECT count(*) FROM audio_index_jobs")==2);assert(sqlite3_close(db)==SQLITE_OK);
    assert(!unlink(path));inference.call=transport;snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"audio_index_step\",\"job_id\":\"%s\"}",next_job);response=call(&cfg,step,0);assert(model_calls==2);free(response);inference.call=NULL;free(watch);
    response=call(&cfg,get,0);assert(strstr(response,"\"completed_audio_items\":1"));free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_audio_history\",\"job_id\":\"%s\"}",job);response=call(&cfg,step,0);assert(strstr(response,rolled_back)&&strstr(response,"\"returned_artifacts\":1"));size_t after=0;assert(json_parse(&j,response,strlen(response))&&json_uint(&j,json_get(&j,0,"next_after"),&after)&&after);free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_audio_history\",\"job_id\":\"%s\",\"after\":%zu}",job,after);response=call(&cfg,step,0);assert(strstr(response,"\"returned_artifacts\":0"));free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_audio_read\",\"job_id\":\"%s\",\"audio_job_id\":\"%s\",\"ordinal\":0}",job,rolled_back);response=call(&cfg,step,0);assert(strstr(response,source)&&strstr(response,"\"kind\":\"derived\"")&&strstr(response,"artifact_id")&&!strstr(response,"evidence_id"));char *old_capture=response;
    SourcesConfig foreign=cfg;foreign.owner="foreign";response=call(&foreign,step,1);assert(strstr(response,"unknown_index_artifact"));free(response);
    char direct[512];snprintf(direct,sizeof(direct),"{\"contract_version\":1,\"op\":\"audio_index_read\",\"job_id\":\"%s\",\"ordinal\":0}",rolled_back);response=call(&cfg,direct,0);assert(!strcmp(old_capture,response));free(old_capture);free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_audio_read\",\"job_id\":\"%s\",\"audio_job_id\":\"%s\",\"ordinal\":0}",job,next_job);response=call(&cfg,step,0);assert(strstr(response,next_source)&&model_calls==2);free(response);free(next_job);
    SourcesInferenceConfig changed=inference;changed.model_sha256=engine;foreign=cfg;foreign.inference=&changed;response=call(&foreign,step,1);assert(strstr(response,"audio_index_configuration_changed"));free(response);
    /* Search retained derived captures without a model or original live file. */
    char search_request[512];snprintf(search_request,sizeof(search_request),"{\"contract_version\":1,\"op\":\"index_audio_search\",\"job_id\":\"%s\",\"query\":\"TRANSCRIPT\"}",job);
    response=call(&cfg,search_request,0);assert(json_parse(&j,response,strlen(response))&&strstr(response,"\"returned_candidates\":2")&&strstr(response,"\"scanned_captures\":2")&&strstr(response,"\"uncaptured_windows\":0")&&strstr(response,source)&&strstr(response,next_source)&&strstr(response,"derived_preview")&&strstr(response,"artifact_id")&&strstr(response,"provenance")&&strstr(response,"range")&&!strstr(response,"evidence_id")&&!strstr(response,"literal_preview")&&model_calls==2);free(response);
    foreign=cfg;foreign.owner="foreign";response=call(&foreign,search_request,1);assert(strstr(response,"unknown_index")&&!strstr(response,"candidates"));free(response);
    foreign=cfg;foreign.inference=&changed;response=call(&foreign,search_request,1);assert(strstr(response,"audio_index_configuration_changed")&&!strstr(response,"derived_preview"));free(response);
    snprintf(search_request,sizeof(search_request),"{\"contract_version\":1,\"op\":\"index_audio_search\",\"job_id\":\"%s\",\"query\":\"absent phrase\"}",job);response=call(&cfg,search_request,0);assert(strstr(response,"\"returned_candidates\":0")&&strstr(response,"complete_for_retained_captures"));free(response);
    snprintf(search_request,sizeof(search_request),"{\"contract_version\":1,\"op\":\"index_audio_search\",\"job_id\":\"%s\",\"query\":\"\"}",job);response=call(&cfg,search_request,1);assert(strstr(response,"invalid_request"));free(response);
    /* A real longer reindex exercises uncaptured coverage and the ten-result cap. */
    size_t long_bytes=44+241000u*32;unsigned char *long_wave=calloc(1,long_bytes);assert(long_wave);memcpy(long_wave,wave,44);put32(long_wave+4,(uint32_t)long_bytes-8);put32(long_wave+40,(uint32_t)long_bytes-44);file=fopen(path,"wb");assert(file&&fwrite(long_wave,1,long_bytes,file)==long_bytes&&!fclose(file));free(long_wave);
    cfg.discovery_interval_ms=100;response=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",0);free(response);cfg.discovery_interval_ms=0;
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"long-audio\",\"max_items\":1}",job);response=call(&cfg,step,0);free(response);
    response=call(&cfg,get,0);assert(json_parse(&j,response,strlen(response)));jobs=json_get(&j,0,"audio_jobs");audio=json_get(&j,jobs+1,"audio_job");char *long_job=json_string(&j,json_get(&j,audio,"job_id"));size_t windows=0;assert(long_job&&json_uint(&j,json_get(&j,audio,"total_windows"),&windows)&&windows>8);free(response);
    snprintf(search_request,sizeof(search_request),"{\"contract_version\":1,\"op\":\"index_audio_search\",\"job_id\":\"%s\",\"query\":\"TRANSCRIPT\"}",job);response=call(&cfg,search_request,0);size_t uncaptured=0;assert(json_parse(&j,response,strlen(response))&&json_uint(&j,json_get(&j,0,"uncaptured_windows"),&uncaptured)&&uncaptured==windows&&strstr(response,"\"scanned_captures\":2"));free(response);
    assert(!unlink(path));inference.call=transport;snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"audio_index_step\",\"job_id\":\"%s\"}",long_job);for(size_t i=0;i<windows;i++){response=call(&cfg,step,0);free(response);}inference.call=NULL;free(long_job);
    response=call(&cfg,search_request,0);size_t scanned=0;assert(json_parse(&j,response,strlen(response))&&json_uint(&j,json_get(&j,0,"scanned_captures"),&scanned)&&scanned==windows+2&&strstr(response,"\"returned_candidates\":10")&&strstr(response,"\"candidate_coverage\":\"partial\"")&&strstr(response,"\"uncaptured_windows\":0")&&model_calls==windows+2);free(response);
    assert(sqlite3_open(database,&db)==SQLITE_OK);sql(db,"UPDATE audio_index_windows SET digest=printf('%064d',0) WHERE job=(SELECT max(job) FROM audio_index_windows)");assert(sqlite3_close(db)==SQLITE_OK);
    response=call(&cfg,search_request,1);assert(strstr(response,"corrupt_audio_index")&&!strstr(response,"candidates")&&!strstr(response,"derived_preview"));free(response);
    assert(sqlite3_open(database,&db)==SQLITE_OK);sql(db,"UPDATE audio_index_windows SET digest=printf('%064d',0)");assert(sqlite3_close(db)==SQLITE_OK);
    response=call(&cfg,get,1);assert(strstr(response,"corrupt_audio_index")&&!strstr(response,"\"audio_jobs\""));free(response);
    response=call(&cfg,step,1);assert(strstr(response,"corrupt_audio_index")&&!strstr(response,"artifact_id"));free(response);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"queue-discovered-audio\",\"max_items\":1}",job);
    SourcesConfig offline=cfg;offline.root=NULL;response=call(&offline,step,0);assert(!strcmp(saved,response));free(saved);free(response);free(job);
    puts("PASS audio discovery/freshness/reindex: atomic admission/rollback, stable queue identity/backoff, owned range refusal, typed completion/corruption validation, bounded refresh/watch intent, root/model-absent pending/replay and retained audio jobs/captures");return 0;
}
