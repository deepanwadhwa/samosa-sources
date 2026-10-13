#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "sources.h"
#include "json.h"
#include <assert.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static void tick(void){struct timespec wait={0,20000000};nanosleep(&wait,NULL);}
static char *call(SourcesConfig *cfg,const char *request){
    for(unsigned tries=0;tries<250;tries++){
        char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
        if(!rc)return body;
        if(!strstr(body,"\"busy\"")&&!strstr(body,"\"store_busy\"")){fprintf(stderr,"queue API: %s\n",body);assert(!rc);}free(body);tick();
    }
    assert(0);return NULL;
}
static char *selector(const char *body,const char *key){Json j;assert(json_parse(&j,body,strlen(body)));char *value=json_string(&j,json_get(&j,0,key));assert(value);return value;}
static size_t number(const char *body,const char *key){Json j;size_t n;assert(json_parse(&j,body,strlen(body))&&json_uint(&j,json_get(&j,0,key),&n));return n;}
static void file(const char *path,const char *text){FILE *out=fopen(path,"w");assert(out);assert(fputs(text,out)>=0&&!fclose(out));}
static pid_t spawn(const char *binary,SourcesConfig *cfg){char *args[]={(char*)binary,"--root",(char*)cfg->root,"--state",(char*)cfg->state,"--index-run","--index-interval-ms","100",NULL};posix_spawn_file_actions_t actions;assert(!posix_spawn_file_actions_init(&actions));assert(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0600));pid_t pid;assert(!posix_spawnp(&pid,binary,&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);return pid;}
static void exited(pid_t pid,int expected,int signal_number){int status=0,done=0;for(unsigned i=0;i<500;i++){pid_t result=waitpid(pid,&status,WNOHANG);assert(result>=0);if(result==pid){done=1;break;}tick();}assert(done);if(signal_number)assert(WIFSIGNALED(status)&&WTERMSIG(status)==signal_number);else assert(WIFEXITED(status)&&WEXITSTATUS(status)==expected);}
static void leased(SourcesConfig *cfg){char path[1024];snprintf(path,sizeof(path),"%s/reindex-runtime.lock",cfg->state);for(unsigned i=0;i<250;i++){int fd=open(path,O_RDWR|O_NOFOLLOW);if(fd>=0){int held=flock(fd,LOCK_EX|LOCK_NB)<0&&(errno==EAGAIN||errno==EWOULDBLOCK);close(fd);if(held)return;}tick();}assert(0);}
static void observe(SourcesConfig *cfg,const char *watch,size_t cycle){char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":%zu}",watch,cycle);char *body=call(cfg,request);free(body);}
static void candidates(SourcesConfig *cfg,const char *job,size_t expected){char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"new\"}",job);for(unsigned i=0;i<250;i++){char *body=call(cfg,request);size_t count=number(body,"returned_candidates");free(body);if(count==expected)return;tick();}assert(0);}
int main(int argc,char **argv){
    assert(argc==2);char fixture[]="/tmp/samosa-queue-runtime-XXXXXX";assert(mkdtemp(fixture));char root[256],state[256],a[512],b[512],request[1024];snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",fixture);snprintf(a,sizeof(a),"%s/a.txt",root);snprintf(b,sizeof(b),"%s/b.txt",root);file(a,"old code");file(b,"old code");SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"create\"}"),*job=selector(body,"job_id");free(body);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"initial\",\"max_items\":4}",job);body=call(&cfg,request);free(body);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"a.txt\"}");char *version=selector(body,"source_id");free(body);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch\",\"locators\":[{\"path\":\"a.txt\",\"expected_version\":\"%s\"},{\"path\":\"b.txt\",\"expected_version\":\"%s\"}]}",version,version);body=call(&cfg,request);char *watch=selector(body,"watch_id");free(body);assert(!unlink(a));file(b,"new code");observe(&cfg,watch,0);
    pid_t worker=spawn(argv[1],&cfg);leased(&cfg);pid_t duplicate=spawn(argv[1],&cfg);exited(duplicate,1,0);assert(!kill(worker,0));candidates(&cfg,job,1);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);body=call(&cfg,request);Json j;assert(json_parse(&j,body,strlen(body)));int rows=json_get(&j,0,"pending");size_t attempts;assert(number(body,"pending_items")==1&&json_uint(&j,json_get(&j,rows+1,"attempts"),&attempts)&&attempts>=1);free(body);
    assert(!kill(worker,SIGTERM));exited(worker,0,0);SourcesConfig offline=cfg;offline.root=NULL;body=call(&offline,request);assert(number(body,"pending_items")==1);free(body);
    file(a,"new code");observe(&cfg,watch,1);worker=spawn(argv[1],&cfg);leased(&cfg);candidates(&cfg,job,2);assert(!kill(worker,SIGKILL));exited(worker,0,SIGKILL);worker=spawn(argv[1],&cfg);leased(&cfg);candidates(&cfg,job,2);
    char held_root[256];snprintf(held_root,sizeof(held_root),"%s/held-root",fixture);assert(!rename(root,held_root)&&!mkdir(root,0700));exited(worker,1,0);body=call(&offline,request);assert(!number(body,"pending_items"));free(body);assert(!rmdir(root)&&!rename(held_root,root));
    worker=spawn(argv[1],&cfg);leased(&cfg);char held_state[256];snprintf(held_state,sizeof(held_state),"%s/held-state",fixture);assert(!rename(state,held_state)&&!mkdir(state,0700));exited(worker,1,0);char fresh_database[512];snprintf(fresh_database,sizeof(fresh_database),"%s/sources.sqlite3",state);assert(access(fresh_database,F_OK)<0&&errno==ENOENT);SourcesConfig retained=cfg;retained.state=held_state;retained.root=NULL;candidates(&retained,job,2);
    assert(!rmdir(state)&&!rename(held_state,state));worker=spawn(argv[1],&cfg);leased(&cfg);char lease_path[512];snprintf(lease_path,sizeof(lease_path),"%s/reindex-runtime.lock",state);assert(!unlink(lease_path));exited(worker,1,0);worker=spawn(argv[1],&cfg);leased(&cfg);assert(!kill(worker,SIGINT));exited(worker,0,0);
    free(job);free(version);free(watch);puts("PASS queue runtime: actual CLI duplicate refusal, missing/healthy progress, TERM/INT, SIGKILL/restart, root/state/lease replacement and retained evidence");return 0;
}
