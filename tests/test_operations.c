#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "operations.h"
#include "json.h"
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL Source operations %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *call(const SourcesConfig *cfg,const char *request,int expected){char *result=NULL;size_t n;FILE *f=open_memstream(&result,&n);CHECK(f);int rc=sources_dispatch(cfg,request,strlen(request),f);CHECK(!fclose(f));if(rc!=expected)fprintf(stderr,"Source result %s",result);CHECK(rc==expected);Json j;CHECK(json_parse(&j,result,n));return result;}
static void public_request(char *request,size_t n,const char *op,const char *id){CHECK(snprintf(request,n,"{\"contract_version\":1,\"op\":\"%s\",\"operation_id\":\"%s\"}",op,id)>0);}
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return t.tv_sec+(double)t.tv_nsec/1e9;}
static void ready(const char *path,pid_t child){double end=now()+20;while(access(path,F_OK)){int status;CHECK(waitpid(child,&status,WNOHANG)==0);CHECK(now()<end);struct timespec delay={0,10000000};nanosleep(&delay,NULL);}}
static void release(const char *path){FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs("release",f)>=0);CHECK(!fclose(f));}
static pid_t worker(const char *self,const char *state,const char *root,const char *marker,const char *gate,const char *output,const char *id){char *args[]={(char*)self,"--worker",(char*)state,(char*)root,(char*)marker,(char*)gate,(char*)output,(char*)id,NULL};pid_t child;CHECK(!posix_spawnp(&child,self,NULL,NULL,args,environ));return child;}
static void success(pid_t child){int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));}
static char *read_file(const char *path){FILE *f=fopen(path,"r");CHECK(f);char bytes[65536];size_t n=fread(bytes,1,sizeof(bytes)-1,f);CHECK(n&&!ferror(f));CHECK(!fclose(f));bytes[n]=0;char *s=strdup(bytes);CHECK(s);return s;}
static char swapped_state[1024],swapped_held[1024];
static void swap_admitted_state(void){CHECK(!rename(swapped_state,swapped_held));CHECK(!mkdir(swapped_state,0700));source_bound_test_admitted=NULL;}
int main(int argc,char **argv){
    if(argc==8&&!strcmp(argv[1],"--worker")){SourcesConfig cfg={.state=argv[2],.root=argv[3],.owner="alice"};CHECK(!setenv("SAMOSA_INVENTORY_TEST_MARKER",argv[4],1));CHECK(!setenv("SAMOSA_INVENTORY_TEST_RELEASE",argv[5],1));char req[512];snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"%s\",\"parent_operation_id\":\"root-run\"}",argv[7]);FILE *f=fopen(argv[6],"w");CHECK(f);sources_dispatch(&cfg,req,strlen(req),f);CHECK(!fclose(f));return 0;}
    if(argc==5&&!strcmp(argv[1],"--reserve")){SourcesConfig cfg={.state=argv[2],.owner="alice"};SourcesOperation o;char *saved=NULL;CHECK(!source_operation_begin(&cfg,argv[4],"fixture",&o,&saved));CHECK(!saved);release(argv[3]);for(;;)pause();}
    char self[4096];CHECK(realpath(argv[0],self));char fixture[]="/tmp/samosa-source-operations.XXXXXX";CHECK(mkdtemp(fixture));char state[1024],root[1024],path[2048],request[2048];snprintf(state,sizeof(state),"%s/state",fixture);snprintf(root,sizeof(root),"%s/docs",fixture);CHECK(!mkdir(root,0700));snprintf(path,sizeof(path),"%s/doc.txt",root);release(path);
    snprintf(swapped_state,sizeof(swapped_state),"%s/swapped-state",fixture);snprintf(swapped_held,sizeof(swapped_held),"%s/held-state",fixture);CHECK(!mkdir(swapped_state,0700));
    SourcesConfig swap_cfg={.state=swapped_state,.root=root,.owner="alice"};source_bound_test_admitted=swap_admitted_state;
    char *swap_result=call(&swap_cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"doc.txt\",\"operation_id\":\"swap-register\"}",1);CHECK(strstr(swap_result,"state_unavailable"));free(swap_result);
    CHECK(!rmdir(swapped_state));CHECK(!rename(swapped_held,swapped_state));
    swap_result=call(&swap_cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"swap-register\"}",0);CHECK(strstr(swap_result,"\"status\":\"failed\""));free(swap_result);
    char committed_state[1024];snprintf(committed_state,sizeof(committed_state),"%s/committed-state",fixture);
    SourcesConfig committed_cfg={.state=committed_state,.owner="alice"};SourcesOperation committed={0};char *committed_saved=NULL;
    CHECK(!source_operation_begin(&committed_cfg,"committed-removal","fixture",&committed,&committed_saved));CHECK(!committed_saved);
    char *cancelled_commit=call(&committed_cfg,"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"committed-removal\"}",0);free(cancelled_commit);
    committed.committed_result=1;committed.deadline_monotonic=now()-1;
    char *receipt=strdup("{\"contract_version\":1,\"state\":\"removed\"}\n");CHECK(receipt);int failed_receipt=0;
    CHECK(!source_operation_finish(&committed,&receipt,&failed_receipt));CHECK(!failed_receipt&&strstr(receipt,"removed"));source_operation_close(&committed);
    CHECK(!source_operation_begin(&committed_cfg,"committed-removal","fixture",&committed,&committed_saved));CHECK(committed_saved&&!strcmp(committed_saved,receipt));
    source_operation_close(&committed);free(committed_saved);free(receipt);
    SourcesConfig cfg={.state=state,.root=root,.owner="alice"};char *out=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"doc.txt\"}",0);Json j;CHECK(json_parse(&j,out,strlen(out)));char *source=json_string(&j,json_get(&j,0,"source_id"));CHECK(source);free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":7,\"operation_id\":\"first\",\"parent_operation_id\":\"root-run\",\"timeout_ms\":60000}",source,source);out=call(&cfg,request,0);CHECK(strstr(out,"release"));char *saved=strdup(out);CHECK(saved);free(out);
    SourcesConfig offline=cfg;offline.root=NULL;offline.deadline_monotonic=now()-1;out=call(&offline,request,0);CHECK(!strcmp(saved,out));free(out);
    snprintf(request,sizeof(request),"{\"end_byte\":7,\"start_byte\":0,\"version\":\"%s\",\"source_id\":\"%s\",\"op\":\"read_text\",\"operation_id\":\"first\",\"parent_operation_id\":\"root-run\",\"timeout_ms\":60000,\"contract_version\":1}",source,source);out=call(&cfg,request,0);CHECK(!strcmp(saved,out));free(out);free(saved);
    char *budget=strstr(request,"60000,");CHECK(budget);memcpy(budget,"59999,",6);out=call(&cfg,request,1);CHECK(strstr(out,"idempotency_conflict"));free(out);
    public_request(request,sizeof(request),"operation_cancel","first");out=call(&cfg,request,0);CHECK(strstr(out,"\"status\":\"completed\"")&&strstr(out,"\"cancel_requested\":false")&&strstr(out,"root-run"));free(out);SourcesConfig foreign=cfg;foreign.owner="bob";out=call(&foreign,request,1);CHECK(strstr(out,"unknown_operation"));free(out);
    char markers[3][1024],gates[3][1024],outputs[3][1024];pid_t workers[3];const char *ids[]={"cancelled-read","peer-read","dead-read"};
    for(unsigned i=0;i<3;i++){snprintf(markers[i],sizeof(markers[i]),"%s/ready-%u",fixture,i);snprintf(gates[i],sizeof(gates[i]),"%s/gate-%u",fixture,i);snprintf(outputs[i],sizeof(outputs[i]),"%s/output-%u",fixture,i);workers[i]=worker(self,state,root,markers[i],gates[i],outputs[i],ids[i]);ready(markers[i],workers[i]);}
    public_request(request,sizeof(request),"operation_cancel",ids[0]);out=call(&foreign,request,1);CHECK(strstr(out,"unknown_operation"));free(out);out=call(&cfg,request,0);CHECK(strstr(out,"\"status\":\"running\"")&&strstr(out,"\"cancel_requested\":true"));free(out);
    public_request(request,sizeof(request),"operation_get",ids[1]);out=call(&cfg,request,0);CHECK(strstr(out,"\"status\":\"running\"")&&strstr(out,"\"cancel_requested\":false"));free(out);
    release(gates[0]);success(workers[0]);out=read_file(outputs[0]);CHECK(strstr(out,"\"code\":\"cancelled\"")&&!strstr(out,"entries"));saved=strdup(out);CHECK(saved);free(out);
    public_request(request,sizeof(request),"operation_get",ids[0]);out=call(&cfg,request,0);CHECK(strstr(out,"\"status\":\"cancelled\"")&&strstr(out,"root-run"));free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"%s\",\"parent_operation_id\":\"root-run\"}",ids[0]);out=call(&offline,request,1);CHECK(!strcmp(saved,out));free(out);free(saved);
    release(gates[1]);success(workers[1]);out=read_file(outputs[1]);CHECK(strstr(out,"doc.txt")&&!strstr(out,"error"));free(out);
    CHECK(!kill(workers[2],SIGKILL));int status;CHECK(waitpid(workers[2],&status,0)==workers[2]&&WIFSIGNALED(status));public_request(request,sizeof(request),"operation_get",ids[2]);out=call(&cfg,request,0);CHECK(strstr(out,"\"status\":\"interrupted\""));free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"%s\",\"parent_operation_id\":\"root-run\"}",ids[2]);out=call(&cfg,request,1);CHECK(strstr(out,"operation_interrupted"));free(out);
    char reserve_state[1024];snprintf(reserve_state,sizeof(reserve_state),"%s/reserve",fixture);cfg.state=reserve_state;pid_t reserved[8];
    for(unsigned i=0;i<8;i++){char marker[1024],id[32];snprintf(marker,sizeof(marker),"%s/reserve-%u",fixture,i);snprintf(id,sizeof(id),"reserved-%u",i);char *args[]={self,"--reserve",reserve_state,marker,id,NULL};CHECK(!posix_spawnp(&reserved[i],self,NULL,NULL,args,environ));ready(marker,reserved[i]);}
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"capacity\"}",1);CHECK(strstr(out,"operation_capacity"));free(out);CHECK(!kill(reserved[0],SIGKILL));CHECK(waitpid(reserved[0],&status,0)==reserved[0]);out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"capacity\"}",0);free(out);for(unsigned i=1;i<8;i++){CHECK(!kill(reserved[i],SIGKILL));CHECK(waitpid(reserved[i],&status,0)==reserved[i]);}
    char quota[1024];snprintf(quota,sizeof(quota),"%s/quota",fixture);cfg.state=quota;
    for(unsigned i=0;i<128;i++){snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"quota-%u\"}",i);out=call(&cfg,request,0);free(out);}
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"quota-overflow\"}",1);CHECK(strstr(out,"operation_limit"));free(out);out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"quota-0\"}",0);CHECK(strstr(out,"doc.txt"));free(out);
    char dbpath[2048];snprintf(dbpath,sizeof(dbpath),"%s/read-operations.sqlite3",quota);sqlite3 *db=NULL;CHECK(sqlite3_open(dbpath,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE operations SET digest='wrong' WHERE id='quota-0'",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"quota-0\"}",1);CHECK(strstr(out,"corrupt_operation"));free(out);
    char badstate[1024],target[2048];snprintf(badstate,sizeof(badstate),"%s/foreign",fixture);CHECK(!mkdir(badstate,0700));snprintf(target,sizeof(target),"%s/read-operations.sqlite3",badstate);CHECK(!symlink(dbpath,target));cfg.state=badstate;out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"refused\"}",1);CHECK(strstr(out,"state_unavailable"));free(out);
    free(source);printf("PASS durable Source operations: %u checks; normalized/offline retry, scoped concurrent cancellation, peer/death recovery,8 active/128 histories, corruption and symlink refusal; QA %s\n",checks,fixture);return 0;
}
