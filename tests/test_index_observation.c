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
static sqlite3_int64 scalar(sqlite3 *db,const char *sql){sqlite3_stmt *stmt=NULL;CHECK(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK);CHECK(sqlite3_step(stmt)==SQLITE_ROW);sqlite3_int64 n=sqlite3_column_int64(stmt,0);CHECK(sqlite3_finalize(stmt)==SQLITE_OK);return n;}
static void retained(const char *state){
    char path[2048];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(scalar(db,"SELECT cursor FROM index_jobs")==1);
    CHECK(scalar(db,"SELECT count(*) FROM index_items WHERE status='indexed' AND text='7319 discovery'")==1);
    CHECK(scalar(db,"SELECT count(*) FROM index_versions")==0);
    CHECK(scalar(db,"SELECT revision FROM live_locators")==1);
    CHECK(scalar(db,"SELECT count(*) FROM index_pending")==0);
    CHECK(scalar(db,"SELECT count(*) FROM locator_operations")==0);
    CHECK(sqlite3_close(db)==SQLITE_OK);
}
typedef struct {SourcesConfig *cfg;int deadline;} Control;
static int cancelled(void *context){
    Control *c=context;const char *marker=getenv("SAMOSA_LOCATOR_TEST_PRECOMMIT_MARKER");
    if(!marker||access(marker,F_OK))return 0;
    if(c->deadline){c->cfg->deadline_monotonic=1;return 0;}
    return 1;
}
static pid_t spawn(const char *exe,const char *root,const char *state,const char *body,const char *mode,const char *marker,const char *release,const char *output){
    CHECK(!unlink(marker)||errno==ENOENT);CHECK(!unlink(release)||errno==ENOENT);
    CHECK(!setenv("SAMOSA_LOCATOR_TEST_PRECOMMIT_MARKER",marker,1));CHECK(!setenv("SAMOSA_LOCATOR_TEST_PRECOMMIT_RELEASE",release,1));
    char *args[]={(char*)exe,"--worker",(char*)root,(char*)state,(char*)body,(char*)mode,NULL};
    posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));CHECK(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,output,O_CREAT|O_WRONLY|O_TRUNC,0600));
    pid_t child;CHECK(!posix_spawnp(&child,exe,&actions,NULL,args,environ));CHECK(!posix_spawn_file_actions_destroy(&actions));
    CHECK(!unsetenv("SAMOSA_LOCATOR_TEST_PRECOMMIT_MARKER"));CHECK(!unsetenv("SAMOSA_LOCATOR_TEST_PRECOMMIT_RELEASE"));
    int ready=0;for(int i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec pause={0,10000000};nanosleep(&pause,NULL);}CHECK(ready);return child;
}
static void finished(pid_t child,const char *output,const char *code,int expected){
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==expected);
    FILE *f=fopen(output,"r");CHECK(f);char body[2048];size_t n=fread(body,1,sizeof(body)-1,f);CHECK(!ferror(f));CHECK(!fclose(f));body[n]=0;
    Json j;CHECK(json_parse(&j,body,n));CHECK(strstr(body,code));CHECK(!strstr(body,"total_items"));if(!expected)CHECK(strstr(body,"\"observed_items\":0"));
}
int main(int argc,char **argv){
    if(argc==6&&!strcmp(argv[1],"--worker")){
        SourcesConfig cfg={.root=argv[2],.state=argv[3],.discovery_interval_ms=100};Control control={.cfg=&cfg,.deadline=!strcmp(argv[5],"deadline")};
        if(strcmp(argv[5],"plain")){cfg.cancelled=cancelled;cfg.cancellation=&control;}
        return sources_dispatch(&cfg,argv[4],strlen(argv[4]),stdout);
    }
    char fixture[]="/tmp/samosa-freshness-control-XXXXXX";CHECK(mkdtemp(fixture));
    char root[1024],folder[1024],state[1024],path[2048],marker[1024],release[1024],output[1024],old[1024],request_body[512];
    snprintf(root,sizeof(root),"%s/root",fixture);snprintf(folder,sizeof(folder),"%s/root/folder",fixture);snprintf(state,sizeof(state),"%s/state",fixture);CHECK(!mkdir(root,0700));CHECK(!mkdir(folder,0700));
    snprintf(marker,sizeof(marker),"%s/marker",fixture);snprintf(release,sizeof(release),"%s/release",fixture);snprintf(output,sizeof(output),"%s/output",fixture);snprintf(old,sizeof(old),"%s/held",fixture);snprintf(path,sizeof(path),"%s/input.txt",folder);file(path);
    SourcesConfig cfg={.root=root,.state=state,.discovery_interval_ms=100};char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"folder\",\"operation_id\":\"create\"}",0);
    Json j;CHECK(json_parse(&j,body,strlen(body)));char *job=json_string(&j,json_get(&j,0,"job_id"));CHECK(job&&strlen(job)==64);free(body);
    snprintf(request_body,sizeof(request_body),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"initial\",\"max_items\":1}",job);body=call(&cfg,request_body,0);CHECK(strstr(body,"\"indexed_items\":1"));free(body);
    const char *refresh="{\"contract_version\":1,\"op\":\"index_refresh_step\"}";body=call(&cfg,refresh,0);CHECK(strstr(body,"\"observed_items\":1"));free(body);
    for(int attempt=0;attempt<7;attempt++){
        FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs("7419 new content",f)>=0);CHECK(!fclose(f));struct timespec delay={0,120000000};nanosleep(&delay,NULL);
        pid_t child=spawn(argv[0],root,state,refresh,attempt==0?"cancel":attempt==1?"deadline":"plain",marker,release,output);
        if(attempt==2){CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);}
        else {
            if(attempt==3)file(path);
            if(attempt==4){CHECK(!rename(root,old));CHECK(!mkdir(root,0700));}
            if(attempt==5){CHECK(!rename(folder,old));CHECK(!mkdir(folder,0700));}
            if(attempt==6)CHECK(!unlink(path));
            file(release);finished(child,output,attempt==0?"cancelled":attempt==1?"deadline_exceeded":"source_changed",attempt<2?1:0);
            if(attempt==4){CHECK(!rmdir(root));CHECK(!rename(old,root));}
            if(attempt==5){CHECK(!rmdir(folder));CHECK(!rename(old,folder));}
        }
        retained(state);
    }
    FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs("7419 accepted new",f)>=0);CHECK(!fclose(f));struct timespec delay={0,120000000};nanosleep(&delay,NULL);body=call(&cfg,refresh,0);CHECK(strstr(body,"\"observed_items\":1"));free(body);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",0);CHECK(strstr(body,"\"reindexed_items\":1"));free(body);
    snprintf(request_body,sizeof(request_body),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"7419\"}",job);SourcesConfig offline=cfg;offline.root=NULL;body=call(&offline,request_body,0);CHECK(strstr(body,"7419 accepted new")&&strstr(body,"available"));free(body);
    char db_path[2048];snprintf(db_path,sizeof(db_path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(db_path,&db)==SQLITE_OK);CHECK(scalar(db,"SELECT revision FROM live_locators")==2);CHECK(scalar(db,"SELECT count(*) FROM index_pending")==0);CHECK(scalar(db,"SELECT count(*) FROM index_versions")==1);CHECK(sqlite3_exec(db,"UPDATE live_locators SET revision=9223372036854775807",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    nanosleep(&delay,NULL);body=call(&cfg,refresh,1);CHECK(strstr(body,"locator_revision_limit"));free(body);
    free(job);char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child;CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    printf("PASS periodic observation transactions: %u checks; cancellation/deadline, SIGKILL, file/root/directory/missing races, atomic intent, retained evidence, retry and revision overflow\n",checks);return 0;
}
