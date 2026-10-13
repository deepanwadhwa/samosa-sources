#include "visual_fixture.h"
#include "chutni.h"
#include <dlfcn.h>

static void symbol(void *library, const char *name, void *target, size_t n) {
    void *p=dlsym(library,name);CHECK(p&&n==sizeof(p));memcpy(target,&p,n);
}
static char *publication_request(const char *source,const char *prompt,const char *operation) {
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);
    fprintf(out,"{\"contract_version\":1,\"op\":\"publish_visual\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":1000,\"end_ms\":1500",source,source);
    if(prompt){fputs(",\"prompt\":",out);json_quote(out,prompt,strlen(prompt));}
    if(operation){fputs(",\"operation_id\":",out);json_quote(out,operation,strlen(operation));}
    fputc('}',out);CHECK(!fclose(out));return body;
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"--sources-native-worker"))return sources_native_worker_main(argc,argv);
    CHECK(argc==5);
    void *library=dlopen(argv[4],RTLD_NOW|RTLD_LOCAL);CHECK(library);
    int (*create)(const char *,const char *,void **)=NULL;
    int (*open_store)(const char *,int,void **)=NULL;
    int (*sdk_call)(void *,const char *,const char *,char **)=NULL;
    void (*sdk_free)(void *)=NULL;void (*close_store)(void *)=NULL;
    symbol(library,"chutni_create",&create,sizeof(create));
    symbol(library,"chutni_open",&open_store,sizeof(open_store));
    symbol(library,"chutni_call",&sdk_call,sizeof(sdk_call));
    symbol(library,"chutni_free",&sdk_free,sizeof(sdk_free));
    symbol(library,"chutni_close",&close_store,sizeof(close_store));
    char temp[]="/tmp/samosa-visual-publication-XXXXXX";CHECK(mkdtemp(temp));
    char *root=realpath(temp,NULL);CHECK(root);
    char state[512],path[512],store_path[512],database[600];
    snprintf(state,sizeof(state),"%s/state",root);snprintf(path,sizeof(path),"%s/video.mp4",root);
    snprintf(store_path,sizeof(store_path),"%s/memory.chutni",root);
    snprintf(database,sizeof(database),"%s/sources.sqlite3",state);
    FILE *from=fopen(argv[2],"rb"),*to=fopen(path,"wb");CHECK(from&&to);
    unsigned char buffer[65536];size_t n;
    while((n=fread(buffer,1,sizeof(buffer),from)))CHECK(fwrite(buffer,1,n,to)==n);
    CHECK(!ferror(from)&&!fclose(from)&&!fclose(to));
    void *store=NULL;CHECK(!create(store_path,"Visual publication native gate",&store));
    char *info=NULL;CHECK(!sdk_call(store,"store_info","{}",&info));
    char *store_id=field(info,"store_id");sdk_free(info);close_store(store);
    char engine[65],model[65];memset(engine,'a',64);memset(model,'b',64);engine[64]=model[64]=0;
    Fixture fixture={.mode="ok"};
    SourcesInferenceConfig inference={.provider_id="fixture-vision-v1",.engine_sha256=engine,
        .model_sha256=model,.call=transport,.context=&fixture};
    SourcesConfig cfg={.root=root,.state=state,.owner="alice",.media_helper=argv[1],
        .visual_inference=&inference,.chutni_helper=argv[3],.chutni_library=argv[4],
        .chutni_store=store_path,.chutni_store_id=store_id,.chutni_publish=1};
    fixture.sources=&cfg;fixture.inference=&inference;
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"video.mp4\"}",NULL);
    char *source=field(body,"source_id");free(body);
    char *publish=publication_request(source,NULL,NULL);
    body=call(&cfg,publish,"inference_unavailable");free(body);CHECK(fixture.calls==0);
    char *req=request(source,1000,1500,NULL,NULL),*capture=call(&cfg,req,NULL);free(req);
    CHECK(fixture.calls==1);inference.call=NULL;
    body=call(&cfg,publish,NULL);CHECK(strstr(body,"\"receipt_state\":\"committed\"")&&!strstr(body,"evidence_id"));
    Json receipt;CHECK(json_parse(&receipt,body,strlen(body)));int p=json_get(&receipt,0,"publication");
    char *external_source=json_string(&receipt,json_get(&receipt,p,"external_source_id")),
         *external_artifact=json_string(&receipt,json_get(&receipt,p,"external_artifact_id"));
    CHECK(external_source&&external_artifact);char *first=strdup(body);CHECK(first);free(body);
    body=call(&cfg,publish,NULL);CHECK(!strcmp(body,first)&&fixture.calls==1);free(body);free(first);
    char *tracked=publication_request(source,NULL,"published-visual");
    char *tracked_result=call(&cfg,tracked,NULL);
    CHECK(strstr(tracked_result,"published-visual")&&fixture.calls==1);
    body=call(&cfg,tracked,NULL);CHECK(!strcmp(body,tracked_result));free(body);
    /* Inspect actual public SDK output, not only the Sources receipt. */
    CHECK(!open_store(store_path,1,&store));char args[512];
    snprintf(args,sizeof(args),"{\"source_id\":\"%s\",\"artifact_id\":\"%s\",\"include_stale\":true}",external_source,external_artifact);
    char *context=NULL;CHECK(!sdk_call(store,"source_context",args,&context));
    CHECK(strstr(context,"image_caption")&&strstr(context,"model_generated")&&strstr(context,"samosa-sources-visual-v1"));
    CHECK(strstr(context,"source_time_us")&&strstr(context,"1000001")&&strstr(context,"selected_frame_only")&&strstr(context,"visual_analysis_uncertain"));
    CHECK(!strstr(context,"\"artifact_kind\":\"transcript\""));sdk_free(context);close_store(store);
    /* SDK commit recovery uses the same artifact, without a model call. */
    sqlite3 *db=NULL;CHECK(sqlite3_open(database,&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"DELETE FROM transcript_publications",NULL,NULL,NULL)==SQLITE_OK);
    CHECK(sqlite3_close(db)==SQLITE_OK);
    body=call(&cfg,publish,NULL);CHECK(strstr(body,"\"recovered\":true")&&strstr(body,external_artifact));free(body);
    CHECK(fixture.calls==1);
    SourcesConfig foreign=cfg;foreign.owner="bob";
    body=call(&foreign,publish,"unknown_source");free(body);
    /* A new prompt never calls the model from publication. */
    req=publication_request(source,"a new uncached prompt",NULL);
    body=call(&cfg,req,"inference_unavailable");free(body);free(req);CHECK(fixture.calls==1);
    char other_model[65];memset(other_model,'c',64);other_model[64]=0;
    inference.model_sha256=other_model;
    body=call(&cfg,publish,"inference_unavailable");free(body);CHECK(fixture.calls==1);
    inference.model_sha256=model;
    /* Retained public artifact read needs neither original file nor model. */
    CHECK(!unlink(path));SourcesConfig offline=cfg;offline.root=NULL;offline.media_helper=NULL;offline.visual_inference=NULL;
    body=call(&offline,tracked,NULL);CHECK(!strcmp(body,tracked_result));free(body);
    snprintf(args,sizeof(args),"{\"contract_version\":1,\"op\":\"memory_read\",\"store_id\":\"%s\",\"external_source_id\":\"%s\",\"external_artifact_id\":\"%s\",\"start_byte\":0,\"end_byte\":16}",store_id,external_source,external_artifact);
    body=call(&offline,args,NULL);CHECK(strstr(body,"A colored frame.")&&strstr(body,"image_caption")&&strstr(body,"selected_frame_only"));free(body);
    /* Captured observations are checked again before publication, even if a
     * corrupt cache writer recomputes its envelope digest. */
    CHECK(sqlite3_open(database,&db)==SQLITE_OK);
    char *artifact=field(capture,"artifact_id"),*damaged=strdup(capture);CHECK(damaged);
    char *scope=strstr(damaged,"selected_frame_only");CHECK(scope);scope[0]='X';
    replace(db,artifact,damaged);CHECK(sqlite3_close(db)==SQLITE_OK);
    body=call(&cfg,publish,"corrupt_cache");free(body);CHECK(fixture.calls==1);
    free(artifact);free(damaged);free(capture);free(publish);free(source);free(store_id);
    free(tracked);free(tracked_result);
    free(external_source);free(external_artifact);dlclose(library);
    CHECK(!nftw(root,remove_file,32,FTW_DEPTH|FTW_PHYS));free(root);
    printf("PASS visual publication: %u checks; actual native frame/public SDK, distinct caption provenance, selected-time coverage, cache-only writes, recovery/owner/corruption/offline reads; fixture model only\n",checks);
    return 0;
}
