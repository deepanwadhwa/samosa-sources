#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
static char *call(SourcesConfig *cfg,const char *request){
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);
    int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
    if(rc)fprintf(stderr,"index freshness: %s\n",body);
    assert(!rc);return body;
}
static void write_file(const char *path,const char *text){
    FILE *out=fopen(path,"w");assert(out);assert(fputs(text,out)>=0&&!fclose(out));
}
static void pending_count(SourcesConfig *cfg,const char *job,size_t expected,const char *status){
    char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);
    SourcesConfig offline=*cfg;offline.root=NULL;char *body=call(&offline,request);Json j;size_t count;
    assert(json_parse(&j,body,strlen(body))&&json_uint(&j,json_get(&j,0,"pending_items"),&count)&&count==expected);
    int rows=json_get(&j,0,"pending");assert(rows>=0&&(size_t)j.tokens[rows].count==expected);
    if(expected){size_t ordinal,revision;assert(json_uint(&j,json_get(&j,rows+1,"ordinal"),&ordinal)&&ordinal==0);
        int observation=json_get(&j,rows+1,"live_observation");assert(json_uint(&j,json_get(&j,observation,"locator_revision"),&revision)&&revision>0);
        char *value=json_string(&j,json_get(&j,observation,"status"));assert(value&&!strcmp(value,status));free(value);
    }
    free(body);
}
typedef struct {unsigned calls,at;} Cancel;
static int cancel_at(void *context){Cancel *c=context;return ++c->calls==c->at;}
typedef struct {const char *path;unsigned calls,change_at;} Change;
static int change(void *context){
    Change *c=context;if(++c->calls==c->change_at)write_file(c->path,"changed!!");return 0;
}
static void observation_history(void){
    char fixture[]="/tmp/samosa-index-observation-XXXXXX";assert(mkdtemp(fixture));
    char root[256],state[256],folder[512],path[1024],request[1024],search[512];
    snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));
    snprintf(state,sizeof(state),"%s/state",fixture);
    snprintf(folder,sizeof(folder),"%s/folder",root);assert(!mkdir(folder,0700));
    snprintf(path,sizeof(path),"%s/input.txt",folder);write_file(path,"code 7319");
    SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"folder\",\"operation_id\":\"create\"}");
    Json j;assert(json_parse(&j,body,strlen(body)));char *job=json_string(&j,json_get(&j,0,"job_id"));assert(job);free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"step\",\"max_items\":1}",job);
    body=call(&cfg,request);free(body);
    snprintf(search,sizeof(search),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"7319\"}",job);
    body=call(&cfg,search);assert(strstr(body,"unobserved"));
    assert(json_parse(&j,body,strlen(body)));int candidates=json_get(&j,0,"candidates");assert(candidates>=0&&j.tokens[candidates].count==1);
    char *version=json_string(&j,json_get(&j,candidates+1,"version"));assert(version);free(body);
    pending_count(&cfg,job,0,NULL);
    const char *statuses[]={"available","changed","missing"};
    for(unsigned i=0;i<3;i++){
        if(i==1)write_file(path,"changed!!");
        if(i==2)assert(!unlink(path));
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"observe-%u\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",i,version);
        body=call(&cfg,request);free(body);
        SourcesConfig offline=cfg;offline.root=NULL;body=call(&offline,search);
        assert(json_parse(&j,body,strlen(body)));candidates=json_get(&j,0,"candidates");
        int observed=json_get(&j,candidates+1,"live_observation");char *status=json_string(&j,json_get(&j,observed,"status"));
        assert(status&&!strcmp(status,statuses[i]));free(status);free(body);
        pending_count(&cfg,job,i?1:0,statuses[i]);
    }
    write_file(path,"updated!!");Cancel cancellation={0,0};SourcesConfig probe=cfg;probe.cancelled=cancel_at;probe.cancellation=&cancellation;
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"count-checks\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&probe,request);free(body);unsigned precommit_checks=cancellation.calls;assert(precommit_checks>2);pending_count(&cfg,job,1,"changed");
    write_file(path,"code 7319");
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"restore\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&cfg,request);free(body);pending_count(&cfg,job,0,NULL);
    write_file(path,"updated!!");cancellation.calls=0;cancellation.at=precommit_checks;
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"cancel-before-commit\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",version);
    size_t cancelled_length;FILE *cancelled_out=open_memstream(&body,&cancelled_length);assert(cancelled_out);
    assert(sources_dispatch(&probe,request,strlen(request),cancelled_out)&&!fclose(cancelled_out));assert(strstr(body,"cancelled"));free(body);
    pending_count(&cfg,job,0,NULL);body=call(&cfg,search);assert(strstr(body,"\"status\":\"available\""));free(body);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"folder/input.txt\"}");assert(json_parse(&j,body,strlen(body)));char *other_version=json_string(&j,json_get(&j,0,"source_id"));assert(other_version&&strcmp(other_version,version));free(body);
    write_file(path,"code 7319");
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",other_version);free(other_version);
    body=call(&cfg,request);assert(json_parse(&j,body,strlen(body)));char *watch=json_string(&j,json_get(&j,0,"watch_id"));assert(watch);free(body);
    for(unsigned cycle=0;cycle<4;cycle++){
        if(cycle==1)write_file(path,"updated!!");
        if(cycle==3)assert(!unlink(path));
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":%u}",watch,cycle);
        body=call(&cfg,request);free(body);
        pending_count(&cfg,job,cycle?1:0,cycle==3?"missing":"changed");
        /* Exact checkpoint retry must not duplicate pending work. */
        body=call(&cfg,request);free(body);pending_count(&cfg,job,cycle?1:0,cycle==3?"missing":"changed");
    }
    free(watch);
    write_file(path,"code 7319");SourcesConfig foreign=cfg;foreign.owner="other-owner";
    body=call(&foreign,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"folder/input.txt\"}");free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"observe-foreign\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&foreign,request);free(body);pending_count(&cfg,job,1,"missing");body=call(&cfg,search);assert(strstr(body,"\"status\":\"missing\""));free(body);
    SourcesConfig other_host=cfg;other_host.host_id="another-host";
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"observe-host\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&other_host,request);free(body);pending_count(&cfg,job,1,"missing");body=call(&cfg,search);assert(strstr(body,"\"status\":\"missing\""));free(body);
    char held[256];snprintf(held,sizeof(held),"%s/held-root",fixture);
    assert(!rename(root,held)&&!mkdir(root,0700)&&!mkdir(folder,0700));write_file(path,"code 7319");
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"observe-root\",\"locators\":[{\"path\":\"folder/input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&cfg,request);free(body);pending_count(&cfg,job,1,"missing");body=call(&cfg,search);assert(strstr(body,"\"status\":\"missing\""));free(body);
    free(version);free(job);
}
int main(void){
    observation_history();
    unsigned checks=0;
    for(int racing=0;racing<2;racing++){
        char fixture[]="/tmp/samosa-index-freshness-XXXXXX";assert(mkdtemp(fixture));
        char root[256],state[256],path[512],request[512];
        snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));
        snprintf(state,sizeof(state),"%s/state",fixture);
        snprintf(path,sizeof(path),"%s/input.txt",root);write_file(path,"code 7319");
        SourcesConfig cfg={.root=root,.state=state};
        char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"create\"}");
        Json j;assert(json_parse(&j,body,strlen(body)));char *job=json_string(&j,json_get(&j,0,"job_id"));assert(job);free(body);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"step\",\"max_items\":1}",job);
        Change mutation={path,0,racing?checks:0};SourcesConfig work=cfg;
        work.cancelled=change;work.cancellation=&mutation;
        body=call(&work,request);assert(json_parse(&j,body,strlen(body)));
        int result=json_get(&j,0,"result");size_t indexed,failed;
        assert(json_uint(&j,json_get(&j,result,"indexed_items"),&indexed));
        assert(json_uint(&j,json_get(&j,result,"failed_items"),&failed));
        assert(indexed==(racing?0u:1u)&&failed==(racing?1u:0u));free(body);
        if(!racing){checks=mutation.calls;assert(checks>4);}
        cfg.root=NULL;body=call(&cfg,request);assert(strstr(body,"completed"));free(body);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"7319\"}",job);
        body=call(&cfg,request);assert(json_parse(&j,body,strlen(body)));size_t count;
        assert(json_uint(&j,json_get(&j,0,"returned_candidates"),&count)&&count==(racing?0u:1u));free(body);free(job);
    }
    puts("PASS index freshness: actual extraction-time mutation commits a failed item, no stale candidate; stable snapshot and offline checkpoints remain readable");return 0;
}
