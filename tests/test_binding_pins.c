#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "inventory.h"
#include "json.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
static char *call(SourcesConfig *cfg,const char *request,int expected){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));if(rc!=expected){fprintf(stderr,"binding: %s\n",body);}assert(rc==expected);Json j;assert(json_parse(&j,body,n));return body;}
static char *selector(const char *body,const char *key){Json j;assert(json_parse(&j,body,strlen(body)));char *value=json_string(&j,json_get(&j,0,key));assert(value);return value;}
static void file(const char *path,const char *text){FILE *out=fopen(path,"w");assert(out);assert(fputs(text,out)>=0&&!fclose(out));}
int main(void){
    char fixture[]="/tmp/samosa-binding-pins-XXXXXX";assert(mkdtemp(fixture));char root[256],state[256],path[512],held_state[256],held_root[256],database[512],request[1024];snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",fixture);snprintf(path,sizeof(path),"%s/a.txt",root);file(path,"old code");SourcesConfig cfg={.root=root,.state=state};char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"a.txt\"}",0),*old=selector(body,"source_id");free(body);
    char scope[65];assert(!inventory_scope_identity(&cfg,"",scope));struct stat st;assert(!stat(state,&st));cfg.state_pinned=1;cfg.state_device=(size_t)st.st_dev;cfg.state_inode=(size_t)st.st_ino;cfg.root_scope_pin=scope;
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",0);assert(strstr(body,"\"idle\":true"));free(body);
    snprintf(held_state,sizeof(held_state),"%s/held-state",fixture);assert(!rename(state,held_state)&&!mkdir(state,0700));body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",1);assert(strstr(body,"state_unavailable"));free(body);snprintf(database,sizeof(database),"%s/sources.sqlite3",state);assert(access(database,F_OK)<0&&errno==ENOENT);assert(!rmdir(state));body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",1);assert(strstr(body,"state_unavailable"));free(body);assert(access(state,F_OK)<0&&errno==ENOENT);assert(!rename(held_state,state));
    snprintf(held_root,sizeof(held_root),"%s/held-root",fixture);assert(!rename(root,held_root)&&!mkdir(root,0700));file(path,"new code");SourcesConfig replacement=cfg;replacement.root_scope_pin=NULL;
    body=call(&replacement,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"new-root\"}",0);char *job=selector(body,"job_id");free(body);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"new-step\",\"max_items\":1}",job);body=call(&replacement,request,0);free(body);body=call(&replacement,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"a.txt\"}",0);char *current=selector(body,"source_id");free(body);file(path,"changed!");snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"new-observation\",\"locators\":[{\"path\":\"a.txt\",\"expected_version\":\"%s\"}]}",current);body=call(&replacement,request,0);free(body);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",1);assert(strstr(body,"scope_unavailable"));free(body);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);body=call(&cfg,request,0);assert(strstr(body,"\"pending_items\":1")&&strstr(body,"\"attempts\":0"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":8}",old,old);body=call(&cfg,request,0);assert(strstr(body,"old code"));free(body);SourcesConfig offline=cfg;offline.root=NULL;body=call(&offline,request,0);assert(strstr(body,"old code"));free(body);
    free(old);free(current);free(job);puts("PASS host binding pins: replaced/missing state refuses without namespace creation; replacement-root pending work is not adopted; immutable evidence stays readable");return 0;
}
