#include "visual_fixture.h"
#include "visual_index.h"
#include "watch.h"
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
static void symbol(void *library,const char *name,void *target,size_t n){void *p=dlsym(library,name);CHECK(p&&n==sizeof(p));memcpy(target,&p,n);}
static char *job_request(const char *op,const char *job){char *s=NULL;size_t n;FILE *f=open_memstream(&s,&n);CHECK(f);fprintf(f,"{\"contract_version\":1,\"op\":\"%s\",\"job_id\":\"%s\"}",op,job);CHECK(!fclose(f));return s;}
static char *create_request(const char *source,size_t start,size_t end){char *s=NULL;size_t n;FILE *f=open_memstream(&s,&n);CHECK(f);fprintf(f,"{\"contract_version\":1,\"op\":\"visual_index_create\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu}",source,source,start,end);CHECK(!fclose(f));return s;}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--sources-native-worker"))return sources_native_worker_main(argc,argv);
    CHECK(argc==5);void *library=dlopen(argv[4],RTLD_NOW|RTLD_LOCAL);CHECK(library);
    int (*create)(const char *,const char *,void **)=NULL;int (*sdk_call)(void *,const char *,const char *,char **)=NULL;
    void (*sdk_free)(void *)=NULL;void (*close_store)(void *)=NULL;
    symbol(library,"chutni_create",&create,sizeof(create));symbol(library,"chutni_call",&sdk_call,sizeof(sdk_call));symbol(library,"chutni_free",&sdk_free,sizeof(sdk_free));symbol(library,"chutni_close",&close_store,sizeof(close_store));
    char temp[]="/tmp/samosa-visual-queue-XXXXXX";CHECK(mkdtemp(temp));char *root=realpath(temp,NULL);CHECK(root);
    char state[512],path[512],store_path[512],database[600];snprintf(state,sizeof(state),"%s/state",root);snprintf(path,sizeof(path),"%s/video.mp4",root);snprintf(store_path,sizeof(store_path),"%s/memory.chutni",root);snprintf(database,sizeof(database),"%s/sources.sqlite3",state);
    FILE *from=fopen(argv[2],"rb"),*to=fopen(path,"wb");CHECK(from&&to);unsigned char buffer[65536];size_t n;while((n=fread(buffer,1,sizeof(buffer),from)))CHECK(fwrite(buffer,1,n,to)==n);CHECK(!ferror(from)&&!fclose(from)&&!fclose(to));
    void *store=NULL;CHECK(!create(store_path,"Durable visual queue native gate",&store));char *info=NULL;CHECK(!sdk_call(store,"store_info","{}",&info));char *store_id=field(info,"store_id");sdk_free(info);close_store(store);
    char engine[65],model[65];memset(engine,'a',64);memset(model,'b',64);engine[64]=model[64]=0;
    Fixture fixture={.mode="ok"};SourcesInferenceConfig inference={.provider_id="fixture-vision-v1",.engine_sha256=engine,.model_sha256=model,.call=transport,.context=&fixture};
    SourcesConfig cfg={.root=root,.state=state,.owner="alice",.media_helper=argv[1],.visual_inference=&inference,.chutni_helper=argv[3],.chutni_library=argv[4],.chutni_store=store_path,.chutni_store_id=store_id,.chutni_publish=1,.cache_limit=1,.cache_byte_limit=1024};fixture.inference=&inference;fixture.sources=&cfg;
    size_t total,start,end;CHECK(!sources_visual_window_plan(&cfg,30000,0,&total,&start,&end)&&total==1&&start==0&&end==30000);
    CHECK(!sources_visual_window_plan(&cfg,30001,1,&total,&start,&end)&&total==2&&start==30000&&end==30001);
    for(size_t i=0;i<480;i++){CHECK(!sources_visual_window_plan(&cfg,14400000,i,&total,&start,&end));CHECK(total==480&&start==i*30000&&end==start+30000);}CHECK(sources_visual_window_plan(&cfg,14400001,0,&total,&start,&end));
    char *unsupported=call(&cfg,"{\"contract_version\":1,\"op\":\"visual_index_unknown\"}","unsupported_operation");free(unsupported);
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"video.mp4\"}",NULL),*source=field(body,"source_id");free(body);
    char *req=create_request(source,0,61000);body=call(&cfg,req,NULL);char *job=field(body,"job_id");CHECK(strstr(body,"\"total_windows\":3")&&strstr(body,"sampled_visual_frames"));char *initial=strdup(body);CHECK(initial);free(body);body=call(&cfg,req,NULL);CHECK(!strcmp(body,initial));free(body);free(initial);free(req);
    sqlite3 *db=NULL;CHECK(sqlite3_open(database,&db)==SQLITE_OK);size_t cached=count(db);CHECK(sqlite3_close(db)==SQLITE_OK);CHECK(fixture.calls==0);
    SourcesConfig foreign=cfg;foreign.owner="bob";req=job_request("visual_index_get",job);body=call(&foreign,req,"unknown_visual_index");free(body);free(req);
    for(size_t ordinal=0;ordinal<3;ordinal++){
        req=job_request("visual_index_step",job);body=call(&cfg,req,NULL);CHECK(strstr(body,"\"phase\":\"captured\"")&&fixture.calls==ordinal+1);free(body);
        inference.call=NULL;body=call(&cfg,"{\"contract_version\":1,\"op\":\"visual_index_queue_step\"}",NULL);CHECK(strstr(body,"\"phase\":\"published\"")&&fixture.calls==ordinal+1);free(body);inference.call=transport;free(req);
        CHECK(sqlite3_open(database,&db)==SQLITE_OK);CHECK(count(db)==cached);CHECK(sqlite3_close(db)==SQLITE_OK);
    }
    req=job_request("visual_index_get",job);body=call(&cfg,req,NULL);CHECK(strstr(body,"\"status\":\"completed\"")&&strstr(body,"\"published_windows\":3"));free(body);free(req);
    /* The existing runtime executes visual work and honors TERM. Status reads
     * can observe the same shared admission lease while the worker is active. */
    req=create_request(source,3000,4000);body=call(&cfg,req,NULL);char *runtime_job=field(body,"job_id");free(body);free(req);
    char runtime_log[600];snprintf(runtime_log,sizeof(runtime_log),"%s/runtime.log",root);
    pid_t runtime=fork();CHECK(runtime>=0);
    if(!runtime){FILE *sink=fopen(runtime_log,"w");if(!sink)_exit(2);int rc=sources_queue_run(&cfg,100,sink);fclose(sink);_exit(rc);}
    req=job_request("visual_index_get",runtime_job);int completed=0;
    for(size_t i=0;i<1000&&!completed;i++){
        int early_status;pid_t ended=waitpid(runtime,&early_status,WNOHANG);CHECK(ended>=0);
        if(ended==runtime){FILE *log=fopen(runtime_log,"r");if(log){char text[4096];size_t bytes=fread(text,1,sizeof(text),log);fwrite(text,1,bytes,stderr);fclose(log);}fprintf(stderr,"runtime exited early: %d\n",early_status);exit(1);}
        body=NULL;size_t length=0;FILE *out=open_memstream(&body,&length);CHECK(out);
        int rc=sources_dispatch(&cfg,req,strlen(req),out);CHECK(!fclose(out));
        if(!rc)completed=strstr(body,"\"status\":\"completed\"")!=NULL;
        else {Json j;CHECK(json_parse(&j,body,length));char *error=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));CHECK(error&&!strcmp(error,"busy"));free(error);}
        free(body);struct timespec pause={0,30000000};nanosleep(&pause,NULL);
    }
    CHECK(completed);CHECK(!kill(runtime,SIGTERM));int runtime_status;CHECK(waitpid(runtime,&runtime_status,0)==runtime&&WIFEXITED(runtime_status)&&WEXITSTATUS(runtime_status)==0);free(req);free(runtime_job);
    /* A failed publisher leaves a captured window and durable retry state; it
     * must publish that same capture without a second model invocation. */
    req=create_request(source,1000,2000);body=call(&cfg,req,NULL);char *retry_job=field(body,"job_id");free(body);free(req);req=job_request("visual_index_step",retry_job);body=call(&cfg,req,NULL);free(body);size_t calls=fixture.calls;inference.call=NULL;
    SourcesConfig missing=cfg;missing.chutni_helper="/missing/visual-publisher";body=call(&missing,req,"native_helper_unavailable");free(body);body=call(&cfg,req,NULL);CHECK(strstr(body,"retry_backoff")&&fixture.calls==calls);free(body);
    CHECK(sqlite3_open(database,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE visual_index_jobs SET next_ms=0",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);body=call(&cfg,req,NULL);CHECK(strstr(body,"published")&&fixture.calls==calls);free(body);free(req);free(retry_job);
    /* Parent death after SDK commit but before local receipt: recover the SDK
     * artifact and commit the retained window, with origin/model absent. */
    req=create_request(source,2000,3000);body=call(&cfg,req,NULL);char *crash_job=field(body,"job_id");free(body);free(req);req=job_request("visual_index_step",crash_job);inference.call=transport;body=call(&cfg,req,NULL);free(body);inference.call=NULL;
    char marker[600];snprintf(marker,sizeof(marker),"%s/post-sdk",root);pid_t child=fork();CHECK(child>=0);
    if(!child){if(setenv("SAMOSA_CHUTNI_PUBLICATION_POST_SDK_MARKER",marker,1))_exit(2);char *reply=call(&cfg,req,NULL);free(reply);_exit(3);}
    int marked=0,status;for(size_t i=0;i<1000&&!marked;i++){marked=!access(marker,F_OK);struct timespec pause={0,30000000};nanosleep(&pause,NULL);}CHECK(marked);CHECK(!kill(child,SIGKILL));CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status));CHECK(!unlink(marker));CHECK(!unlink(path));
    body=call(&cfg,req,NULL);CHECK(strstr(body,"published"));free(body);free(req);free(crash_job);
    char read[512];snprintf(read,sizeof(read),"{\"contract_version\":1,\"op\":\"visual_index_read\",\"job_id\":\"%s\",\"ordinal\":2}",job);body=call(&cfg,read,NULL);CHECK(strstr(body,"60000000")&&strstr(body,"selected_frame_only")&&strstr(body,"visual_analysis_uncertain")&&!strstr(body,"evidence_id"));free(body);
    /* Shared admission capacity applies across modalities and retries cannot
     * create duplicate jobs. Eight jobs are retained; the ninth refuses. */
    for(size_t i=0;i<3;i++){req=create_request(source,4000+i*1000,5000+i*1000);body=call(&cfg,req,NULL);free(body);free(req);}
    unsigned char wave[32044]={0};memcpy(wave,"RIFF",4);memcpy(wave+8,"WAVEfmt ",8);wave[4]=36;wave[5]=125;wave[16]=16;wave[20]=1;wave[22]=1;wave[24]=128;wave[25]=62;wave[28]=0;wave[29]=125;wave[32]=2;wave[34]=16;memcpy(wave+36,"data",4);wave[40]=0;wave[41]=125;
    char *registered=NULL;size_t registered_bytes=0;FILE *registration=open_memstream(&registered,&registered_bytes);CHECK(registration);CHECK(!sources_upload(&cfg,SOURCES_UPLOAD_WAV,wave,sizeof(wave),registration));CHECK(!fclose(registration));char *audio_source=field(registered,"source_id");free(registered);
    SourcesInferenceConfig audio_identity={.provider_id="fixture-whisper",.engine_sha256=engine,.model_sha256=model};SourcesConfig audio=cfg;audio.inference=&audio_identity;audio.chutni_publish=0;
    char audio_request[512];snprintf(audio_request,sizeof(audio_request),"{\"contract_version\":1,\"op\":\"audio_index_create\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":1000}",audio_source,audio_source);body=call(&audio,audio_request,NULL);free(body);free(audio_source);
    req=create_request(source,8000,9000);body=call(&cfg,req,"visual_index_limit");free(body);free(req);
    CHECK(sqlite3_open(database,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE visual_index_windows SET digest='damaged' WHERE ordinal=2",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);body=call(&cfg,read,"corrupt_visual_index");free(body);
    free(job);free(source);free(store_id);dlclose(library);CHECK(!nftw(root,remove_file,32,FTW_DEPTH|FTW_PHYS));free(root);
    printf("PASS durable visual queue: %u checks; actual native frame/SDK, temporal planning, transient capture, publication phases/backoff, owner/quota, SDK commit/SIGKILL recovery and retained uncertain frames; fixture model only\n",checks);return 0;
}
