#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 2048
#include "sources.h"
#include "json.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
extern char **environ;
static char *call(SourcesConfig *cfg,const char *request){
    for(unsigned attempt=0;attempt<250;attempt++){
        char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);
        int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
        if(!rc)return body;
        Json j;char *code=NULL;if(json_parse(&j,body,n))code=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));
        int busy=code&&(!strcmp(code,"busy")||!strcmp(code,"store_busy"));free(code);
        if(!busy){fprintf(stderr,"runtime API request: %s\nresponse: %s\n",request,body);assert(!rc);}
        free(body);struct timespec delay={0,20000000};nanosleep(&delay,NULL);
    }
    assert(0);return NULL;
}
static void write_file(const char *path,const char *text){FILE *out=fopen(path,"w");assert(out);assert(fputs(text,out)>=0&&!fclose(out));}
static pid_t spawn(const char *binary,SourcesConfig *cfg,const char *id){
    char *args[]={(char*)binary,"--root",(char*)cfg->root,"--state",(char*)cfg->state,"--watch-id",(char*)id,"--watch-interval-ms","100",NULL};
    posix_spawn_file_actions_t actions;assert(!posix_spawn_file_actions_init(&actions));
    assert(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0600));
    pid_t pid;assert(!posix_spawnp(&pid,binary,&actions,NULL,args,environ));assert(!posix_spawn_file_actions_destroy(&actions));return pid;
}
static void exited(pid_t pid,int code){int status;assert(waitpid(pid,&status,0)==pid);assert(WIFEXITED(status)&&WEXITSTATUS(status)==code);}
static void observed(SourcesConfig *cfg,const char *id,const char *status){
    char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_get\",\"watch_id\":\"%s\"}",id);
    for(unsigned i=0;i<250;i++){
        char *body=call(cfg,request);int found=strstr(body,status)!=NULL;free(body);if(found)return;
        struct timespec delay={0,20000000};nanosleep(&delay,NULL);
    }
    fprintf(stderr,"watch never observed %s\n",status);assert(0);
}
static void healthy_at_least(SourcesConfig *cfg,const char *id,size_t minimum){
    char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_get\",\"watch_id\":\"%s\"}",id);
    for(unsigned i=0;i<250;i++){
        char *body=call(cfg,request);Json j;size_t successes=0;
        assert(json_parse(&j,body,strlen(body)));int health=json_get(&j,0,"health");
        assert(json_uint(&j,json_get(&j,health,"successes"),&successes));free(body);if(successes>=minimum)return;
        struct timespec delay={0,20000000};nanosleep(&delay,NULL);
    }
    assert(0);
}
static void multiwatch(const char *binary){
    char fixture[]="/tmp/samosa-multiwatch-XXXXXX";assert(mkdtemp(fixture));
    char root[256],state[256],path[512],request[1024];snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",fixture);snprintf(path,sizeof(path),"%s/input.txt",root);write_file(path,"watch text");SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"input.txt\"}");Json j;assert(json_parse(&j,body,strlen(body)));char *version=json_string(&j,json_get(&j,0,"version"));assert(version);free(body);
    char *ids[8];pid_t workers[8];
    for(unsigned i=0;i<8;i++){
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"peer-%u\",\"locators\":[{\"path\":\"input.txt\",\"expected_version\":\"%s\"}]}",i,version);
        body=call(&cfg,request);assert(json_parse(&j,body,strlen(body)));ids[i]=json_string(&j,json_get(&j,0,"watch_id"));assert(ids[i]);free(body);workers[i]=spawn(binary,&cfg,ids[i]);
    }
    for(unsigned i=0;i<8;i++)healthy_at_least(&cfg,ids[i],16);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_stop\",\"watch_id\":\"%s\"}",ids[0]);body=call(&cfg,request);free(body);exited(workers[0],0);
    for(unsigned i=1;i<8;i++){observed(&cfg,ids[i],"\"monitoring\":\"running\"");assert(!kill(workers[i],SIGTERM));exited(workers[i],0);}
    char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);
    sqlite3 *db=NULL;sqlite3_stmt *stmt=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);
    assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM locator_watches),(SELECT count(*) FROM watch_checkpoints),(SELECT count(*) FROM watch_health),(SELECT count(*) FROM live_locators),(SELECT count(*) FROM locator_operations)",-1,&stmt,NULL)==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_ROW);
    assert(sqlite3_column_int(stmt,0)==8&&sqlite3_column_int(stmt,1)==8&&sqlite3_column_int(stmt,2)==8);
    assert(sqlite3_column_int(stmt,3)==1&&sqlite3_column_int(stmt,4)==0);
    assert(sqlite3_finalize(stmt)==SQLITE_OK&&sqlite3_close(db)==SQLITE_OK);
    for(unsigned i=0;i<8;i++)free(ids[i]);
    free(version);
    puts("PASS eight continuous watches: at least128 successful cycles, progress for every peer, bounded checkpoint/health/locator rows and no per-cycle history growth");
}
int main(int argc,char **argv){
    assert(argc==2);multiwatch(argv[1]);char fixture[]="/tmp/samosa-watch-runtime-XXXXXX";assert(mkdtemp(fixture));
    char root[256],state[256],path[512],request[1024];snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));
    snprintf(state,sizeof(state),"%s/state",fixture);snprintf(path,sizeof(path),"%s/input.txt",root);write_file(path,"watch text");SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"input.txt\"}");Json j;assert(json_parse(&j,body,strlen(body)));char *version=json_string(&j,json_get(&j,0,"version"));assert(version);free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"runtime\",\"locators\":[{\"path\":\"input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&cfg,request);assert(json_parse(&j,body,strlen(body)));char *id=json_string(&j,json_get(&j,0,"watch_id"));assert(id);free(body);
    pid_t worker=spawn(argv[1],&cfg,id);observed(&cfg,id,"\"monitoring\":\"running\"");observed(&cfg,id,"\"status\":\"available\"");
    pid_t duplicate=spawn(argv[1],&cfg,id);exited(duplicate,1);
    write_file(path,"changed!!!");observed(&cfg,id,"\"status\":\"changed\"");assert(!unlink(path));observed(&cfg,id,"\"status\":\"missing\"");
    assert(!kill(worker,SIGTERM));exited(worker,0);observed(&cfg,id,"\"monitoring\":\"not_running\"");
    worker=spawn(argv[1],&cfg,id);observed(&cfg,id,"\"monitoring\":\"running\"");assert(!kill(worker,SIGKILL));int status;assert(waitpid(worker,&status,0)==worker&&WIFSIGNALED(status));observed(&cfg,id,"\"monitoring\":\"not_running\"");
    worker=spawn(argv[1],&cfg,id);observed(&cfg,id,"\"monitoring\":\"running\"");
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_stop\",\"watch_id\":\"%s\"}",id);body=call(&cfg,request);free(body);exited(worker,0);
    free(id);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"runtime-root\",\"locators\":[{\"path\":\"input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&cfg,request);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"watch_id"));assert(id);free(body);
    worker=spawn(argv[1],&cfg,id);observed(&cfg,id,"\"status\":\"missing\"");
    char held[256];snprintf(held,sizeof(held),"%s/held-root",fixture);assert(!rename(root,held)&&!mkdir(root,0700));write_file(path,"watch text");
    exited(worker,1);observed(&cfg,id,"\"monitoring\":\"not_running\"");
    observed(&cfg,id,"\"last_error\":\"scope_unavailable\"");
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"runtime-state\",\"locators\":[{\"path\":\"input.txt\",\"expected_version\":\"%s\"}]}",version);
    free(id);body=call(&cfg,request);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"watch_id"));assert(id);free(body);
    worker=spawn(argv[1],&cfg,id);observed(&cfg,id,"\"status\":\"available\"");
    snprintf(held,sizeof(held),"%s/held-state",fixture);assert(!rename(state,held)&&!mkdir(state,0700));exited(worker,1);
    cfg.state=held;observed(&cfg,id,"\"monitoring\":\"not_running\"");observed(&cfg,id,"\"status\":\"available\"");
    char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",held);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);
    assert(sqlite3_exec(db,"UPDATE watch_health SET failures=-1",NULL,NULL,NULL)==SQLITE_OK);assert(sqlite3_close(db)==SQLITE_OK);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_get\",\"watch_id\":\"%s\"}",id);
    char *refusal=NULL;size_t length;FILE *stream=open_memstream(&refusal,&length);assert(stream);
    assert(sources_dispatch(&cfg,request,strlen(request),stream)==1&&!fclose(stream));
    assert(json_parse(&j,refusal,length)&&strstr(refusal,"corrupt_locator")&&!strstr(refusal,"checkpoint"));free(refusal);
    free(id);free(version);puts("PASS continuous watch: lease/duplicate refusal, actual changes, signal/public shutdown, SIGKILL recovery, root/state replacement refusal and retained checkpoints");return 0;
}
