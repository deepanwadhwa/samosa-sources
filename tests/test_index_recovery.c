#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "sources.h"
#include "json.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void file(const char *path){FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs("7319 discovery",f)>=0);CHECK(!fclose(f));}
static char *call(const SourcesConfig *cfg,const char *request,int expected){
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);
    CHECK(sources_dispatch(cfg,request,strlen(request),out)==expected);CHECK(!fclose(out));
    Json j;CHECK(json_parse(&j,body,n));return body;
}
static char *request(const char *job,const char *operation){
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);
    fprintf(out,"{\"contract_version\":1,\"op\":\"index_discover\",\"job_id\":\"%s\",\"operation_id\":\"%s\"}",job,operation);
    CHECK(!fclose(out));return body;
}
static sqlite3_int64 scalar(sqlite3 *db,const char *sql){sqlite3_stmt *stmt=NULL;CHECK(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK);CHECK(sqlite3_step(stmt)==SQLITE_ROW);sqlite3_int64 n=sqlite3_column_int64(stmt,0);CHECK(sqlite3_finalize(stmt)==SQLITE_OK);return n;}
static void retained(const char *state){
    char path[2048];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(scalar(db,"SELECT cursor FROM index_jobs")==1);
    CHECK(scalar(db,"SELECT count(*) FROM index_items WHERE status='failed' AND text IS NULL AND evidence IS NULL")==1);
    CHECK(scalar(db,"SELECT count(*) FROM index_repaired")==0);
    CHECK(scalar(db,"SELECT count(*) FROM index_recovery WHERE next_ms>0 AND failures=1 AND turn>0")==1);
    CHECK(sqlite3_close(db)==SQLITE_OK);
}
typedef struct {SourcesConfig *cfg;int deadline;} Control;
static int cancelled(void *context){
    Control *c=context;const char *marker=getenv("SAMOSA_RECOVERY_TEST_PRECOMMIT_MARKER");
    if(!marker||access(marker,F_OK))return 0;
    if(c->deadline){c->cfg->deadline_monotonic=1;return 0;}
    return 1;
}
static pid_t spawn(const char *exe,const char *root,const char *state,const char *body,const char *mode,const char *marker,const char *release,const char *output){
    CHECK(!unlink(marker)||errno==ENOENT);CHECK(!unlink(release)||errno==ENOENT);
    CHECK(!setenv("SAMOSA_RECOVERY_TEST_PRECOMMIT_MARKER",marker,1));CHECK(!setenv("SAMOSA_RECOVERY_TEST_PRECOMMIT_RELEASE",release,1));
    char *args[]={(char*)exe,"--worker",(char*)root,(char*)state,(char*)body,(char*)mode,NULL};
    posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));CHECK(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,output,O_CREAT|O_WRONLY|O_TRUNC,0600));
    pid_t child;CHECK(!posix_spawnp(&child,exe,&actions,NULL,args,environ));CHECK(!posix_spawn_file_actions_destroy(&actions));
    CHECK(!unsetenv("SAMOSA_RECOVERY_TEST_PRECOMMIT_MARKER"));CHECK(!unsetenv("SAMOSA_RECOVERY_TEST_PRECOMMIT_RELEASE"));
    int ready=0;for(int i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec pause={0,10000000};nanosleep(&pause,NULL);}CHECK(ready);return child;
}
static void finished(pid_t child,const char *output,const char *code,int expected){
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==expected);
    FILE *f=fopen(output,"r");CHECK(f);char body[2048];size_t n=fread(body,1,sizeof(body)-1,f);CHECK(!ferror(f));CHECK(!fclose(f));body[n]=0;
    Json j;CHECK(json_parse(&j,body,n));CHECK(strstr(body,code));CHECK(!strstr(body,"total_items"));if(!expected)CHECK(strstr(body,"\"recovered_items\":0"));
}
int main(int argc,char **argv){
    if(argc==6&&!strcmp(argv[1],"--worker")){
        SourcesConfig cfg={.root=argv[2],.state=argv[3],.discovery_interval_ms=100};Control control={.cfg=&cfg,.deadline=!strcmp(argv[5],"deadline")};
        if(strcmp(argv[5],"plain")){cfg.cancelled=cancelled;cfg.cancellation=&control;}
        return sources_dispatch(&cfg,argv[4],strlen(argv[4]),stdout);
    }
    char fixture[]="/tmp/samosa-recovery-control-XXXXXX";CHECK(mkdtemp(fixture));
    char root[1024],folder[1024],state[1024],path[2048],marker[1024],release[1024],output[1024],old[1024],request_body[512];
    snprintf(root,sizeof(root),"%s/root",fixture);snprintf(folder,sizeof(folder),"%s/root/folder",fixture);snprintf(state,sizeof(state),"%s/state",fixture);CHECK(!mkdir(root,0700));CHECK(!mkdir(folder,0700));
    snprintf(marker,sizeof(marker),"%s/marker",fixture);snprintf(release,sizeof(release),"%s/release",fixture);snprintf(output,sizeof(output),"%s/output",fixture);snprintf(old,sizeof(old),"%s/held",fixture);snprintf(path,sizeof(path),"%s/input.txt",folder);file(path);
    SourcesConfig cfg={.root=root,.state=state,.discovery_interval_ms=100};char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"folder\",\"operation_id\":\"create\"}",0);
    Json j;CHECK(json_parse(&j,body,strlen(body)));char *job=json_string(&j,json_get(&j,0,"job_id"));CHECK(job&&strlen(job)==64);free(body);CHECK(!unlink(path));
    snprintf(request_body,sizeof(request_body),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"initial\",\"max_items\":1}",job);body=call(&cfg,request_body,0);CHECK(strstr(body,"\"failed_items\":1"));free(body);
    const char *refresh="{\"contract_version\":1,\"op\":\"index_refresh_step\"}";
    const char *modes[]={"cancel","deadline","plain","plain","plain"};
    for(int attempt=0;attempt<5;attempt++){
        file(path);char operation[32];snprintf(operation,sizeof(operation),"queue-%d",attempt);char *input=request(job,operation);body=call(&cfg,input,0);free(body);free(input);
        pid_t child=spawn(argv[0],root,state,refresh,modes[attempt],marker,release,output);
        if(attempt==2){CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);}
        else {
            if(attempt==3){CHECK(!rename(root,old));CHECK(!mkdir(root,0700));}
            if(attempt==4){CHECK(!rename(folder,old));CHECK(!mkdir(folder,0700));}
            file(release);finished(child,output,attempt==0?"cancelled":attempt==1?"deadline_exceeded":attempt==3?"scope_unavailable":"stale_scope",attempt<2?1:0);
            if(attempt==3){CHECK(!rmdir(root));CHECK(!rename(old,root));}
            if(attempt==4){CHECK(!rmdir(folder));CHECK(!rename(old,folder));}
        }
        retained(state);
        snprintf(request_body,sizeof(request_body),"{\"contract_version\":1,\"op\":\"index_get\",\"job_id\":\"%s\"}",job);SourcesConfig probe=cfg;probe.root=NULL;body=call(&probe,request_body,0);
        CHECK(json_parse(&j,body,strlen(body)));int queue=json_get(&j,0,"recovery_queue");size_t n=0;CHECK(json_uint(&j,json_get(&j,queue,"pending_items"),&n)&&n==1);CHECK(json_uint(&j,json_get(&j,queue,"highest_failure_level"),&n)&&n==1);CHECK(json_uint(&j,json_get(&j,queue,"earliest_retry_unix_ms"),&n)&&n>0);free(body);
    }
    char db_path[2048];snprintf(db_path,sizeof(db_path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(db_path,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE index_recovery SET failures=8",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    SourcesConfig corrupt=cfg;corrupt.root=NULL;body=call(&corrupt,request_body,1);CHECK(strstr(body,"corrupt_index")&&!strstr(body,"total_items"));free(body);
    file(path);char *bad_input=request(job,"corrupt-new-target");body=call(&cfg,bad_input,1);CHECK(strstr(body,"corrupt_index"));free(body);free(bad_input);
    CHECK(sqlite3_open(db_path,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE index_recovery SET failures=1",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    file(path);char *input=request(job,"queue-success");body=call(&cfg,input,0);free(body);free(input);body=call(&cfg,refresh,0);CHECK(strstr(body,"\"recovered_items\":1"));free(body);
    snprintf(request_body,sizeof(request_body),"{\"contract_version\":1,\"op\":\"index_get\",\"job_id\":\"%s\"}",job);SourcesConfig offline=cfg;offline.root=NULL;body=call(&offline,request_body,0);CHECK(strstr(body,"\"cursor\":1")&&strstr(body,"\"indexed_items\":1")&&strstr(body,"\"recovered_items_total\":1"));CHECK(strstr(body,"\"pending_items\":0")&&strstr(body,"\"earliest_retry_unix_ms\":null"));free(body);
    free(job);char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child;CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    printf("PASS recovery transactions: %u checks; precommit cancellation/deadline, actual SIGKILL, root/directory replacement, retained cooldown/cursor and successful retry\n",checks);return 0;
}
