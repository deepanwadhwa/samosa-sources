#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "embedding.h"
#include "sha256.h"
#include "sources.h"
#include "native.h"
#include "chutni.h"
#include "semantic.h"
#include "watch.h"
#include <signal.h>
#include <sys/wait.h>
#include <sqlite3.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static const char *pin =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char *profile =
    "{\"representation_kind\":\"text_embedding\",\"model_id\":\"fixture/"
    "embed\",\"model_revision\":\"fixed\",\"dimensions\":2,\"dtype\":\"f32\","
    "\"normalization\":\"l2\",\"tokenizer_hash\":null,\"projector_hash\":null}";
typedef struct {
  SourcesEmbeddingConfig *embedding;
  const char *profile;
  unsigned calls, mode;
  double deadline;
} Fixture;
static const char *embedding(void *context,
                             const SourcesInferenceControl *control,
                             const char *request, size_t bytes, char **response,
                             size_t *length) {
  Fixture *f = context;
  f->calls++;
  f->deadline = control->deadline_monotonic;
  *response = NULL;
  *length = 0;
  Json j;
  CHECK(json_parse(&j, request, bytes));
  char *op = json_string(&j, json_get(&j, 0, "op")),
       *text = json_string(&j, json_get(&j, 0, "text")),
       *role = json_string(&j, json_get(&j, 0, "role")),
       *title = json_string(&j, json_get(&j, 0, "title")),
       *operation = json_string(&j, json_get(&j, 0, "operation_id"));
  CHECK(op && !strcmp(op, "embed_text") && text && role && operation &&
        !strncmp(operation, "embed-", 6));
  CHECK(json_get(&j, 0, "model_id") < 0 && json_get(&j, 0, "provider_id") < 0);
  if (f->mode == 20) {
    free(op);
    free(text);
    free(role);
    free(title);
    free(operation);
    return "cancelled";
  }
  if (f->mode == 21) {
    free(op);
    free(text);
    free(role);
    free(title);
    free(operation);
    return "transport_failed";
  }
  if (f->mode == 19) {
    free(op);
    free(text);
    free(role);
    free(title);
    free(operation);
    return NULL;
  }
  char *prepared = NULL;
  size_t n = 0;
  FILE *out = open_memstream(&prepared, &n);
  CHECK(out);
  fputs(!strcmp(role, "query") ? f->embedding->query_prefix
                               : f->embedding->document_prefix,
        out);
  if (!strcmp(role, "document"))
    fprintf(out, "title: %s | text: ", title ? title : "none");
  fputs(text, out);
  CHECK(!fclose(out));
  char input[65], formatted[65];
  hash_bytes(text, strlen(text), input);
  hash_bytes(prepared, n, formatted);
  out = open_memstream(response, length);
  CHECK(out);
  if (f->mode == 18)
    fputs("{invalid", out);
  else if (f->mode == 22)
    fputs("{\"contract_version\":1,\"error\":{\"code\":\"busy\"}}", out);
  else {
    fprintf(
        out,
        "{\"contract_version\":1,\"kind\":\"derived\",\"artifact_type\":"
        "\"embedding\",\"provider_id\":\"%s\",\"input_sha256\":\"%s\",\"input_"
        "bytes\":%zu,\"input_tokens\":%u,\"role\":\"%s\",\"title\":",
        f->mode == 2 ? "wrong" : f->embedding->inference.provider_id,
        f->mode == 1 ? pin : input, strlen(text) + (f->mode == 13),
        f->mode == 12 ? 8193 : 12, f->mode == 6 ? "wrong" : role);
    if (title || f->mode == 15)
      json_quote(out, title ? title : "wrong", strlen(title ? title : "wrong"));
    else
      fputs("null", out);
    fprintf(out,
            ",\"prepared_input_sha256\":\"%s\",\"prepared_input_bytes\":%zu,"
            "\"profile\":",
            f->mode == 7 ? pin : formatted, n + (f->mode == 14));
    if (f->mode == 10)
      fputs(
          "{\"representation_kind\":\"text_embedding\",\"model_id\":\"fixture/"
          "embed\",\"model_revision\":\"fixed\",\"dimensions\":3,\"dtype\":"
          "\"f32\",\"normalization\":\"l2\"}",
          out);
    else if (f->mode == 11)
      fputs(
          "{\"representation_kind\":\"text_embedding\",\"model_id\":\"fixture/"
          "embed\",\"model_revision\":\"wrong\",\"dimensions\":2,\"dtype\":"
          "\"f32\",\"normalization\":\"l2\"}",
          out);
    else
      fputs(f->profile?f->profile:profile, out);
    fprintf(out,
            ",\"vector\":%s,\"provenance\":{\"model_sha256\":\"%s\",\"engine_"
            "sha256\":\"%s\",\"recipe\":\"%s\"},\"operation_id\":\"%s\"%s}",
            f->mode == 8   ? "[0,0]"
            : f->mode == 9 ? "[1e999,0]"
                           : "[1,0]",
            f->mode == 3 ? "wrong" : f->embedding->inference.model_sha256,
            f->mode == 4 ? "wrong" : f->embedding->inference.engine_sha256,
            f->mode == 5 ? "wrong" : f->embedding->recipe,
            f->mode == 17 ? "wrong" : operation,
            f->mode == 16 ? ",\"extra\":true" : "");
  }
  CHECK(!fclose(out));
  free(prepared);
  free(op);
  free(text);
  free(role);
  free(title);
  free(operation);
  return NULL;
}
static char *call(const SourcesConfig *cfg, const char *request,
                  const char *error) {
  char *body = NULL;
  size_t n = 0;
  FILE *out = open_memstream(&body, &n);
  CHECK(out);
  int rc = sources_dispatch(cfg, request, strlen(request), out);
  CHECK(!fclose(out));
  if (rc != (error ? 1 : 0))
    fprintf(stderr, "response: %s\n", body);
  CHECK(rc == (error ? 1 : 0));
  Json j;
  CHECK(json_parse(&j, body, n));
  if (error) {
    char *code =
        json_string(&j, json_get(&j, json_get(&j, 0, "error"), "code"));
    if (!code || strcmp(code, error))
      fprintf(stderr, "response: %s\n", body);
    CHECK(code && !strcmp(code, error));
    free(code);
  }
  return body;
}
static void symbol(void *library, const char *name, void *target, size_t n) {
  void *s = dlsym(library, name);
  CHECK(s && n == sizeof(s));
  memcpy(target, &s, n);
}
static char *field(const char *body, const char *key) {
  Json j;
  CHECK(json_parse(&j, body, strlen(body)));
  char *text = json_string(&j, json_get(&j, 0, key));
  CHECK(text);
  return text;
}
static int cancelled(void *context) {
  (void)context;
  return 1;
}
static char *vector_metadata(const SourcesConfig *cfg,const char *artifact,const char *text,const char *payload,const char *arguments){
  char *body=NULL;size_t bytes=0;FILE *out=open_memstream(&body,&bytes);CHECK(out);
  const char *keys[]={"library","store","store_id","artifact_id","text"};
  const char *values[]={cfg->chutni_library,cfg->chutni_store,cfg->chutni_store_id,artifact,text};
  fputc('{',out);for(size_t i=0;i<5;i++){if(i)fputc(',',out);json_quote(out,keys[i],strlen(keys[i]));fputc(':',out);json_quote(out,values[i],strlen(values[i]));}
  fputs(",\"payload_hash\":",out);if(payload)json_quote(out,payload,strlen(payload));else fputs("null",out);
  fputs(",\"arguments\":",out);fputs(arguments?arguments:"null",out);fputc('}',out);CHECK(!fclose(out));return body;
}
static void vector_worker(const SourcesConfig *cfg,const char *artifact){
  char *metadata=vector_metadata(cfg,artifact,"Evidence document 0",NULL,NULL),*body=NULL;
  size_t bytes=0;const char *error=sources_native_chutni_embedding(cfg,metadata,&body,&bytes);
  if(error)fprintf(stderr,"Vector read: %s\n",error);
  CHECK(!error&&body&&bytes&&!sources_chutni_embedding_validate(metadata,body,bytes));
  char *payload=field(body,"payload_hash");CHECK(payload&&!strncmp(payload,"blake3:",7));
  free(body);free(metadata);body=NULL;
  Json q;CHECK(json_parse(&q,"{\"vector\":[0.8,0.6]}",20));
  char *arguments=NULL;CHECK(!sources_semantic_arguments(cfg->chutni_semantic_profile,&q,json_get(&q,0,"vector"),&arguments));
  metadata=vector_metadata(cfg,artifact,"Evidence document 0",payload,arguments);
  error=sources_native_chutni_embedding(cfg,metadata,&body,&bytes);CHECK(error&&!strcmp(error,"chutni_publish_disabled")&&!body);
  SourcesConfig writer=*cfg;writer.chutni_publish=1;
  CHECK(!sources_native_chutni_embedding(&writer,metadata,&body,&bytes));
  CHECK(strstr(body,"\"recovered\":true")&&!sources_chutni_embedding_validate(metadata,body,bytes));
  char *digest_position=strstr(body,"\"text_sha256\":\"");CHECK(digest_position);
  digest_position+=strlen("\"text_sha256\":\"");char original_digest=*digest_position;
  *digest_position=original_digest=='0'?'1':'0';CHECK(sources_chutni_embedding_validate(metadata,body,bytes));*digest_position=original_digest;
  char *representation=field(body,"representation_id");CHECK(representation);free(body);body=NULL;
  CHECK(!sources_native_chutni_embedding(&writer,metadata,&body,&bytes));
  char *retry=field(body,"representation_id");CHECK(retry&&!strcmp(representation,retry));free(retry);free(body);free(representation);body=NULL;
  CHECK(sources_chutni_embedding_validate(metadata,"{}",2));
  SourcesConfig wrong_profile=writer;wrong_profile.chutni_semantic_profile=NULL;
  error=sources_native_chutni_embedding(&wrong_profile,metadata,&body,&bytes);CHECK(error&&!strcmp(error,"invalid_semantic_configuration")&&!body);
  free(metadata);
  char changed[72];snprintf(changed,sizeof(changed),"%s",payload);changed[7]=changed[7]=='0'?'1':'0';
  metadata=vector_metadata(cfg,artifact,"Evidence document 0",changed,arguments);
  error=sources_native_chutni_embedding(&writer,metadata,&body,&bytes);CHECK(error&&!strcmp(error,"chutni_source_changed")&&!body);free(metadata);
  metadata=vector_metadata(cfg,artifact,"Wrong document",NULL,NULL);
  error=sources_native_chutni_embedding(cfg,metadata,&body,&bytes);CHECK(error&&!strcmp(error,"chutni_source_changed")&&!body);free(metadata);
  metadata=vector_metadata(cfg,artifact,"Evidence document 0",NULL,NULL);
  SourcesConfig refused=*cfg;refused.chutni_store_id="00000000-0000-0000-0000-000000000000";
  error=sources_native_chutni_embedding(&refused,metadata,&body,&bytes);CHECK(error&&!strcmp(error,"invalid_request")&&!body);
  refused=*cfg;refused.cancelled=cancelled;
  error=sources_native_chutni_embedding(&refused,metadata,&body,&bytes);CHECK(error&&!strcmp(error,"cancelled")&&!body);
  refused=*cfg;struct timespec expired;CHECK(!clock_gettime(CLOCK_MONOTONIC,&expired));
  refused.deadline_monotonic=(double)expired.tv_sec+(double)expired.tv_nsec/1e9-1;
  error=sources_native_chutni_embedding(&refused,metadata,&body,&bytes);CHECK(error&&!strcmp(error,"deadline_exceeded")&&!body);
  free(metadata);free(payload);free(arguments);
}
static void interrupted_embedding(const SourcesConfig *cfg,const char *root,const char *name){
  char marker[512];CHECK(snprintf(marker,sizeof(marker),"%s/%s",root,name)<(int)sizeof(marker));
  CHECK(!setenv(name,marker,1));pid_t child=fork();CHECK(child>=0);
  if(!child){FILE *out=fopen("/dev/null","w");if(!out)_exit(90);const char *request="{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}";int rc=sources_dispatch(cfg,request,strlen(request),out);fclose(out);_exit(rc);}
  CHECK(!unsetenv(name));int ready=0;
  for(int i=0;i<1000;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}
  CHECK(ready);CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);CHECK(!unlink(marker));
}
static void replace_receipt(sqlite3 *db,const char *key,const char *body){
  char digest[65];hash_bytes(body,strlen(body),digest);sqlite3_stmt *q=NULL;
  CHECK(sqlite3_prepare_v2(db,"UPDATE index_embeddings SET receipt=?,receipt_digest=? WHERE key=?",-1,&q,NULL)==SQLITE_OK);
  CHECK(sqlite3_bind_text(q,1,body,-1,SQLITE_TRANSIENT)==SQLITE_OK);CHECK(sqlite3_bind_text(q,2,digest,-1,SQLITE_TRANSIENT)==SQLITE_OK);CHECK(sqlite3_bind_text(q,3,key,-1,SQLITE_TRANSIENT)==SQLITE_OK);CHECK(sqlite3_step(q)==SQLITE_DONE);CHECK(sqlite3_finalize(q)==SQLITE_OK);
}
static void archived_corruption(const SourcesConfig *cfg,const char *request){
  char path[512];CHECK(snprintf(path,sizeof(path),"%s/sources.sqlite3",cfg->state)<(int)sizeof(path));
  sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_busy_timeout(db,5000)==SQLITE_OK);
  sqlite3_stmt *q=NULL;CHECK(sqlite3_prepare_v2(db,"SELECT key,receipt FROM index_embeddings WHERE status='published' LIMIT 1",-1,&q,NULL)==SQLITE_OK);
  CHECK(sqlite3_step(q)==SQLITE_ROW);char *key=strdup((const char *)sqlite3_column_text(q,0)),*original=strdup((const char *)sqlite3_column_text(q,1));CHECK(key&&original);CHECK(sqlite3_finalize(q)==SQLITE_OK);
  CHECK(sqlite3_exec(db,"BEGIN",NULL,NULL,NULL)==SQLITE_OK);
  CHECK(sqlite3_exec(db,"UPDATE index_embeddings SET artifact='00000000-0000-0000-0000-000000000000'",NULL,NULL,NULL)==SQLITE_OK);
  CHECK(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK);
  char *body=call(cfg,request,"corrupt_embedding");free(body);
  CHECK(sqlite3_exec(db,"UPDATE index_embeddings SET artifact=json_extract(receipt,'$.artifact_id')",NULL,NULL,NULL)==SQLITE_OK);
  body=call(cfg,request,NULL);free(body);
  Json j;CHECK(json_parse(&j,original,strlen(original)));int token=json_get(&j,0,"text_sha256");CHECK(token>=0);
  char *changed=strdup(original);CHECK(changed);memset(changed+j.tokens[token].begin+1,'0',64);
  replace_receipt(db,key,changed);body=call(cfg,request,"corrupt_embedding");free(body);free(changed);
  int vector=json_get(&j,json_get(&j,0,"arguments"),"vector");CHECK(vector>=0);
  size_t n=0;FILE *f=open_memstream(&changed,&n);CHECK(f);
  CHECK(fwrite(original,1,j.tokens[vector].begin,f)==j.tokens[vector].begin);CHECK(fputs("[0,1]",f)>=0);CHECK(fputs(original+j.tokens[vector].end,f)>=0);CHECK(!fclose(f));
  replace_receipt(db,key,changed);body=call(cfg,request,"corrupt_embedding");free(body);free(changed);
  replace_receipt(db,key,original);CHECK(sqlite3_close(db)==SQLITE_OK);free(key);free(original);
  SourcesConfig retired=*cfg;retired.embedding=NULL;retired.chutni_semantic_profile=NULL;
  body=call(&retired,request,NULL);CHECK(strstr(body,"\"phase\":\"published\""));free(body);
}
static int timed_stop(void *context){
  struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 1;
  return (double)now.tv_sec+(double)now.tv_nsec/1e9>=*(double *)context;
}
static void indexed_embedding(const SourcesConfig *base,Fixture *fixture,const char *root){
  char files[512],path[512],state[512];
  CHECK(snprintf(files,sizeof(files),"%s/files",root)<(int)sizeof(files));CHECK(!mkdir(files,0700));
  CHECK(snprintf(path,sizeof(path),"%s/article.txt",files)<(int)sizeof(path));
  CHECK(snprintf(state,sizeof(state),"%s/index-state",root)<(int)sizeof(state));
  FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs("Indexed document about recovery and durable publication.",f)>=0);CHECK(!fclose(f));
  SourcesConfig cfg=*base;cfg.root=files;cfg.state=state;cfg.chutni_publish=1;cfg.discovery_interval_ms=100;
  fixture->mode=0;unsigned before=fixture->calls;
  char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"index-doc\"}",NULL);
  char *job=field(body,"job_id");free(body);
  char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"index-read\",\"max_items\":1}",job);
  body=call(&cfg,request,NULL);CHECK(strstr(body,"\"indexed_items\":1"));free(body);
  body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"processed_items\":0"));free(body);
  body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_publish_queue_step\"}",NULL);if(!strstr(body,"\"published_items\":1"))fprintf(stderr,"publication: %s",body);CHECK(strstr(body,"\"published_items\":1"));free(body);
  body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\",\"operation_id\":\"capture-doc\",\"timeout_ms\":60000}",NULL);
  CHECK(strstr(body,"\"phase\":\"captured\"")&&fixture->calls==before+1);char *capture=strdup(body);free(body);
  SourcesEmbeddingConfig offline_embedding=*cfg.embedding;offline_embedding.inference.call=NULL;SourcesConfig offline=cfg;offline.embedding=&offline_embedding;
  body=call(&offline,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\",\"operation_id\":\"capture-doc\",\"timeout_ms\":60000}",NULL);CHECK(!strcmp(capture,body)&&fixture->calls==before+1);free(body);free(capture);
  interrupted_embedding(&offline,root,"SAMOSA_EMBEDDING_POST_SDK_MARKER");
  body=call(&offline,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\",\"operation_id\":\"publish-vector\",\"timeout_ms\":60000}",NULL);CHECK(strstr(body,"\"phase\":\"published\"")&&fixture->calls==before+1);free(body);
  body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"processed_items\":0")&&fixture->calls==before+1);free(body);
  snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_embedding_status\",\"job_id\":\"%s\"}",job);
  body=call(&offline,request,NULL);CHECK(strstr(body,"\"phase\":\"published\"")&&strstr(body,"representation_id"));
  Json status;CHECK(json_parse(&status,body,strlen(body)));int items=json_get(&status,0,"items");CHECK(items>=0&&status.tokens[items].count==1);
  char *artifact=json_string(&status,json_get(&status,items+1,"artifact_id"));CHECK(artifact);free(body);
  SourcesConfig other=cfg;other.owner="bob";body=call(&other,request,"unknown_index");free(body);
  body=call(&other,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"processed_items\":0")&&fixture->calls==before+1);free(body);
  body=call(&cfg,"{\"contract_version\":1,\"op\":\"memory_search_text\",\"query\":\"How can I recover durable work?\"}",NULL);CHECK(strstr(body,artifact));free(body);free(artifact);
  f=fopen(path,"w");CHECK(f);CHECK(fputs("A new indexed version about recovery, retry and publication.",f)>=0);CHECK(!fclose(f));
  body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",NULL);free(body);
  snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"reindex-doc\",\"max_items\":1}",job);
  body=call(&cfg,request,NULL);free(body);
  body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_publish_queue_step\"}",NULL);if(!strstr(body,"\"published_items\":1"))fprintf(stderr,"publication: %s",body);CHECK(strstr(body,"\"published_items\":1"));free(body);
  interrupted_embedding(&cfg,root,"SAMOSA_EMBEDDING_POST_CAPTURE_MARKER");
  body=call(&offline,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"phase\":\"published\""));free(body);
  snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_embedding_status\",\"job_id\":\"%s\"}",job);
  body=call(&offline,request,NULL);CHECK(json_parse(&status,body,strlen(body)));items=json_get(&status,0,"items");CHECK(items>=0&&status.tokens[items].count==2);free(body);
  const char *migrated_profile="{\"representation_kind\":\"text_embedding\",\"model_id\":\"fixture/embed\",\"model_revision\":\"migration\",\"dimensions\":2,\"dtype\":\"f32\",\"normalization\":\"l2\",\"tokenizer_hash\":null,\"projector_hash\":null}";
  SourcesEmbeddingConfig migrated_embedding=*cfg.embedding;migrated_embedding.recipe="mean-l2-migration-v1";
  SourcesConfig migrated=cfg;migrated.embedding=&migrated_embedding;migrated.chutni_semantic_profile=migrated_profile;
  fixture->embedding=&migrated_embedding;fixture->profile=migrated_profile;unsigned migration_calls=fixture->calls;
  body=call(&migrated,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"phase\":\"captured\"")&&strstr(body,"\"generation\":1")&&fixture->calls==migration_calls+1);free(body);
  SourcesEmbeddingConfig migrated_offline=migrated_embedding;migrated_offline.inference.call=NULL;SourcesConfig migrated_pub=migrated;migrated_pub.embedding=&migrated_offline;
  body=call(&migrated_pub,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"phase\":\"published\"")&&fixture->calls==migration_calls+1);free(body);
  body=call(&offline,request,NULL);CHECK(json_parse(&status,body,strlen(body)));items=json_get(&status,0,"items");CHECK(items>=0&&status.tokens[items].count==3);free(body);
  fixture->embedding=(SourcesEmbeddingConfig *)cfg.embedding;fixture->profile=NULL;free(job);
  SourcesConfig runtime=cfg;char runtime_state[512];CHECK(snprintf(runtime_state,sizeof(runtime_state),"%s/runtime-state",root)<(int)sizeof(runtime_state));runtime.state=runtime_state;runtime.discovery_interval_ms=0;
  body=call(&runtime,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"runtime-index\"}",NULL);job=field(body,"job_id");free(body);
  snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"runtime-read\",\"max_items\":1}",job);body=call(&runtime,request,NULL);free(body);
  struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));double until=(double)now.tv_sec+(double)now.tv_nsec/1e9+20;runtime.cancelled=timed_stop;runtime.cancellation=&until;
  FILE *output=tmpfile();CHECK(output);CHECK(!sources_queue_run(&runtime,100,output));CHECK(!fclose(output));runtime.cancelled=NULL;runtime.cancellation=NULL;
  snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_embedding_status\",\"job_id\":\"%s\"}",job);body=call(&runtime,request,NULL);CHECK(strstr(body,"\"phase\":\"published\""));free(body);archived_corruption(&runtime,request);free(job);
}
static void embedding_bounds(const SourcesConfig *base,Fixture *fixture,const char *root){
  for(int capacity=0;capacity<2;capacity++){
    char files[512],state[512],path[512],request[512],db_path[512];
    CHECK(snprintf(files,sizeof(files),"%s/bounds-%d",root,capacity)<(int)sizeof(files));CHECK(!mkdir(files,0700));
    CHECK(snprintf(state,sizeof(state),"%s/bounds-state-%d",root,capacity)<(int)sizeof(state));
    size_t count=capacity?60:2;
    for(size_t k=0;k<count;k++){
      CHECK(snprintf(path,sizeof(path),"%s/%02zu.txt",files,k)<(int)sizeof(path));FILE *f=fopen(path,"w");CHECK(f);
      size_t bytes=!capacity&&!k?60000:60001;char *text=malloc(bytes);CHECK(text);memset(text,!capacity&&!k?'"':'x',bytes);
      CHECK(fwrite(text,1,bytes,f)==bytes);free(text);CHECK(!fclose(f));
    }
    SourcesConfig cfg=*base;cfg.root=files;cfg.state=state;cfg.chutni_publish=1;
    unsigned before=fixture->calls;char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"bounds-create\"}",NULL);
    char *job=field(body,"job_id");free(body);
    for(size_t k=0;k<(count+3)/4;k++){
      snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"bounds-read-%zu\",\"max_items\":4}",job,k);
      body=call(&cfg,request,NULL);free(body);
    }
    for(size_t k=0;k<count;k++){body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_publish_queue_step\"}",NULL);CHECK(strstr(body,"\"published_items\":1"));free(body);}
    if(!capacity){
      body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"phase\":\"captured\"")&&fixture->calls==before+1);free(body);
      SourcesEmbeddingConfig off=*cfg.embedding;off.inference.call=NULL;SourcesConfig pub=cfg;pub.embedding=&off;
      body=call(&pub,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",NULL);CHECK(strstr(body,"\"phase\":\"published\""));free(body);
      body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}","embedding_input_limit");free(body);CHECK(fixture->calls==before+1);
    }else{
      CHECK(snprintf(db_path,sizeof(db_path),"%s/sources.sqlite3",state)<(int)sizeof(db_path));sqlite3 *db=NULL;CHECK(sqlite3_open(db_path,&db)==SQLITE_OK);CHECK(sqlite3_busy_timeout(db,5000)==SQLITE_OK);
      for(size_t k=0;k<59;k++){
        body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}","embedding_input_limit");free(body);CHECK(fixture->calls==before);
        CHECK(sqlite3_exec(db,"UPDATE index_embeddings SET next_ms=CAST(strftime('%s','now') AS INTEGER)*1000+60000",NULL,NULL,NULL)==SQLITE_OK);
      }
      body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}","embedding_capacity");free(body);CHECK(fixture->calls==before);
      sqlite3_stmt *q=NULL;CHECK(sqlite3_prepare_v2(db,"SELECT count(*),sum(CASE status WHEN 'pending' THEN 140000 ELSE 0 END) FROM index_embeddings",-1,&q,NULL)==SQLITE_OK);CHECK(sqlite3_step(q)==SQLITE_ROW);CHECK(sqlite3_column_int64(q,0)==59&&sqlite3_column_int64(q,1)==8260000);CHECK(sqlite3_finalize(q)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    }
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_embedding_status\",\"job_id\":\"%s\"}",job);body=call(&cfg,request,NULL);CHECK(strstr(body,"embedding_input_limit"));free(body);free(job);
  }
}
int main(int argc, char **argv) {
  CHECK(argc == 3);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(library);
  int (*create)(const char *, const char *, void **) = NULL;
  int (*sdk)(void *, const char *, const char *, char **) = NULL;
  void (*release)(void *) = NULL;
  void (*close_store)(void *) = NULL;
  symbol(library, "chutni_create", &create, sizeof(create));
  symbol(library, "chutni_call", &sdk, sizeof(sdk));
  symbol(library, "chutni_free", &release, sizeof(release));
  symbol(library, "chutni_close", &close_store, sizeof(close_store));
  char root[] = "/tmp/samosa-text-query-XXXXXX";
  CHECK(mkdtemp(root));
  char store_path[512], state[512];
  snprintf(store_path, sizeof(store_path), "%s/memory.chutni", root);
  snprintf(state, sizeof(state), "%s/state", root);
  void *store = NULL;
  CHECK(!create(store_path, "Sources text query fixture", &store));
  char *first = NULL;
  const char *vectors[] = {"[0.8,0.6]", "[0,1]", "[-1,0]", "[0,-1]"};
  for (size_t k = 0; k < 4; k++) {
    char request[2048], *reply = NULL;
    snprintf(request, sizeof(request),
             "{\"memory_kind\":\"note\",\"text\":\"Evidence document "
             "%zu\",\"producer\":{\"producer_kind\":\"human\",\"name\":"
             "\"fixture\"},\"operation\":\"test-text-query\"}",
             k);
    CHECK(!sdk(store, "put_memory", request, &reply));
    char *artifact = field(reply, "artifact_id");
    if (!k)
      first = strdup(artifact);
    release(reply);
    reply = NULL;
    snprintf(request, sizeof(request),
             "{\"artifact_id\":\"%s\",\"profile\":%s,\"vector\":%s}", artifact,
             profile, vectors[k]);
    CHECK(!sdk(store, "put_representation", request, &reply));
    release(reply);
    free(artifact);
  }
  char *info = NULL;
  CHECK(!sdk(store, "store_info", "{}", &info));
  char *id = field(info, "store_id");
  release(info);
  close_store(store);
  SourcesEmbeddingConfig e = {.inference = {.provider_id = "fixture-embedding",
                                            .engine_sha256 = pin,
                                            .model_sha256 = pin,
                                            .call = embedding},
                              .recipe = "mean-l2-fixture-v1",
                              .query_prefix = "query: ",
                              .document_prefix = "document: ",
                              .context_tokens = 8192};
  Fixture fixture = {.embedding = &e};
  e.inference.context = &fixture;
  SourcesConfig cfg = {.state = state,
                       .owner = "alice",
                       .embedding = &e,
                       .chutni_semantic_profile = profile,
                       .chutni_library = argv[1],
                       .chutni_store = store_path,
                       .chutni_store_id = id,
                       .chutni_helper = argv[2]};
  vector_worker(&cfg,first);
  const char *request =
      "{\"contract_version\":1,\"op\":\"memory_search_text\",\"query\":\"How "
      "can I recover my "
      "work?\",\"operation_id\":\"text-1\",\"timeout_ms\":10000}";
  char *accepted = call(&cfg, request, NULL);
  CHECK(fixture.calls == 1 && fixture.deadline > 0);
  CHECK(strstr(accepted, "How can I recover my work?") &&
        strstr(accepted, "query_embedding") &&
        strstr(accepted, "cosine_bruteforce"));
  Json j;
  CHECK(json_parse(&j, accepted, strlen(accepted)));
  int memory = json_get(&j, 0, "memory"),
      result = json_get(&j, memory, "result"),
      items = json_get(&j, result, "items");
  CHECK(j.tokens[items].count == 4);
  char *top = json_string(&j, json_get(&j, items + 1, "external_artifact_id"));
  CHECK(top && !strcmp(top, first));
  free(top);
  unsigned calls = fixture.calls;
  e.inference.call = NULL;
  SourcesConfig offline = cfg;
  offline.chutni_library = NULL;
  offline.chutni_helper = NULL;
  char *reply = call(&offline, request, NULL);
  CHECK(!strcmp(reply, accepted) && fixture.calls == calls);
  free(reply);
  e.inference.call = embedding;
  e.query_prefix = "changed: ";
  reply = call(&cfg, request, "idempotency_conflict");
  CHECK(fixture.calls == calls);
  free(reply);
  e.query_prefix = "query: ";
  SourcesEmbeddingConfig other = e;
  other.inference.call = NULL;
  offline.embedding = &other;
  offline.owner = "bob";
  reply = call(&offline, request, "inference_unavailable");
  free(reply);
  for (unsigned mode = 1; mode <= 22; mode++) {
    fixture.mode = mode;
    const char *error = mode == 11   ? "inference_profile_mismatch"
                        : mode == 20 ? "cancelled"
                        : mode == 21 ? "inference_unavailable"
                        : mode == 22 ? "inference_busy"
                                     : "inference_response_invalid";
    reply = call(&cfg,
                 "{\"contract_version\":1,\"op\":\"memory_search_text\","
                 "\"query\":\"Validation query\"}",
                 error);
    free(reply);
  }
  fixture.mode = 0;
  calls = fixture.calls;
  SourcesConfig refused = cfg;
  refused.cancelled = cancelled;
  reply = call(&refused,
               "{\"contract_version\":1,\"op\":\"memory_search_text\","
               "\"query\":\"Control query\"}",
               "cancelled");
  free(reply);
  CHECK(fixture.calls == calls);
  refused = cfg;
  struct timespec now;
  CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
  refused.deadline_monotonic =
      (double)now.tv_sec + (double)now.tv_nsec / 1e9 - 1;
  reply = call(&refused,
               "{\"contract_version\":1,\"op\":\"memory_search_text\","
               "\"query\":\"Control query\"}",
               "deadline_exceeded");
  free(reply);
  CHECK(fixture.calls == calls);
  const char *invalid[] = {
      "{\"contract_version\":1,\"op\":\"memory_search_text\",\"query\":\"\"}",
      "{\"contract_version\":1,\"op\":\"memory_search_text\",\"query\":12}",
      "{\"contract_version\":1,\"op\":\"memory_search_text\",\"query\":\"q\","
      "\"model_id\":\"override\"}",
      "{\"contract_version\":1,\"op\":\"memory_search_text\",\"query\":\"q\","
      "\"op|contract_version\":1}"};
  for (size_t k = 0; k < 4; k++) {
    reply = call(&cfg, invalid[k], k < 2 ? "invalid_request" : "unknown_field");
    free(reply);
  }
  CHECK(fixture.calls == calls);
  char *response = NULL;
  size_t response_bytes = 0;
  CHECK(!sources_embedding_generate(&cfg, "Document body", "document",
                                    "Document title", &response,
                                    &response_bytes));
  CHECK(response_bytes && strstr(response, "Document title"));
  unsigned capture_calls=fixture.calls;
  SourcesEmbeddingConfig saved_embedding=e;saved_embedding.inference.call=NULL;
  SourcesConfig saved_cfg=cfg;saved_cfg.embedding=&saved_embedding;
  char *saved_arguments=NULL;
  CHECK(!sources_embedding_capture_arguments(&saved_cfg,"Document body","Document title",response,response_bytes,&saved_arguments));
  CHECK(saved_arguments&&fixture.calls==capture_calls);free(saved_arguments);saved_arguments=NULL;
  CHECK(sources_embedding_capture_arguments(&saved_cfg,"Changed body","Document title",response,response_bytes,&saved_arguments));
  CHECK(!saved_arguments&&fixture.calls==capture_calls);
  CHECK(sources_embedding_capture_arguments(&saved_cfg,"Document body",NULL,response,response_bytes,&saved_arguments));
  saved_embedding.recipe="changed-recipe";
  CHECK(sources_embedding_capture_arguments(&saved_cfg,"Document body","Document title",response,response_bytes,&saved_arguments));
  CHECK(!saved_arguments&&fixture.calls==capture_calls);
  free(response);
  SourcesEmbeddingConfig parsed;
  const char *configuration =
      "{\"provider_id\":\"fixture-embedding\",\"engine_sha256\":"
      "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
      "\"model_sha256\":"
      "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
      "\"recipe\":\"mean-l2-fixture-v1\",\"query_prefix\":\"query: "
      "\",\"document_prefix\":\"document: \",\"context_tokens\":8192}";
  CHECK(sources_embedding_config_parse(configuration, strlen(configuration),
                                       &parsed));
  SourcesConfig parsed_cfg = cfg;
  parsed_cfg.embedding = &parsed;
  char a[65], b[65];
  CHECK(!sources_embedding_identity(&cfg, a) &&
        !sources_embedding_identity(&parsed_cfg, b) && !strcmp(a, b));
  sources_embedding_config_free(&parsed);
  CHECK(!sources_embedding_config_parse("{}", 2, &parsed));
  CHECK(!sources_embedding_config_parse("[]", 2, &parsed));
  indexed_embedding(&cfg,&fixture,root);
  embedding_bounds(&cfg,&fixture,root);
  free(first);
  free(id);
  free(accepted);
  CHECK(!dlclose(library));
  printf("PASS Sources text embedding/query: %u checks; actual SDK ranking, "
         "validated fixture embeddings, profile/model/input refusal, controls, "
         "owner and offline replay; no actual-model quality claim\n",
         checks);
  return 0;
}
