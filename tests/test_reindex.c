#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "sources.h"
#include "json.h"
#include <assert.h>
#include <sqlite3.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static char *call(const SourcesConfig *cfg,const char *request,int expected){
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
    if(expected>=0&&rc!=expected){fprintf(stderr,"reindex unexpected %d: %s\n",rc,body);}
    assert(expected<0||rc==expected);Json j;assert(json_parse(&j,body,n));return body;
}
static char *selector(const char *body,const char *key){Json j;assert(json_parse(&j,body,strlen(body)));char *value=json_string(&j,json_get(&j,0,key));assert(value);return value;}
static size_t number(const char *body,const char *key){Json j;size_t n;assert(json_parse(&j,body,strlen(body))&&json_uint(&j,json_get(&j,0,key),&n));return n;}
static void file(const char *path,const char *value){FILE *out=fopen(path,"w");assert(out);assert(fputs(value,out)>=0&&!fclose(out));}
static char *step(const SourcesConfig *cfg,const char *job,const char *operation,int rc,size_t batch){char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"%s\",\"max_items\":%zu}",job,operation,batch);return call(cfg,request,rc);}
static size_t pending(const SourcesConfig *cfg,const char *job){char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);SourcesConfig offline=*cfg;offline.root=NULL;char *body=call(&offline,request,0);size_t n=number(body,"pending_items");free(body);return n;}
static char *search(const SourcesConfig *cfg,const char *job,const char *query){char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"%s\"}",job,query);return call(cfg,request,0);}
static void observe(const SourcesConfig *cfg,const char *watch,size_t cycle){char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":%zu}",watch,cycle);char *body=call(cfg,request,0);free(body);}
static char *queue_step(const SourcesConfig *cfg,int rc){return call(cfg,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",rc);}
static size_t retry_status(SourcesConfig *cfg,const char *job,size_t *next){
    char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);SourcesConfig offline=*cfg;offline.root=NULL;char *body=call(&offline,request,0);Json j;size_t failures;assert(json_parse(&j,body,strlen(body)));int rows=json_get(&j,0,"pending");assert(rows>=0&&j.tokens[rows].count==1&&json_uint(&j,json_get(&j,rows+1,"backoff_level"),&failures)&&json_uint(&j,json_get(&j,rows+1,"next_attempt_at_ms"),next));free(body);return failures;
}
static size_t milliseconds(void){struct timespec now;assert(!clock_gettime(CLOCK_REALTIME,&now));return (size_t)now.tv_sec*1000+(size_t)now.tv_nsec/1000000;}
static void across_jobs(void){
    char fixture[]="/tmp/samosa-reindex-jobs-XXXXXX";assert(mkdtemp(fixture));char root[256],state[256],one[512],two[512],a[1024],b[1024],request[1024];
    snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",fixture);snprintf(one,sizeof(one),"%s/one",root);snprintf(two,sizeof(two),"%s/two",root);assert(!mkdir(one,0700)&&!mkdir(two,0700));snprintf(a,sizeof(a),"%s/a.txt",one);snprintf(b,sizeof(b),"%s/b.txt",two);file(a,"old code");file(b,"old code");SourcesConfig cfg={.root=root,.state=state};
    char *jobs[2];const char *paths[]={"one","two"};
    for(unsigned i=0;i<2;i++){snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"%s\",\"operation_id\":\"create-%u\"}",paths[i],i);char *created=call(&cfg,request,0);jobs[i]=selector(created,"job_id");free(created);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"initial-%u\",\"max_items\":4}",jobs[i],i);created=call(&cfg,request,0);free(created);}
    char *body=search(&cfg,jobs[0],"old");Json j;assert(json_parse(&j,body,strlen(body)));int rows=json_get(&j,0,"candidates");char *version=json_string(&j,json_get(&j,rows+1,"version"));assert(version);free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch\",\"locators\":[{\"path\":\"one/a.txt\",\"expected_version\":\"%s\"},{\"path\":\"two/b.txt\",\"expected_version\":\"%s\"}]}",version,version);body=call(&cfg,request,0);char *watch=selector(body,"watch_id");free(body);
    unsigned missing=strcmp(jobs[0],jobs[1])<0?0:1,healthy=1-missing;char *files[]={a,b};assert(!unlink(files[missing]));file(files[healthy],"new code");observe(&cfg,watch,0);assert(pending(&cfg,jobs[0])==1&&pending(&cfg,jobs[1])==1);
    SourcesConfig foreign=cfg;foreign.owner="foreign";body=queue_step(&foreign,0);assert(strstr(body,"\"idle\":true")&&!strstr(body,jobs[0])&&!strstr(body,jobs[1]));free(body);foreign=cfg;foreign.host_id="other-host";body=queue_step(&foreign,0);assert(strstr(body,"\"idle\":true"));free(body);
    char held[256];snprintf(held,sizeof(held),"%s/held",fixture);assert(!rename(root,held)&&!mkdir(root,0700));body=queue_step(&cfg,0);assert(strstr(body,"\"idle\":true"));free(body);assert(!rmdir(root)&&!rename(held,root));
    body=queue_step(&cfg,1);assert(strstr(body,"live_unavailable"));free(body);observe(&cfg,watch,1);body=queue_step(&cfg,0);assert(number(body,"reindexed_items")==1);char *selected=selector(body,"job_id");assert(!strcmp(selected,jobs[healthy]));free(selected);free(body);assert(pending(&cfg,jobs[missing])==1&&!pending(&cfg,jobs[healthy]));
    body=search(&cfg,jobs[healthy],"old");assert(number(body,"returned_candidates")==1);free(body);body=search(&cfg,jobs[healthy],"new");assert(number(body,"returned_candidates")==1);free(body);
    size_t next_retry;assert(retry_status(&cfg,jobs[missing],&next_retry)==1&&next_retry>milliseconds());size_t retained_retry=next_retry;
    body=step(&cfg,jobs[missing],"explicit-retry",1,1);assert(strstr(body,"live_unavailable"));free(body);assert(retry_status(&cfg,jobs[missing],&next_retry)==1&&next_retry==retained_retry);
    observe(&cfg,watch,2);assert(retry_status(&cfg,jobs[missing],&next_retry)==1&&next_retry==retained_retry);
    /* Real clock expiry triggers a second attempt and a doubled delay. */
    size_t second_before=0,second_after=0;int retried=0;for(unsigned i=0;i<300;i++){second_before=milliseconds();body=queue_step(&cfg,-1);second_after=milliseconds();if(strstr(body,"live_unavailable")){retried=1;free(body);break;}assert(strstr(body,"\"idle\":true"));free(body);struct timespec wait={0,20000000};nanosleep(&wait,NULL);}assert(retried);assert(retry_status(&cfg,jobs[missing],&next_retry)==2&&next_retry>=second_before+2000&&next_retry<=second_after+2000);
    char retry_database[512];snprintf(retry_database,sizeof(retry_database),"%s/sources.sqlite3",state);sqlite3 *retry_db=NULL;
    /* Expire owned fixture rows to qualify the long-delay cap without a minute wait. */
    for(unsigned failures=3;failures<=8;failures++){assert(sqlite3_open(retry_database,&retry_db)==SQLITE_OK);assert(sqlite3_exec(retry_db,"UPDATE index_retry SET next_ms=0",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(retry_db);size_t before=milliseconds();body=queue_step(&cfg,1);size_t after=milliseconds();assert(strstr(body,"live_unavailable"));free(body);size_t expected=failures>7?7:failures;assert(retry_status(&cfg,jobs[missing],&next_retry)==expected);size_t delay=expected==7?60000:((size_t)1000<<(expected-1));assert(next_retry>=before+delay&&next_retry<=after+delay);}
    assert(sqlite3_open(retry_database,&retry_db)==SQLITE_OK);assert(sqlite3_exec(retry_db,"UPDATE index_retry SET failures=8",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(retry_db);body=queue_step(&cfg,1);assert(strstr(body,"corrupt_index"));free(body);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",jobs[missing]);body=call(&cfg,request,1);assert(strstr(body,"corrupt_index")&&!strstr(body,"\"pending\""));free(body);assert(sqlite3_open(retry_database,&retry_db)==SQLITE_OK);assert(sqlite3_exec(retry_db,"UPDATE index_retry SET failures=7",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(retry_db);
    /* More automatic polls than the public operation cap consume no receipts. */
    for(unsigned i=0;i<520;i++){body=queue_step(&cfg,0);assert(strstr(body,"\"idle\":true"));free(body);}
    for(unsigned i=0;i<507;i++){char operation[64];snprintf(operation,sizeof(operation),"receipt-%u",i);body=step(&cfg,jobs[healthy],operation,0,1);free(body);}
    body=step(&cfg,jobs[healthy],"receipt-full",1,1);assert(strstr(body,"index_limit"));free(body);
    /* Automatic work still progresses when all client receipt slots are retained. */
    file(files[missing],"new code");observe(&cfg,watch,3);body=queue_step(&cfg,0);assert(number(body,"reindexed_items")==1);free(body);assert(!pending(&cfg,jobs[missing]));body=queue_step(&cfg,0);assert(strstr(body,"\"idle\":true"));free(body);
    /* A job that changes again immediately cannot overtake an older ready peer. */
    file(files[healthy],"hot code");file(files[missing],"hot code");observe(&cfg,watch,4);body=queue_step(&cfg,0);selected=selector(body,"job_id");assert(!strcmp(selected,jobs[healthy]));free(selected);free(body);
    file(files[healthy],"warmcode");observe(&cfg,watch,5);body=queue_step(&cfg,0);selected=selector(body,"job_id");assert(!strcmp(selected,jobs[missing]));free(selected);free(body);assert(!pending(&cfg,jobs[missing])&&pending(&cfg,jobs[healthy])==1);body=queue_step(&cfg,0);free(body);assert(!pending(&cfg,jobs[healthy]));
    char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_exec(db,"UPDATE index_schedule SET turn=-1",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(db);body=queue_step(&cfg,1);assert(strstr(body,"corrupt_index"));free(body);
    assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_exec(db,"UPDATE index_schedule SET turn=9223372036854775807",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(db);body=queue_step(&cfg,1);assert(strstr(body,"scheduler_limit"));free(body);
    free(jobs[0]);free(jobs[1]);free(version);free(watch);
}
static void slow_failure(const char *binary){
    char fixture[]="/tmp/samosa-retry-timing-XXXXXX";assert(mkdtemp(fixture));char root[256],state[256],path[512],request[1024],marker[256],release[256];snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",fixture);snprintf(path,sizeof(path),"%s/a.txt",root);file(path,"old code");SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"create\"}",0),*job=selector(body,"job_id");free(body);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"initial\",\"max_items\":1}",job);body=call(&cfg,request,0);free(body);body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"a.txt\"}",0);char *version=selector(body,"source_id");free(body);snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch\",\"locators\":[{\"path\":\"a.txt\",\"expected_version\":\"%s\"}]}",version);body=call(&cfg,request,0);char *watch=selector(body,"watch_id");free(body);assert(!unlink(path));observe(&cfg,watch,0);
    snprintf(marker,sizeof(marker),"%s/marker",fixture);snprintf(release,sizeof(release),"%s/release",fixture);assert(!setenv("SAMOSA_REINDEX_TEST_RETRY_MARKER",marker,1)&&!setenv("SAMOSA_REINDEX_TEST_RETRY_RELEASE",release,1));char *args[]={(char*)binary,"--queue-worker",root,state,NULL};posix_spawn_file_actions_t actions;assert(!posix_spawn_file_actions_init(&actions));assert(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0600));pid_t worker;assert(!posix_spawnp(&worker,binary,&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);assert(!unsetenv("SAMOSA_REINDEX_TEST_RETRY_MARKER")&&!unsetenv("SAMOSA_REINDEX_TEST_RETRY_RELEASE"));int ready=0;
    for(unsigned i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec wait={0,10000000};nanosleep(&wait,NULL);}assert(ready);size_t reserved;assert(retry_status(&cfg,job,&reserved)==1);
    struct timespec slow={1,500000000};nanosleep(&slow,NULL);size_t finished_before=milliseconds();file(release,"release");int status;assert(waitpid(worker,&status,0)==worker&&WIFEXITED(status)&&WEXITSTATUS(status)==1);size_t finished_after=milliseconds(),next;assert(retry_status(&cfg,job,&next)==1&&next>=finished_before+1000&&next<=finished_after+1000&&next>reserved);
    /* A killed admitted attempt retains its reservation without finalization. */
    char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_exec(db,"UPDATE index_retry SET next_ms=0",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(db);assert(!unlink(marker)&&!unlink(release));assert(!setenv("SAMOSA_REINDEX_TEST_RETRY_MARKER",marker,1)&&!setenv("SAMOSA_REINDEX_TEST_RETRY_RELEASE",release,1));assert(!posix_spawn_file_actions_init(&actions));assert(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0600));assert(!posix_spawnp(&worker,binary,&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);assert(!unsetenv("SAMOSA_REINDEX_TEST_RETRY_MARKER")&&!unsetenv("SAMOSA_REINDEX_TEST_RETRY_RELEASE"));ready=0;
    for(unsigned i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec wait={0,10000000};nanosleep(&wait,NULL);}assert(ready);assert(!kill(worker,SIGKILL));assert(waitpid(worker,&status,0)==worker&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);assert(retry_status(&cfg,job,&next)==2&&next>milliseconds());body=queue_step(&cfg,0);assert(strstr(body,"\"idle\":true"));free(body);assert(pending(&cfg,job)==1);free(job);free(version);free(watch);
}
static void fairness(void){
    char fixture[]="/tmp/samosa-reindex-fair-XXXXXX";assert(mkdtemp(fixture));char root[256],state[256],a[512],b[512],request[1024];
    snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",fixture);snprintf(a,sizeof(a),"%s/a.txt",root);snprintf(b,sizeof(b),"%s/b.txt",root);file(a,"old code");file(b,"old code");SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"create\"}",0),*job=selector(body,"job_id");free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"initial\",\"max_items\":4}",job);body=call(&cfg,request,0);free(body);
    body=search(&cfg,job,"old");Json j;assert(json_parse(&j,body,strlen(body)));int rows=json_get(&j,0,"candidates");char *version=json_string(&j,json_get(&j,rows+1,"version"));assert(version);free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch\",\"locators\":[{\"path\":\"a.txt\",\"expected_version\":\"%s\"},{\"path\":\"b.txt\",\"expected_version\":\"%s\"}]}",version,version);body=call(&cfg,request,0);char *watch=selector(body,"watch_id");free(body);
    assert(!unlink(a));file(b,"new code");observe(&cfg,watch,0);assert(pending(&cfg,job)==2);
    body=step(&cfg,job,"missing-first",1,1);assert(strstr(body,"live_unavailable"));free(body);
    /* Repeated watch observations must preserve the attempted item's priority. */
    observe(&cfg,watch,1);body=step(&cfg,job,"healthy-second",0,1);assert(strstr(body,"\"reindexed_items\":1"));free(body);assert(pending(&cfg,job)==1);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);SourcesConfig offline=cfg;offline.root=NULL;body=call(&offline,request,0);assert(json_parse(&j,body,strlen(body)));rows=json_get(&j,0,"pending");size_t attempts;assert(json_uint(&j,json_get(&j,rows+1,"attempts"),&attempts)&&attempts==1);free(body);
    body=step(&cfg,job,"missing-again",1,1);assert(strstr(body,"live_unavailable"));free(body);body=call(&offline,request,0);assert(json_parse(&j,body,strlen(body)));rows=json_get(&j,0,"pending");assert(json_uint(&j,json_get(&j,rows+1,"attempts"),&attempts)&&attempts==2);free(body);
    char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_exec(db,"UPDATE index_attempts SET attempts=-1",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(db);
    body=call(&offline,request,1);assert(strstr(body,"corrupt_index")&&!strstr(body,"\"pending\""));free(body);body=step(&cfg,job,"corrupt-counter",1,1);assert(strstr(body,"corrupt_index"));free(body);
    assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_exec(db,"UPDATE index_attempts SET attempts=2",NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(db);
    body=search(&offline,job,"new");assert(number(body,"returned_candidates")==1);free(body);
    file(a,"old code");observe(&cfg,watch,2);assert(!pending(&cfg,job));file(a,"new code");observe(&cfg,watch,3);body=call(&offline,request,0);assert(json_parse(&j,body,strlen(body)));rows=json_get(&j,0,"pending");assert(json_uint(&j,json_get(&j,rows+1,"attempts"),&attempts)&&attempts==0);free(body);body=step(&cfg,job,"restored",0,1);free(body);assert(!pending(&cfg,job));
    free(job);free(version);free(watch);
}
int main(int argc,char **argv){
    if(argc==4&&!strcmp(argv[1],"--queue-worker")){SourcesConfig cfg={.root=argv[2],.state=argv[3]};const char *request="{\"contract_version\":1,\"op\":\"index_queue_step\"}";return sources_dispatch(&cfg,request,strlen(request),stdout);}
    if(argc==6&&!strcmp(argv[1],"--worker")){SourcesConfig cfg={.root=argv[2],.state=argv[3]};char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"%s\",\"max_items\":4}",argv[4],argv[5]);return sources_dispatch(&cfg,request,strlen(request),stdout);}
    slow_failure(argv[0]);
    across_jobs();
    fairness();
    char fixture[]="/tmp/samosa-reindex-XXXXXX";assert(mkdtemp(fixture));char root[256],state[256],a[512],b[512],request[1024];
    snprintf(root,sizeof(root),"%s/root",fixture);assert(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",fixture);snprintf(a,sizeof(a),"%s/a.txt",root);snprintf(b,sizeof(b),"%s/b.txt",root);file(a,"old code");file(b,"old code");SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"create\"}",0),*job=selector(body,"job_id");free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"initial\",\"max_items\":4}",job);body=call(&cfg,request,0);free(body);
    body=search(&cfg,job,"old");Json j;assert(json_parse(&j,body,strlen(body)));int rows=json_get(&j,0,"candidates");char *version=json_string(&j,json_get(&j,rows+1,"version"));assert(version&&number(body,"returned_candidates")==2);free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch\",\"locators\":[{\"path\":\"a.txt\",\"expected_version\":\"%s\"},{\"path\":\"b.txt\",\"expected_version\":\"%s\"}]}",version,version);body=call(&cfg,request,0);char *watch=selector(body,"watch_id");free(body);
    file(a,"new code");file(b,"new code");observe(&cfg,watch,0);assert(pending(&cfg,job)==2);
    char marker[256],output[256];snprintf(marker,sizeof(marker),"%s/marker",fixture);snprintf(output,sizeof(output),"%s/output",fixture);assert(!setenv("SAMOSA_REINDEX_TEST_COMMITTED_MARKER",marker,1));char *args[]={argv[0],"--worker",root,state,job,"crash",NULL};posix_spawn_file_actions_t actions;assert(!posix_spawn_file_actions_init(&actions));assert(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,output,O_CREAT|O_WRONLY|O_TRUNC,0600));pid_t worker;assert(!posix_spawnp(&worker,argv[0],&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);assert(!unsetenv("SAMOSA_REINDEX_TEST_COMMITTED_MARKER"));int ready=0;
    for(int i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}assert(ready&&pending(&cfg,job)==1);
    body=step(&cfg,job,"competing",1,1);assert(strstr(body,"busy"));free(body);assert(!kill(worker,SIGKILL));int status;assert(waitpid(worker,&status,0)==worker&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    SourcesConfig offline=cfg;offline.root=NULL;body=step(&offline,job,"crash",0,4);assert(strstr(body,"interrupted"));free(body);
    body=step(&cfg,job,"resume",0,4);assert(strstr(body,"\"reindexed_items\":1"));char *repeat=step(&offline,job,"resume",0,4);assert(!strcmp(body,repeat));free(body);free(repeat);assert(!pending(&cfg,job));
    body=step(&cfg,job,"resume",1,1);assert(strstr(body,"idempotency_conflict"));free(body);
    body=search(&offline,job,"old");assert(number(body,"returned_candidates")==2&&strstr(body,"\"status\":\"changed\""));free(body);body=search(&offline,job,"new");assert(number(body,"returned_candidates")==2&&strstr(body,"\"status\":\"available\""));free(body);
    observe(&cfg,watch,1);assert(!pending(&cfg,job));
    file(a,"nextcode");observe(&cfg,watch,2);assert(pending(&cfg,job)==1);file(a,"latecode");body=step(&cfg,job,"stale",1,1);assert(strstr(body,"stale_source"));free(body);assert(pending(&cfg,job)==1);
    observe(&cfg,watch,3);body=step(&cfg,job,"new-observation",0,1);free(body);assert(!pending(&cfg,job));body=search(&cfg,job,"late");assert(number(body,"returned_candidates")==1);free(body);
    assert(!unlink(a));observe(&cfg,watch,4);body=step(&cfg,job,"missing",1,1);assert(strstr(body,"live_unavailable"));free(body);assert(pending(&cfg,job)==1);file(a,"latecode");observe(&cfg,watch,5);assert(!pending(&cfg,job));
    SourcesConfig foreign=cfg;foreign.owner="foreign";body=step(&foreign,job,"foreign",1,1);assert(strstr(body,"unknown_index"));free(body);
    /* A newer observation arriving after extraction must remain queued. */
    file(a,"nextcode");observe(&cfg,watch,6);assert(pending(&cfg,job)==1);
    char precommit[256],release[256];snprintf(precommit,sizeof(precommit),"%s/precommit",fixture);snprintf(release,sizeof(release),"%s/release",fixture);
    assert(!setenv("SAMOSA_REINDEX_TEST_PRECOMMIT_MARKER",precommit,1)&&!setenv("SAMOSA_REINDEX_TEST_PRECOMMIT_RELEASE",release,1));
    args[5]="racing-observation";assert(!posix_spawn_file_actions_init(&actions));assert(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,output,O_CREAT|O_WRONLY|O_TRUNC,0600));assert(!posix_spawnp(&worker,argv[0],&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);assert(!unsetenv("SAMOSA_REINDEX_TEST_PRECOMMIT_MARKER")&&!unsetenv("SAMOSA_REINDEX_TEST_PRECOMMIT_RELEASE"));ready=0;
    for(int waiting=0;waiting<500;waiting++){if(!access(precommit,F_OK)){ready=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}assert(ready);
    observe(&cfg,watch,7);file(release,"release");assert(waitpid(worker,&status,0)==worker&&WIFEXITED(status)&&WEXITSTATUS(status)==1);assert(pending(&cfg,job)==1);
    FILE *race_out=fopen(output,"r");assert(race_out);char race_body[1024];size_t race_bytes=fread(race_body,1,sizeof(race_body)-1,race_out);race_body[race_bytes]=0;assert(!fclose(race_out)&&strstr(race_body,"stale_observation"));
    body=search(&cfg,job,"nextcode");assert(number(body,"returned_candidates")==0);free(body);body=step(&cfg,job,"after-race",0,1);free(body);assert(!pending(&cfg,job));
    /* Public admissions exercise the retained revision bound using two snapshots. */
    for(size_t i=0;i<252;i++){file(a,i%2?"nextcode":"latecode");observe(&cfg,watch,8+i);char operation[64];snprintf(operation,sizeof(operation),"capacity-%zu",i);body=step(&cfg,job,operation,0,1);free(body);assert(!pending(&cfg,job));}
    file(a,"latecode");observe(&cfg,watch,260);body=step(&cfg,job,"capacity-refused",1,1);assert(strstr(body,"index_limit"));free(body);assert(pending(&cfg,job)==1);
    body=search(&offline,job,"old");assert(number(body,"returned_candidates")==2);free(body);
    free(job);free(version);free(watch);puts("PASS reindex: actual SIGKILL/resume, exact offline retry, retained evidence, stale/missing refusal, latest-head scheduling and public revision capacity");return 0;
}
