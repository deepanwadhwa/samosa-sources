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
static void retained(const char *state,size_t items,size_t cycles,size_t operations){
    char path[2048];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;
    CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    CHECK(scalar(db,"SELECT total FROM index_jobs")== (sqlite3_int64)items);
    CHECK(scalar(db,"SELECT count(*) FROM index_items")== (sqlite3_int64)items);
    CHECK(scalar(db,"SELECT coalesce(max(cycle),0) FROM index_discovery")== (sqlite3_int64)cycles);
    CHECK(scalar(db,"SELECT count(*) FROM index_operations")== (sqlite3_int64)operations);
    CHECK(sqlite3_close(db)==SQLITE_OK);
}
typedef struct {SourcesConfig *cfg;int deadline;} Control;
static int cancelled(void *context){
    Control *c=context;const char *marker=getenv("SAMOSA_DISCOVERY_TEST_PRECOMMIT_MARKER");
    if(!marker||access(marker,F_OK))return 0;
    if(c->deadline){c->cfg->deadline_monotonic=1;return 0;}
    return 1;
}
static pid_t spawn(const char *exe,const char *root,const char *state,const char *body,const char *mode,const char *marker,const char *release,const char *output){
    CHECK(!unlink(marker)||errno==ENOENT);CHECK(!unlink(release)||errno==ENOENT);
    CHECK(!setenv("SAMOSA_DISCOVERY_TEST_PRECOMMIT_MARKER",marker,1));CHECK(!setenv("SAMOSA_DISCOVERY_TEST_PRECOMMIT_RELEASE",release,1));
    char *args[]={(char*)exe,"--worker",(char*)root,(char*)state,(char*)body,(char*)mode,NULL};
    posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));CHECK(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,output,O_CREAT|O_WRONLY|O_TRUNC,0600));
    pid_t child;CHECK(!posix_spawnp(&child,exe,&actions,NULL,args,environ));CHECK(!posix_spawn_file_actions_destroy(&actions));
    CHECK(!unsetenv("SAMOSA_DISCOVERY_TEST_PRECOMMIT_MARKER"));CHECK(!unsetenv("SAMOSA_DISCOVERY_TEST_PRECOMMIT_RELEASE"));
    int ready=0;for(int i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec pause={0,10000000};nanosleep(&pause,NULL);}CHECK(ready);return child;
}
static void finished(pid_t child,const char *output,const char *code){
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==1);
    FILE *f=fopen(output,"r");CHECK(f);char body[2048];size_t n=fread(body,1,sizeof(body)-1,f);CHECK(!ferror(f));CHECK(!fclose(f));body[n]=0;
    Json j;CHECK(json_parse(&j,body,n));CHECK(strstr(body,code));CHECK(!strstr(body,"total_items"));
}
int main(int argc,char **argv){
    if(argc==6&&!strcmp(argv[1],"--worker")){
        SourcesConfig cfg={.root=argv[2],.state=argv[3]};Control control={.cfg=&cfg,.deadline=!strcmp(argv[5],"deadline")};
        if(strcmp(argv[5],"plain")){cfg.cancelled=cancelled;cfg.cancellation=&control;}
        return sources_dispatch(&cfg,argv[4],strlen(argv[4]),stdout);
    }
    char fixture[]="/tmp/samosa-discovery-XXXXXX";CHECK(mkdtemp(fixture));
    char root[1024],state[1024],path[2048],marker[1024],release[1024],output[1024],old[1024];
    snprintf(root,sizeof(root),"%s/root",fixture);snprintf(state,sizeof(state),"%s/state",fixture);CHECK(!mkdir(root,0700));
    snprintf(marker,sizeof(marker),"%s/marker",fixture);snprintf(release,sizeof(release),"%s/release",fixture);snprintf(output,sizeof(output),"%s/output",fixture);snprintf(old,sizeof(old),"%s/original-root",fixture);
    snprintf(path,sizeof(path),"%s/first.txt",root);file(path);SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"create\"}",0);
    Json j;CHECK(json_parse(&j,body,strlen(body)));char *job=json_string(&j,json_get(&j,0,"job_id"));CHECK(job&&strlen(job)==64);free(body);
    snprintf(path,sizeof(path),"%s/new.txt",root);file(path);
    char *input=request(job,"cancel");pid_t child=spawn(argv[0],root,state,input,"cancel",marker,release,output);file(release);finished(child,output,"cancelled");retained(state,1,0,1);free(input);
    input=request(job,"deadline");child=spawn(argv[0],root,state,input,"deadline",marker,release,output);file(release);finished(child,output,"deadline_exceeded");retained(state,1,0,1);free(input);
    input=request(job,"crash");child=spawn(argv[0],root,state,input,"plain",marker,release,output);CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);retained(state,1,0,1);
    body=call(&cfg,input,0);CHECK(strstr(body,"\"cycle\":1"));free(body);retained(state,2,1,2);free(input);
    snprintf(path,sizeof(path),"%s/third.txt",root);file(path);
    input=request(job,"crash-update");child=spawn(argv[0],root,state,input,"plain",marker,release,output);CHECK(!kill(child,SIGKILL));CHECK(waitpid(child,&status,0)==child);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);free(input);retained(state,2,1,2);
    input=request(job,"root-race");child=spawn(argv[0],root,state,input,"plain",marker,release,output);
    CHECK(!rename(root,old));CHECK(!mkdir(root,0700));file(release);finished(child,output,"source_changed");retained(state,2,1,2);CHECK(!rmdir(root));CHECK(!rename(old,root));
    body=call(&cfg,input,0);CHECK(strstr(body,"\"cycle\":2"));free(body);free(input);retained(state,3,2,3);
    for(size_t i=0;i<253;i++){snprintf(path,sizeof(path),"%s/quota-%03zu.txt",root,i);file(path);}
    input=request(job,"fill");body=call(&cfg,input,0);free(body);free(input);retained(state,256,3,4);
    snprintf(path,sizeof(path),"%s/first.txt",root);CHECK(!unlink(path));snprintf(path,sizeof(path),"%s/overflow.txt",root);file(path);
    input=request(job,"overflow");body=call(&cfg,input,1);CHECK(strstr(body,"index_limit"));free(body);free(input);retained(state,256,3,4);
    free(job);
    char folder[1024],old_folder[1024];snprintf(folder,sizeof(folder),"%s/root/folder",fixture);snprintf(old_folder,sizeof(old_folder),"%s/old-folder",fixture);CHECK(!mkdir(folder,0700));
    snprintf(state,sizeof(state),"%s/folder-state",fixture);cfg.state=state;snprintf(path,sizeof(path),"%s/first.txt",folder);file(path);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"folder\",\"operation_id\":\"create\"}",0);
    CHECK(json_parse(&j,body,strlen(body)));job=json_string(&j,json_get(&j,0,"job_id"));CHECK(job&&strlen(job)==64);free(body);
    snprintf(path,sizeof(path),"%s/new.txt",folder);file(path);input=request(job,"folder-race");child=spawn(argv[0],root,state,input,"plain",marker,release,output);
    CHECK(!rename(folder,old_folder));CHECK(!mkdir(folder,0700));file(release);finished(child,output,"source_changed");retained(state,1,0,1);CHECK(!rmdir(folder));CHECK(!rename(old_folder,folder));
    body=call(&cfg,input,0);CHECK(strstr(body,"\"cycle\":1"));free(body);free(input);retained(state,2,1,2);
    free(job);char *cleanup[]={"find",fixture,"-depth","-delete",NULL};CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    printf("PASS discovery transactions: %u checks; cancellation/deadline, precommit SIGKILL recovery, root/directory replacement and accumulated quota rollback\n",checks);return 0;
}
