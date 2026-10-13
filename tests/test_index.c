#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "sources.h"
#include "json.h"
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <sqlite3.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *call(const SourcesConfig *cfg,const char *request,int expected){char *out=NULL;size_t n=0;FILE *stream=open_memstream(&out,&n);CHECK(stream);CHECK(sources_dispatch(cfg,request,strlen(request),stream)==expected);CHECK(!fclose(stream));Json j;CHECK(json_parse(&j,out,n));return out;}
static char *request(const char *op,const char *job,const char *operation,size_t batch){char *out=NULL;size_t n;FILE *stream=open_memstream(&out,&n);CHECK(stream);fprintf(stream,"{\"contract_version\":1,\"op\":\"%s\"",op);if(job)fprintf(stream,",\"job_id\":\"%s\"",job);if(operation)fprintf(stream,",\"operation_id\":\"%s\"",operation);if(!strcmp(op,"index_create"))fputs(",\"path\":\"\"",stream);if(batch)fprintf(stream,",\"max_items\":%zu",batch);if(!strcmp(op,"index_search"))fputs(",\"query\":\"7319\"",stream);fputc('}',stream);CHECK(!fclose(stream));return out;}
static char *job_id(const char *out){Json j;CHECK(json_parse(&j,out,strlen(out)));char *job=json_string(&j,json_get(&j,0,"job_id"));CHECK(job&&strlen(job)==64);return job;}
static size_t number(const char *out,const char *key){Json j;size_t value=0;CHECK(json_parse(&j,out,strlen(out)));CHECK(json_uint(&j,json_get(&j,0,key),&value));return value;}
static void file(const char *path,const char *value){FILE *stream=fopen(path,"w");CHECK(stream);CHECK(fputs(value,stream)>=0);CHECK(!fclose(stream));}
static void scan_report(const char *out,size_t cycle,size_t observed){
    Json j;size_t value=0;CHECK(json_parse(&j,out,strlen(out)));
    int result=json_get(&j,0,"result"),scan=json_get(&j,result>=0?result:0,"latest_discovery");
    CHECK(scan>=0&&j.tokens[scan].type==JT_OBJECT);
    CHECK(json_uint(&j,json_get(&j,scan,"cycle"),&value)&&value==cycle);
    CHECK(json_uint(&j,json_get(&j,scan,"observed_items"),&value)&&value==observed);
    char *freshness=json_string(&j,json_get(&j,scan,"freshness"));
    CHECK(freshness&&!strcmp(freshness,"captured_snapshot"));free(freshness);
    int coverage=json_get(&j,scan,"inventory_coverage");
    char *status=json_string(&j,json_get(&j,coverage,"status"));
    CHECK(status&&!strcmp(status,"complete"));free(status);
}
int main(int argc,char **argv){
    if(argc==6&&!strcmp(argv[1],"--worker")){SourcesConfig cfg={.root=argv[2],.state=argv[3]};char *input=request("index_step",argv[4],argv[5],4);int rc=sources_dispatch(&cfg,input,strlen(input),stdout);free(input);return rc;}
    char fixture[]="/tmp/samosa-native-index-XXXXXX";CHECK(mkdtemp(fixture));char root[1024],state[1024],path[2048],marker[1024],output[1024];snprintf(root,sizeof(root),"%s/root",fixture);snprintf(state,sizeof(state),"%s/state",fixture);CHECK(!mkdir(root,0700));SourcesConfig cfg={.root=root,.state=state};
    for(size_t i=0;i<3;i++){snprintf(path,sizeof(path),"%s/%zu.txt",root,i);file(path,"Access code 7319\n");}
    char *input=request("index_create",NULL,"create",0),*created=call(&cfg,input,0),*job=job_id(created);SourcesConfig offline=cfg;offline.root=NULL;char *out=call(&offline,input,0);CHECK(!strcmp(out,created));free(out);free(input);
    input=request("index_step",job,"crash",4);snprintf(marker,sizeof(marker),"%s/marker",fixture);snprintf(output,sizeof(output),"%s/output",fixture);CHECK(!setenv("SAMOSA_INDEX_TEST_COMMITTED_MARKER",marker,1));char *args[]={argv[0],"--worker",root,state,job,"crash",NULL};posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));CHECK(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,output,O_CREAT|O_WRONLY|O_TRUNC,0600));pid_t worker;CHECK(!posix_spawnp(&worker,argv[0],&actions,NULL,args,environ));posix_spawn_file_actions_destroy(&actions);CHECK(!unsetenv("SAMOSA_INDEX_TEST_COMMITTED_MARKER"));int ready=0;for(int i=0;i<500;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}CHECK(ready);
    char *get=request("index_get",job,NULL,0);out=call(&cfg,get,0);CHECK(number(out,"cursor")==1);CHECK(strstr(out,"\"latest_discovery\":null"));free(out);char *other=request("index_step",job,"busy",1);out=call(&cfg,other,1);CHECK(strstr(out,"busy"));free(out);free(other);CHECK(!kill(worker,SIGKILL));int status;CHECK(waitpid(worker,&status,0)==worker);CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    out=call(&cfg,get,0);CHECK(number(out,"cursor")==1);free(out);out=call(&offline,input,0);CHECK(strstr(out,"interrupted"));free(out);free(input);
    snprintf(path,sizeof(path),"%s/0.txt",root);CHECK(!unlink(path));
    input=request("index_step",job,"resume",4);out=call(&cfg,input,0);CHECK(strstr(out,"completed"));char *repeat=call(&offline,input,0);CHECK(!strcmp(out,repeat));free(repeat);free(out);free(input);
    input=request("index_search",job,NULL,0);out=call(&offline,input,0);CHECK(number(out,"returned_candidates")==3);CHECK(strstr(out,"captured_snapshot"));CHECK(strstr(out,"exhaustive_current_filesystem\":false"));free(out);free(input);
    SourcesConfig stranger=cfg;stranger.owner="other-owner";out=call(&stranger,get,1);CHECK(strstr(out,"unknown_index"));free(out);free(get);
    input=request("index_step",job,"resume",1);out=call(&cfg,input,1);CHECK(strstr(out,"idempotency_conflict"));free(out);free(input);
    input=request("index_step",job,"invalid",5);out=call(&cfg,input,1);CHECK(strstr(out,"invalid_request"));free(out);free(input);
    /* A captured metadata identity must refuse changed content before reading. */
    snprintf(path,sizeof(path),"%s/0.txt",root);file(path,"Access code 7319\n");
    input=request("index_create",NULL,"stale",0);out=call(&cfg,input,0);char *stale_job=job_id(out);free(out);free(input);for(size_t i=0;i<3;i++){snprintf(path,sizeof(path),"%s/%zu.txt",root,i);file(path,"changed longer content\n");}
    input=request("index_step",stale_job,"stale-step",4);out=call(&cfg,input,0);CHECK(strstr(out,"\"failed_items\":3"));CHECK(strstr(out,"\"content_coverage\":\"partial\""));free(out);free(input);input=request("index_search",stale_job,NULL,0);out=call(&cfg,input,0);CHECK(number(out,"returned_candidates")==0);free(out);free(input);free(stale_job);
    for(size_t i=2;i<9;i++){char operation[32];snprintf(operation,sizeof(operation),"quota-%zu",i);input=request("index_create",NULL,operation,0);out=call(&cfg,input,i==8?1:0);if(i==8)CHECK(strstr(out,"index_limit"));free(out);free(input);}
    /* Owner-local corruption fixture: text cannot diverge from its literal envelope. */
    snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE index_items SET text='forged 7319' WHERE status='indexed'",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    input=request("index_search",job,NULL,0);out=call(&offline,input,1);CHECK(strstr(out,"corrupt_index"));CHECK(!strstr(out,"candidates"));free(out);free(input);
    free(job);free(created);
    /* Fill the actual bounded text store through successful public admissions. */
    snprintf(root,sizeof(root),"%s/large",fixture);snprintf(state,sizeof(state),"%s/large-state",fixture);CHECK(!mkdir(root,0700));cfg.root=root;cfg.state=state;char *large=malloc(65537);CHECK(large);memset(large,'a',65536);large[65536]=0;
    for(size_t i=0;i<80;i++){snprintf(large,16,"7319-%04zu",i);large[strlen(large)]='a';snprintf(path,sizeof(path),"%s/%03zu.txt",root,i);file(path,large);}free(large);
    input=request("index_create",NULL,"large-create",0);out=call(&cfg,input,0);job=job_id(out);free(input);free(out);
    for(size_t i=0;i<20;i++){char operation[32];snprintf(operation,sizeof(operation),"large-%zu",i);input=request("index_step",job,operation,4);out=call(&cfg,input,0);free(input);free(out);}
    get=request("index_get",job,NULL,0);out=call(&cfg,get,0);size_t accepted=number(out,"indexed_items"),refused=number(out,"failed_items");CHECK(accepted>50&&accepted<64);CHECK(refused>0&&accepted+refused==80);CHECK(strstr(out,"\"content_coverage\":\"partial\""));free(get);free(out);
    input=request("index_search",job,NULL,0);out=call(&cfg,input,0);CHECK(number(out,"returned_candidates")==10);CHECK(strstr(out,"\"candidate_coverage\":\"partial\""));free(input);free(out);free(job);
    snprintf(root,sizeof(root),"%s/discovery",fixture);snprintf(state,sizeof(state),"%s/discovery-state",fixture);CHECK(!mkdir(root,0700));cfg.root=root;cfg.state=state;snprintf(path,sizeof(path),"%s/first.txt",root);file(path,"7319 original");input=request("index_create",NULL,"discovery-create",0);out=call(&cfg,input,0);job=job_id(out);free(input);free(out);input=request("index_step",job,"discovery-initial",4);out=call(&cfg,input,0);free(input);free(out);
    snprintf(path,sizeof(path),"%s/new.txt",root);file(path,"7319 newly discovered");input=request("index_discover",job,"discover-new",0);out=call(&cfg,input,0);Json discovery;CHECK(json_parse(&discovery,out,strlen(out)));int result=json_get(&discovery,0,"result");size_t total=0;CHECK(json_uint(&discovery,json_get(&discovery,result,"total_items"),&total)&&total==2);scan_report(out,1,2);char *saved=strdup(out);CHECK(saved);free(out);offline=cfg;offline.root=NULL;out=call(&offline,input,0);CHECK(!strcmp(out,saved));free(out);free(saved);free(input);
    input=request("index_step",job,"discovery-step",4);out=call(&cfg,input,0);free(input);free(out);get=request("index_get",job,NULL,0);out=call(&cfg,get,0);CHECK(number(out,"indexed_items")==2&&strstr(out,"\"content_coverage\":\"partial\""));free(out);free(get);input=request("index_search",job,NULL,0);out=call(&cfg,input,0);CHECK(strstr(out,"newly discovered"));free(input);free(out);
    CHECK(!unlink(path));
    input=request("index_discover",job,"discover-repeat",0);out=call(&cfg,input,0);scan_report(out,2,1);saved=strdup(out);CHECK(saved);free(out);
    get=request("index_get",job,NULL,0);out=call(&offline,get,0);CHECK(number(out,"total_items")==2);scan_report(out,2,1);free(out);
    stranger=cfg;stranger.owner="other-owner";out=call(&stranger,get,1);CHECK(strstr(out,"unknown_index"));free(out);
    snprintf(path,sizeof(path),"%s/sources.sqlite3",state);CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE index_discovery SET digest=printf('%064d',0)",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    out=call(&offline,get,1);CHECK(strstr(out,"corrupt_index"));CHECK(!strstr(out,"total_items"));free(out);free(get);
    out=call(&offline,input,0);CHECK(!strcmp(out,saved));free(out);free(saved);free(input);
    input=request("index_discover",job,"discover-corrupt",0);out=call(&cfg,input,1);CHECK(strstr(out,"corrupt_index"));free(out);free(input);free(job);
    char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child;CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);printf("PASS native index: %u checks; SIGKILL recovery, owner/retry/stale/corruption, quota %zu accepted/%zu refused\n",checks,accepted,refused);return 0;
}
