#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "summary.h"
#include "json.h"
#include "sha256.h"
#include <sqlite3.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *call(const SourcesConfig *cfg,const char *request,const char *error){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);int rc=sources_dispatch(cfg,request,strlen(request),out);CHECK(!fclose(out));if(rc!=(error?1:0))fprintf(stderr,"request %s response %s\n",request,body);CHECK(rc==(error?1:0));if(error){Json j;CHECK(json_parse(&j,body,n));char *code=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));if(!code||strcmp(code,error))fprintf(stderr,"request %s response %s\n",request,body);CHECK(code&&!strcmp(code,error));free(code);}return body;}
static char *field(const char *body,const char *name){Json j;CHECK(json_parse(&j,body,strlen(body)));char *text=json_string(&j,json_get(&j,0,name));CHECK(text);return text;}
typedef struct { SourcesInferenceConfig *config;SourcesConfig *sources;const char *mode;size_t calls,sampled; } Fixture;
static const char *transport(void *context,const SourcesInferenceControl *control,const char *input,size_t bytes,char **response,size_t *length){
    Fixture *f=context;f->calls++;CHECK(control->deadline_monotonic>0);
    Json j;CHECK(json_parse(&j,input,bytes));CHECK(j.tokens[0].count==6);
    char *op=json_string(&j,json_get(&j,0,"op")),*text=json_string(&j,json_get(&j,0,"text"));CHECK(op&&text&&*text);int opening=!strcmp(op,"summarize_opening");CHECK(opening||!strcmp(op,"summarize"));free(op);
    f->sampled=strlen(text);CHECK(f->sampled<=(opening?60000u:1400u));char digest[65];hash_bytes(text,f->sampled,digest);free(text);
    if(opening){FILE *out=open_memstream(response,length);CHECK(out);fprintf(out,"{\"contract_version\":1,\"provider_id\":\"%s\",\"kind\":\"derived\",\"artifact_type\":\"summary\",\"input_sha256\":\"%s\",\"input_bytes\":%zu,\"text\":\"bounded summary\",\"coverage\":{\"status\":\"partial\",\"reason\":\"native_partitioning_and_generated_summary_uncertain\"},\"provenance\":{\"engine\":\"samosa-native-t5\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"recipe\":\"samosa-native-summary-opening-map-reduce-v1\",\"max_output_tokens\":128,\"encoder_context_tokens\":512,\"processed_input_tokens\":null,\"chunk_max_output_tokens\":64,\"input_partitioning\":\"native_token_chunks\",\"map_output_bytes\":15,\"reduction_rounds\":0,\"max_output_bytes\":1800,\"output_truncated\":false}}",f->config->provider_id,digest,f->sampled,f->config->engine_sha256,f->config->model_sha256);CHECK(!fclose(out));return NULL;}
    if(!strcmp(f->mode,"unavailable"))return "transport_error";
    if(!strcmp(f->mode,"deadline"))return "deadline_exceeded";
    if(!strcmp(f->mode,"cancel")){char *body=call(f->sources,"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"cancel-summary\"}",NULL);free(body);CHECK(control->cancelled&&control->cancelled(control->cancellation));return "cancelled";}
    if(!strcmp(f->mode,"busy")){*response=strdup("{\"contract_version\":1,\"error\":{\"code\":\"busy\"}}");CHECK(*response);*length=strlen(*response);return NULL;}
    if(!strcmp(f->mode,"wrong-input"))memset(digest,'0',64);
    FILE *out=open_memstream(response,length);CHECK(out);
    fprintf(out,"{\"contract_version\":1,\"provider_id\":\"%s\",\"kind\":\"%s\",\"artifact_type\":\"summary\",\"input_sha256\":\"%s\",\"input_bytes\":%zu,\"text\":\"bounded summary\",\"coverage\":{\"status\":\"%s\",\"reason\":\"bounded_encoder_and_generated_summary_uncertain\"},\"provenance\":{\"engine\":\"samosa-native-t5\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"recipe\":\"%s\",\"max_output_tokens\":128,\"encoder_context_tokens\":512,\"processed_input_tokens\":%s}}",f->config->provider_id,!strcmp(f->mode,"literal")?"literal":"derived",digest,f->sampled,!strcmp(f->mode,"complete")?"complete":"partial",f->config->engine_sha256,!strcmp(f->mode,"wrong-model")?f->config->engine_sha256:f->config->model_sha256,!strcmp(f->mode,"wrong-recipe")?"wrong":"samosa-native-summary-scalar-v1",!strcmp(f->mode,"consumed")?"512":"null");CHECK(!fclose(out));return NULL;
}
static char *request(const char *source,const char *evidence,const char *id){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);fprintf(f,"{\"contract_version\":1,\"op\":\"summarize_evidence\",\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"",source,source,evidence);if(id)fprintf(f,",\"operation_id\":\"%s\"",id);fputc('}',f);CHECK(!fclose(f));return out;}
static int remove_file(const char *path,const struct stat *s,int type,struct FTW *walk){(void)s;(void)type;(void)walk;return remove(path);}
int main(int argc,char **argv){CHECK(argc==1||argc==3);
    char root[]="/tmp/samosa-source-summary-XXXXXX";CHECK(mkdtemp(root));char state[512],path[512],database[600];snprintf(state,sizeof(state),"%s/state",root);snprintf(path,sizeof(path),"%s/input.txt",root);snprintf(database,sizeof(database),"%s/sources.sqlite3",state);
    char content[1602];memset(content,'a',sizeof(content));content[1399]=(char)0xc3;content[1400]=(char)0xa9;content[1601]=0;
    FILE *f=fopen(path,"w");CHECK(f);CHECK(fwrite(content,1,1601,f)==1601);CHECK(!fclose(f));
    SourcesConfig cfg={.root=root,.state=state,.owner="alice"};
    SourcesInferenceConfig inference={.provider_id="native-t5-summary-v1",.engine_sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",.model_sha256="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};
    Fixture fixture={.config=&inference,.sources=&cfg,.mode="good"};inference.call=transport;inference.context=&fixture;cfg.summary_inference=&inference;
    char reg[1024];snprintf(reg,sizeof(reg),"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"%s\"}","input.txt");char *body=call(&cfg,reg,NULL),*source=field(body,"source_id");free(body);
    char read[1024];snprintf(read,sizeof(read),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":1601}",source,source);body=call(&cfg,read,NULL);char *evidence=field(body,"evidence_id");free(body);
    char *req=request(source,evidence,NULL);SourcesConfig limited=cfg;limited.cache_limit=1;body=call(&limited,req,"cache_limit");free(body);CHECK(!fixture.calls);
    SourcesConfig bob=cfg;bob.owner="bob";body=call(&bob,req,"unknown_source");free(body);
    char empty_path[512];snprintf(empty_path,sizeof(empty_path),"%s/empty.txt",root);f=fopen(empty_path,"w");CHECK(f);CHECK(!fclose(f));
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"empty.txt\"}",NULL);char *empty_source=field(body,"source_id");free(body);
    char empty_read[512];snprintf(empty_read,sizeof(empty_read),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":0}",empty_source,empty_source);body=call(&cfg,empty_read,NULL);char *empty_evidence=field(body,"evidence_id");free(body);char *empty_req=request(empty_source,empty_evidence,NULL);body=call(&cfg,empty_req,"empty_evidence");free(body);free(empty_source);free(empty_evidence);free(empty_req);CHECK(!fixture.calls);
    
const char *bad_modes[]={"wrong-input","literal","complete","wrong-model","wrong-recipe","consumed","busy","unavailable","deadline"};
    for(size_t i=0;i<sizeof(bad_modes)/sizeof(bad_modes[0]);i++){fixture.mode=bad_modes[i];body=call(&cfg,req,i<6?"inference_response_invalid":i==6?"inference_busy":i==7?"inference_unavailable":"deadline_exceeded");free(body);CHECK(fixture.sampled==1399);}
    fixture.mode="cancel";char *controlled=request(source,evidence,"cancel-summary");body=call(&cfg,controlled,"cancelled");free(body);free(controlled);
    fixture.mode="good";controlled=request(source,evidence,"summary-1");body=call(&cfg,controlled,NULL);CHECK(strstr(body,"opening_sample_and_generated_summary_uncertain")&&strstr(body,"sampled_bytes\":1399")&&strstr(body,"input_evidence_id"));char *artifact=field(body,"artifact_id");char *saved=strdup(body);CHECK(saved);free(body);size_t calls=fixture.calls;
    body=call(&cfg,req,NULL);CHECK(!sources_summary_capture_validate(&cfg,body,strlen(body)));free(body);
    char opening_req[512];snprintf(opening_req,sizeof(opening_req),"{\"contract_version\":1,\"op\":\"summarize_evidence_opening\",\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"}",source,source,evidence);
    body=call(&cfg,opening_req,NULL);CHECK(fixture.sampled==1601);CHECK(strstr(body,"native_token_chunks"));CHECK(!sources_summary_capture_validate(&cfg,body,strlen(body)));char *opening_saved=strdup(body);CHECK(opening_saved);free(body);
    char *tampered=strdup(opening_saved);CHECK(tampered);char *at=strstr(tampered,"\"map_output_bytes\":15");CHECK(at);at[strlen("\"map_output_bytes\":1")]='6';CHECK(sources_summary_capture_validate(&cfg,tampered,strlen(tampered)));free(tampered);
    size_t opening_calls=fixture.calls;inference.call=NULL;body=call(&cfg,opening_req,NULL);CHECK(!strcmp(body,opening_saved)&&fixture.calls==opening_calls);free(body);free(opening_saved);inference.call=transport;
    if(argc==3){
        void *library=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);CHECK(library);int (*create)(const char*,const char*,void**)=NULL;int (*sdkcall)(void*,const char*,const char*,char**)=NULL;void (*release)(void*)=NULL;void (*close_store)(void*)=NULL;
#define SYMBOL(name,target) do{void *symbol=dlsym(library,name);CHECK(symbol&&sizeof(symbol)==sizeof(target));memcpy(&target,&symbol,sizeof(target));}while(0)
        SYMBOL("chutni_create",create);SYMBOL("chutni_call",sdkcall);SYMBOL("chutni_free",release);SYMBOL("chutni_close",close_store);
        char store_path[512];snprintf(store_path,sizeof(store_path),"%s/summary.chutni",root);void *store=NULL;CHECK(!create(store_path,"Summary publication fixture",&store));char *info=NULL;CHECK(!sdkcall(store,"store_info","{}",&info));char *store_id=field(info,"store_id");release(info);close_store(store);
        cfg.chutni_publish=1;cfg.chutni_helper=argv[1];cfg.chutni_library=argv[2];cfg.chutni_store=store_path;cfg.chutni_store_id=store_id;
        char publish[512];snprintf(publish,sizeof(publish),"{\"contract_version\":1,\"op\":\"publish_summary\",\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"}",source,source,evidence);
        body=call(&cfg,publish,NULL);CHECK(strstr(body,"\"receipt_state\":\"committed\"")&&fixture.calls==opening_calls);char *receipt=strdup(body);CHECK(receipt);free(body);
        SourcesConfig without_helper=cfg;without_helper.chutni_helper="/missing/worker";body=call(&without_helper,publish,NULL);CHECK(!strcmp(body,receipt));free(body);free(receipt);
        CHECK(!unlink(path));sqlite3 *publication_db=NULL;CHECK(sqlite3_open(database,&publication_db)==SQLITE_OK);CHECK(sqlite3_exec(publication_db,"DELETE FROM transcript_publications",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(publication_db)==SQLITE_OK);
        body=call(&cfg,publish,NULL);CHECK(strstr(body,"\"recovered\":true")&&fixture.calls==opening_calls);free(body);
        free(store_id);CHECK(!dlclose(library));
        f=fopen(path,"w");CHECK(f&&fwrite(content,1,1601,f)==1601&&!fclose(f));
    }
    calls=fixture.calls;
    CHECK(!unlink(path));inference.call=NULL;body=call(&cfg,req,NULL);CHECK(strstr(body,artifact));free(body);CHECK(fixture.calls==calls);
    SourcesConfig offline=cfg;offline.summary_inference=NULL;body=call(&offline,controlled,NULL);CHECK(strstr(body,artifact));free(body);
    const char *old_model=inference.model_sha256;inference.model_sha256="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";body=call(&cfg,req,"inference_unavailable");free(body);inference.call=transport;body=call(&cfg,req,NULL);char *new_artifact=field(body,"artifact_id");CHECK(strcmp(new_artifact,artifact));free(new_artifact);free(body);inference.model_sha256=old_model;
    sqlite3 *db=NULL;CHECK(sqlite3_open(database,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE snapshots SET bytes=x'00'",NULL,NULL,NULL)==SQLITE_OK);body=call(&cfg,req,"corrupt_snapshot");free(body);
    sqlite3_stmt *stmt=NULL;CHECK(sqlite3_prepare_v2(db,"UPDATE snapshots SET bytes=?",-1,&stmt,NULL)==SQLITE_OK);CHECK(sqlite3_bind_blob(stmt,1,content,1601,SQLITE_TRANSIENT)==SQLITE_OK);CHECK(sqlite3_step(stmt)==SQLITE_DONE);sqlite3_finalize(stmt);
    CHECK(sqlite3_exec(db,"UPDATE prepared_artifacts SET digest='wrong'",NULL,NULL,NULL)==SQLITE_OK);body=call(&cfg,req,"corrupt_cache");free(body);
    CHECK(sqlite3_exec(db,"DELETE FROM prepared_artifacts;UPDATE evidence_cache SET digest='wrong'",NULL,NULL,NULL)==SQLITE_OK);body=call(&cfg,req,"corrupt_cache");free(body);CHECK(sqlite3_close(db)==SQLITE_OK);
    free(source);free(evidence);free(req);free(controlled);free(artifact);free(saved);CHECK(!nftw(root,remove_file,32,FTW_DEPTH|FTW_PHYS));
    printf("PASS Sources public summaries: %u checks; bounded UTF-8 sample, pins, provenance, owner/cache/replay, controls and corruption\n",checks);return 0;
}
