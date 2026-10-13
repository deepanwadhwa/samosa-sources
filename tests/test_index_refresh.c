#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "sources.h"
#include "json.h"
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void tick(void){struct timespec delay={0,20000000};nanosleep(&delay,NULL);}
static void file(const char *path,const char *text){FILE *out=fopen(path,"w");CHECK(out);CHECK(fputs(text,out)>=0);CHECK(!fclose(out));}
static char *call(const SourcesConfig *cfg,const char *request,int expected){
    for(int i=0;i<250;i++){
        char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);int rc=sources_dispatch(cfg,request,strlen(request),out);CHECK(!fclose(out));
        if(rc!=expected&&(strstr(body,"\"busy\"")||strstr(body,"\"store_busy\""))){free(body);tick();continue;}
        if(rc!=expected)fprintf(stderr,"API: %s\n",body);
        CHECK(rc==expected);Json j;CHECK(json_parse(&j,body,n));return body;
    }
    CHECK(0);return NULL;
}
static size_t number(const char *body,const char *key){Json j;size_t n;CHECK(json_parse(&j,body,strlen(body)));CHECK(json_uint(&j,json_get(&j,0,key),&n));return n;}
static char *create(SourcesConfig *cfg,const char *path,const char *operation){
    char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"%s\",\"operation_id\":\"%s\"}",path,operation);
    char *body=call(cfg,request,0);Json j;CHECK(json_parse(&j,body,strlen(body)));char *job=json_string(&j,json_get(&j,0,"job_id"));CHECK(job&&strlen(job)==64);free(body);return job;
}
static char *get(SourcesConfig *cfg,const char *job){char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_get\",\"job_id\":\"%s\"}",job);return call(cfg,request,0);}
static void refreshed(SourcesConfig *cfg){char *body=call(cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",0);CHECK(number(body,"scanned_jobs")<=1);CHECK(number(body,"processed_items")<=1);CHECK(number(body,"recovered_items")<=1);CHECK(number(body,"observed_items")<=1);free(body);}
static sqlite3_int64 scalar(sqlite3 *db,const char *sql){sqlite3_stmt *stmt=NULL;CHECK(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)==SQLITE_OK);CHECK(sqlite3_step(stmt)==SQLITE_ROW);sqlite3_int64 n=sqlite3_column_int64(stmt,0);CHECK(sqlite3_finalize(stmt)==SQLITE_OK);return n;}
static sqlite3 *database(SourcesConfig *cfg){char path[2048];snprintf(path,sizeof(path),"%s/sources.sqlite3",cfg->state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_busy_timeout(db,1000)==SQLITE_OK);return db;}
static pid_t spawn(const char *binary,SourcesConfig *cfg){
    char *args[]={(char*)binary,"--root",(char*)cfg->root,"--state",(char*)cfg->state,"--index-run","--index-interval-ms","100","--discovery-interval-ms","100",NULL};
    posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));CHECK(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0600));pid_t child;CHECK(!posix_spawnp(&child,binary,&actions,NULL,args,environ));CHECK(!posix_spawn_file_actions_destroy(&actions));return child;
}
static void exited(pid_t child,int expected,int signal_number){int status=0,done=0;for(int i=0;i<500;i++){pid_t rc=waitpid(child,&status,WNOHANG);CHECK(rc>=0);if(rc==child){done=1;break;}tick();}CHECK(done);if(signal_number)CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==signal_number);else CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==expected);}
static void leased(SourcesConfig *cfg){char path[2048];snprintf(path,sizeof(path),"%s/reindex-runtime.lock",cfg->state);for(int i=0;i<250;i++){int fd=open(path,O_RDWR|O_NOFOLLOW);if(fd>=0){int held=flock(fd,LOCK_EX|LOCK_NB)<0&&(errno==EAGAIN||errno==EWOULDBLOCK);close(fd);if(held)return;}tick();}CHECK(0);}
static void query_candidates(SourcesConfig *cfg,const char *job,const char *query,size_t expected){
    char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"%s\"}",job,query);
    for(int i=0;i<250;i++){char *body=call(cfg,request,0);size_t n=number(body,"returned_candidates");free(body);if(n==expected)return;tick();}CHECK(0);
}
static void candidates(SourcesConfig *cfg,const char *job,size_t expected){query_candidates(cfg,job,"7319",expected);}
static void pending(SourcesConfig *cfg,const char *job,size_t expected){
    char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);
    for(int i=0;i<250;i++){char *body=call(cfg,request,0);size_t n=number(body,"pending_items");free(body);if(n==expected)return;tick();}CHECK(0);
}
int main(int argc,char **argv){
    CHECK(argc==2);char fixture[]="/tmp/samosa-refresh-XXXXXX";CHECK(mkdtemp(fixture));char root[1024],state[1024],bad[1024],good[1024],held[1024],path[2048];
    snprintf(root,sizeof(root),"%s/root",fixture);snprintf(state,sizeof(state),"%s/state",fixture);snprintf(bad,sizeof(bad),"%s/root/missing",fixture);snprintf(good,sizeof(good),"%s/root/healthy",fixture);snprintf(held,sizeof(held),"%s/held",fixture);CHECK(!mkdir(root,0700));CHECK(!mkdir(bad,0700));CHECK(!mkdir(good,0700));
    snprintf(path,sizeof(path),"%s/first.txt",bad);file(path,"7319 missing");snprintf(path,sizeof(path),"%s/first.txt",good);file(path,"7319 healthy");SourcesConfig cfg={.root=root,.state=state,.discovery_interval_ms=100};
    char *bad_job=create(&cfg,"missing","bad"),*job=create(&cfg,"healthy","healthy");CHECK(!rename(bad,held));refreshed(&cfg);refreshed(&cfg);
    char *body=get(&cfg,job);CHECK(number(body,"indexed_items")==1);free(body);sqlite3 *db=database(&cfg);CHECK(scalar(db,"SELECT count(*) FROM index_refresh_schedule")==2);CHECK(scalar(db,"SELECT count(*) FROM index_refresh_schedule WHERE scan_failures>0 AND initial_failures>0")==1);CHECK(sqlite3_close(db)==SQLITE_OK);
    snprintf(path,sizeof(path),"%s/new.txt",good);file(path,"7319 new");int ready=0;for(int i=0;i<250;i++){refreshed(&cfg);body=get(&cfg,job);ready=number(body,"indexed_items")==2;free(body);if(ready)break;tick();}CHECK(ready);
    cfg.discovery_interval_ms=60000;for(int i=0;i<520;i++)refreshed(&cfg);db=database(&cfg);CHECK(scalar(db,"SELECT count(*) FROM index_operations")==2);CHECK(scalar(db,"SELECT count(*) FROM index_refresh_schedule")==2);
    CHECK(sqlite3_exec(db,"UPDATE index_refresh_schedule SET initial_turn=9223372036854775807",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",1);CHECK(strstr(body,"scheduler_limit"));free(body);
    cfg.discovery_interval_ms=0;body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",1);CHECK(strstr(body,"discovery_disabled"));free(body);cfg.discovery_interval_ms=99;body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",1);CHECK(strstr(body,"invalid_configuration"));free(body);free(job);free(bad_job);
    char recovery_root[1024],request[512];snprintf(recovery_root,sizeof(recovery_root),"%s/recovery",fixture);CHECK(!mkdir(recovery_root,0700));snprintf(state,sizeof(state),"%s/recovery-state",fixture);cfg.root=recovery_root;cfg.state=state;cfg.discovery_interval_ms=100;
    snprintf(path,sizeof(path),"%s/a.txt",recovery_root);file(path,"7319 original a");snprintf(path,sizeof(path),"%s/b.txt",recovery_root);file(path,"7319 original b");snprintf(path,sizeof(path),"%s/c.txt",recovery_root);file(path,"7319 retained c");snprintf(path,sizeof(path),"%s/d.txt",recovery_root);CHECK(!mkdir(path,0700));job=create(&cfg,"","recovery-create");
    snprintf(path,sizeof(path),"%s/a.txt",recovery_root);CHECK(!unlink(path));snprintf(path,sizeof(path),"%s/b.txt",recovery_root);file(path,"\xff");
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"fail-initial\",\"max_items\":4}",job);body=call(&cfg,request,0);free(body);body=get(&cfg,job);CHECK(number(body,"cursor")==4&&number(body,"failed_items")==2&&number(body,"indexed_items")==1);free(body);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",0);CHECK(strstr(body,"invalid_utf8"));CHECK(number(body,"recovered_items")==0);free(body);
    db=database(&cfg);CHECK(scalar(db,"SELECT count(*) FROM index_recovery")==1);CHECK(scalar(db,"SELECT failures FROM index_recovery")==1);sqlite3_int64 cooldown=scalar(db,"SELECT next_ms FROM index_recovery");CHECK(sqlite3_close(db)==SQLITE_OK);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_discover\",\"job_id\":\"%s\",\"operation_id\":\"same-target\"}",job);body=call(&cfg,request,0);free(body);db=database(&cfg);CHECK(scalar(db,"SELECT failures FROM index_recovery")==1&&scalar(db,"SELECT next_ms FROM index_recovery")==cooldown);CHECK(sqlite3_close(db)==SQLITE_OK);
    snprintf(path,sizeof(path),"%s/a.txt",recovery_root);file(path,"7319 recovered a");snprintf(path,sizeof(path),"%s/b.txt",recovery_root);file(path,"7319 recovered b");snprintf(path,sizeof(path),"%s/d.txt",recovery_root);CHECK(!rmdir(path));file(path,"7319 recovered d");snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_discover\",\"job_id\":\"%s\",\"operation_id\":\"changed-target\"}",job);body=call(&cfg,request,0);free(body);
    db=database(&cfg);CHECK(scalar(db,"SELECT count(*) FROM index_recovery WHERE next_ms=0 AND failures=0")==3);CHECK(sqlite3_close(db)==SQLITE_OK);refreshed(&cfg);refreshed(&cfg);refreshed(&cfg);body=get(&cfg,job);CHECK(number(body,"cursor")==4&&number(body,"indexed_items")==4&&number(body,"failed_items")==0);CHECK(number(body,"recovered_items_total")==3&&strstr(body,"\"content_coverage\":\"partial\""));free(body);db=database(&cfg);CHECK(scalar(db,"SELECT count(*) FROM index_recovery")==0);CHECK(sqlite3_close(db)==SQLITE_OK);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"7319\"}",job);SourcesConfig recovered_offline=cfg;recovered_offline.root=NULL;body=call(&recovered_offline,request,0);CHECK(strstr(body,"recovered a")&&strstr(body,"recovered b")&&strstr(body,"retained c")&&strstr(body,"recovered d"));free(body);free(job);
    snprintf(state,sizeof(state),"%s/runtime-state",fixture);cfg.root=good;cfg.state=state;cfg.discovery_interval_ms=100;job=create(&cfg,"","runtime");pid_t worker=spawn(argv[1],&cfg);leased(&cfg);pid_t duplicate=spawn(argv[1],&cfg);exited(duplicate,1,0);candidates(&cfg,job,2);
    snprintf(path,sizeof(path),"%s/later.txt",good);file(path,"7319 later");candidates(&cfg,job,3);CHECK(!kill(worker,SIGKILL));exited(worker,0,SIGKILL);
    snprintf(path,sizeof(path),"%s/restarted.txt",good);file(path,"7319 restart");worker=spawn(argv[1],&cfg);leased(&cfg);candidates(&cfg,job,4);
    snprintf(path,sizeof(path),"%s/first.txt",good);struct stat original,current;CHECK(!stat(path,&original));file(path,"7419 healthy");
#ifdef __APPLE__
    struct timespec times[2]={original.st_atimespec,original.st_mtimespec};
#else
    struct timespec times[2]={original.st_atim,original.st_mtim};
#endif
    CHECK(!utimensat(AT_FDCWD,path,times,0));CHECK(!stat(path,&current));CHECK(current.st_ino==original.st_ino&&current.st_size==original.st_size);
#ifdef __APPLE__
    CHECK(current.st_mtimespec.tv_sec==original.st_mtimespec.tv_sec&&current.st_mtimespec.tv_nsec==original.st_mtimespec.tv_nsec);
#else
    CHECK(current.st_mtim.tv_sec==original.st_mtim.tv_sec&&current.st_mtim.tv_nsec==original.st_mtim.tv_nsec);
#endif
    query_candidates(&cfg,job,"7419",1);snprintf(path,sizeof(path),"%s/new.txt",good);CHECK(!unlink(path));pending(&cfg,job,1);file(path,"7419 restored");query_candidates(&cfg,job,"7419",2);pending(&cfg,job,0);
    CHECK(!kill(worker,SIGTERM));exited(worker,0,0);
    db=database(&cfg);CHECK(scalar(db,"SELECT count(*) FROM index_operations")==1);CHECK(scalar(db,"SELECT count(*) FROM index_refresh_schedule")==1);CHECK(scalar(db,"SELECT count(*) FROM index_freshness_schedule")==1);CHECK(scalar(db,"SELECT count(*) FROM locator_operations")==0);CHECK(scalar(db,"SELECT count(*) FROM index_versions")==2);CHECK(scalar(db,"SELECT count(*) FROM index_items WHERE text LIKE '7319%'")==4);CHECK(sqlite3_close(db)==SQLITE_OK);SourcesConfig offline=cfg;offline.root=NULL;candidates(&offline,job,4);query_candidates(&offline,job,"7419",2);free(job);
    char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child;CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));exited(child,0,0);
    printf("PASS automatic refresh: %u checks; bounded scan/capture/recovery, failed targets/cooldowns, missing/healthy fairness, >512 receipt-free ticks, scheduler bounds, real worker new files and restored-mtime changes, missing/restored files, duplicate refusal, SIGKILL/restart, shutdown and offline evidence\n",checks);return 0;
}
