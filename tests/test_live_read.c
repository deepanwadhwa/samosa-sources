#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
static char *call(SourcesConfig *cfg,const char *request,int expected){
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);
    int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
    if(rc!=expected)fprintf(stderr,"live read: %s\n",body);
    assert(rc==expected);return body;
}
static void write_file(const char *path,const char *text){
    FILE *out=fopen(path,"w");assert(out);assert(fputs(text,out)>=0&&!fclose(out));
}
typedef struct {const char *path;unsigned calls;} Mutation;
static int mutate(void *context){
    Mutation *m=context;if(++m->calls==4)write_file(m->path,"changed!!");return 0;
}
typedef struct {SourcesConfig *cfg;unsigned calls;} Cancellation;
static int request_cancel(void *context){
    Cancellation *c=context;
    if(++c->calls==4){
        char *body=call(c->cfg,"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"live-cancel\"}",0);
        free(body);
    }
    return 0;
}
typedef struct {const char *selected,*held,*path;unsigned calls;int directory;} Replacement;
static int replace_node(void *context){
    Replacement *r=context;
    if(++r->calls==4){
        assert(!rename(r->selected,r->held));
        if(r->directory)assert(!mkdir(r->selected,0700));
        if(r->directory==2){
            char parent[1024];snprintf(parent,sizeof(parent),"%s/folder",r->selected);
            assert(!mkdir(parent,0700));
        }
        write_file(r->path,"code 7319");
    }
    return 0;
}
static void identity_races(void){
    for(int mode=0;mode<3;mode++){
        char fixture[]="/tmp/samosa-live-binding-XXXXXX";assert(mkdtemp(fixture));
        char root[256],state[256],parent[512],path[1024],held[256],request[1400];
        snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));
        snprintf(state,sizeof(state),"%s/state",fixture);
        snprintf(parent,sizeof(parent),"%s/folder",root);assert(!mkdir(parent,0700));
        snprintf(path,sizeof(path),"%s/input.txt",parent);write_file(path,"code 7319");
        snprintf(held,sizeof(held),"%s/held",fixture);
        SourcesConfig cfg={.root=root,.state=state};
        char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"folder/input.txt\"}",0);
        Json j;assert(json_parse(&j,body,strlen(body)));char *version=json_string(&j,json_get(&j,0,"version"));assert(version);free(body);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_read\",\"path\":\"folder/input.txt\",\"read\":{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":9}}",version,version);
        body=call(&cfg,request,0);assert(strstr(body,"code 7319"));free(body);
        struct stat original;assert(!stat(path,&original));
        char pinned[1800];
        snprintf(pinned,sizeof(pinned),"{\"expected_device\":%llu,\"expected_inode\":%llu,%s",(unsigned long long)original.st_dev,(unsigned long long)original.st_ino,request+1);
        Replacement replacement={mode==0?path:mode==1?parent:root,held,path,0,mode};
        SourcesConfig racing=cfg;racing.cancelled=replace_node;racing.cancellation=&replacement;
        body=call(&racing,request,1);assert(strstr(body,"source_changed")&&!strstr(body,"evidence"));free(body);
        body=call(&cfg,request,0);assert(strstr(body,"verified_live_read"));free(body);
        body=call(&cfg,pinned,1);assert(strstr(body,"live_source_unavailable")&&!strstr(body,"evidence"));free(body);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":9}",version,version);
        cfg.root=NULL;body=call(&cfg,request,0);assert(strstr(body,"code 7319"));free(body);free(version);
    }
}
int main(void){
    identity_races();
    char root[]="/tmp/samosa-live-read-XXXXXX";assert(mkdtemp(root));
    char path[512],state[512],request[1024];snprintf(path,sizeof(path),"%s/input.txt",root);
    snprintf(state,sizeof(state),"%s/state",root);write_file(path,"code 7319");
    SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"input.txt\"}",0);
    Json j;assert(json_parse(&j,body,strlen(body)));char *version=json_string(&j,json_get(&j,0,"version"));assert(version);free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_read\",\"path\":\"input.txt\",\"read\":{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":9}}",version,version);
    body=call(&cfg,request,0);assert(strstr(body,"verified_live_read")&&strstr(body,"code 7319"));free(body);
    char tracked[1400];
    snprintf(tracked,sizeof(tracked),"{\"operation_id\":\"live-success\",\"parent_operation_id\":\"live-parent\",\"timeout_ms\":60000,%s",request+1);
    char *saved=call(&cfg,tracked,0);assert(strstr(saved,"live-parent"));
    write_file(path,"changed!!");SourcesConfig offline=cfg;offline.root=NULL;
    body=call(&offline,tracked,0);assert(!strcmp(saved,body));free(body);
    char reordered[1400];
    snprintf(reordered,sizeof(reordered),"{\"timeout_ms\":60000,\"parent_operation_id\":\"live-parent\",\"operation_id\":\"live-success\",\"read\":{\"end_byte\":9,\"start_byte\":0,\"version\":\"%s\",\"source_id\":\"%s\",\"op\":\"read_text\",\"contract_version\":1},\"path\":\"input.txt\",\"op\":\"locator_read\",\"contract_version\":1}",version,version);
    body=call(&offline,reordered,0);assert(!strcmp(saved,body));free(body);free(saved);
    char *end=strstr(reordered,"\"end_byte\":9");assert(end);end[11]='8';
    body=call(&offline,reordered,1);assert(strstr(body,"idempotency_conflict"));free(body);
    body=call(&cfg,request,1);assert(strstr(body,"live_source_unavailable"));free(body);
    write_file(path,"code 7319");
    snprintf(tracked,sizeof(tracked),"{\"operation_id\":\"live-cancel\",%s",request+1);
    Cancellation cancellation={&cfg,0};SourcesConfig controlled=cfg;
    controlled.cancelled=request_cancel;controlled.cancellation=&cancellation;
    saved=call(&controlled,tracked,1);assert(strstr(saved,"cancelled")&&!strstr(saved,"evidence"));
    body=call(&offline,tracked,1);assert(!strcmp(saved,body));free(saved);free(body);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"live-cancel\"}",0);
    assert(strstr(body,"\"status\":\"cancelled\""));free(body);
    snprintf(tracked,sizeof(tracked),"{\"operation_id\":\"live-deadline\",%s",request+1);
    SourcesConfig expired=cfg;expired.deadline_monotonic=1;
    saved=call(&expired,tracked,1);assert(strstr(saved,"deadline_exceeded")&&!strstr(saved,"evidence"));
    body=call(&offline,tracked,1);assert(!strcmp(saved,body));free(saved);free(body);
    Mutation mutation={path,0};SourcesConfig racing=cfg;racing.cancelled=mutate;racing.cancellation=&mutation;
    body=call(&racing,request,1);assert(strstr(body,"source_changed")&&!strstr(body,"code 7319"));free(body);
    body=call(&cfg,request,1);assert(strstr(body,"live_source_unavailable")&&!strstr(body,"evidence"));free(body);
    write_file(path,"code 7319");body=call(&cfg,request,0);assert(strstr(body,"code 7319"));free(body);
    SourcesConfig foreign=cfg;foreign.owner="foreign";body=call(&foreign,request,1);assert(strstr(body,"unknown_source"));free(body);
    assert(!unlink(path));body=call(&cfg,request,1);assert(strstr(body,"live_source_unavailable"));free(body);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"locator_read\",\"path\":\"input.txt\"}",1);assert(strstr(body,"invalid_request"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":9}",version,version);
    cfg.root=NULL;body=call(&cfg,request,0);assert(strstr(body,"code 7319"));free(body);free(version);
    puts("PASS live read: stable evidence, warm-cache mutation refusal, changed/missing/owner refusal and retained immutable citations; durable normalized retries, conflict, public cancellation and deadline");return 0;
}
