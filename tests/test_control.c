#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL Source control %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int cancelled(void *context){return *(int*)context;}
static char *call(const SourcesConfig *cfg,const char *request,int expected){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);int rc=sources_dispatch(cfg,request,strlen(request),f);CHECK(!fclose(f));if(rc!=expected)fprintf(stderr,"Source result: %s",out);CHECK(rc==expected);Json j;CHECK(json_parse(&j,out,n));return out;}
static char *field(const char *body,const char *name){Json j;CHECK(json_parse(&j,body,strlen(body)));char *out=json_string(&j,json_get(&j,0,name));CHECK(out);return out;}
int main(void){char root[]="/tmp/samosa-source-control.XXXXXX";CHECK(mkdtemp(root));char docs[512],state[512],path[512],marker[512],release[512];
    snprintf(docs,sizeof(docs),"%s/docs",root);snprintf(state,sizeof(state),"%s/state",root);snprintf(path,sizeof(path),"%s/docs/doc.txt",root);snprintf(marker,sizeof(marker),"%s/ready",root);snprintf(release,sizeof(release),"%s/release",root);CHECK(!mkdir(docs,0700));FILE *f=fopen(path,"w");CHECK(f);CHECK(fwrite("hello",1,5,f)==5);CHECK(!fclose(f));
    SourcesConfig cfg={.root=docs,.state=state,.owner="alice"};char *out=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"doc.txt\"}",0),*id=field(out,"source_id");free(out);char request[1024];
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":5}",id,id);out=call(&cfg,request,0);char *evidence=field(out,"evidence_id");free(out);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":5,\"operation_id\":\"read-1\",\"parent_operation_id\":\"run-1\",\"timeout_ms\":60000}",id,id);
    out=call(&cfg,request,0);CHECK(strstr(out,"\"operation_id\":\"read-1\"")&&strstr(out,"\"parent_operation_id\":\"run-1\""));char *same=field(out,"evidence_id");CHECK(!strcmp(evidence,same));free(same);free(out);
    SourcesConfig foreign=cfg;foreign.owner="bob";out=call(&foreign,request,1);CHECK(strstr(out,"unknown_source")&&strstr(out,"run-1"));free(out);
    SourcesConfig expired=cfg;expired.deadline_monotonic=now()-1;out=call(&expired,request,0);CHECK(strstr(out,"hello"));free(out);char fresh[1024];snprintf(fresh,sizeof(fresh),"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"expired\",\"parent_operation_id\":\"run-1\"}");out=call(&expired,fresh,1);CHECK(strstr(out,"deadline_exceeded")&&strstr(out,"run-1"));free(out);
    int stop=1;SourcesConfig stopped=cfg;stopped.cancelled=cancelled;stopped.cancellation=&stop;out=call(&stopped,request,0);CHECK(strstr(out,"hello"));free(out);snprintf(fresh,sizeof(fresh),"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"stopped\",\"parent_operation_id\":\"run-1\"}");out=call(&stopped,fresh,1);CHECK(strstr(out,"cancelled")&&strstr(out,"run-1"));free(out);stop=0;out=call(&stopped,fresh,1);CHECK(strstr(out,"cancelled"));free(out);
    const char *bad[]={"\"timeout_ms\":0","\"timeout_ms\":60001","\"timeout_ms\":\"20\"","\"timeout_ms\":1.5","\"operation_id\":\"\"","\"operation_id\":null","\"operation_id\":\"bad\\nref\"","\"operation_id\":\"r\",\"parent_operation_id\":\"r\"","\"parent_operation_id\":\"p\""};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++){snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",%s}",bad[i]);out=call(&cfg,request,1);CHECK(strstr(out,"invalid_request"));free(out);}
    out=call(&cfg,"{\"contract_version\":2,\"op\":\"inventory\",\"path\":\"\",\"timeout_ms\":0}",1);CHECK(strstr(out,"unsupported_contract"));free(out);
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"../foreign\",\"operation_id\":\"denied\",\"parent_operation_id\":\"run\"}",1);CHECK(strstr(out,"unauthorized_path")&&strstr(out,"\"operation_id\":\"denied\""));free(out);
    CHECK(!setenv("SAMOSA_INVENTORY_TEST_MARKER",marker,1));CHECK(!setenv("SAMOSA_INVENTORY_TEST_RELEASE",release,1));pid_t p=fork();CHECK(p>=0);if(!p){struct timespec t={0,100000000};nanosleep(&t,NULL);FILE *ready=fopen(release,"w");if(!ready)_exit(2);fputs("release",ready);_exit(fclose(ready)?2:0);}
    double started=now();out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"operation_id\":\"slow\",\"parent_operation_id\":\"run\",\"timeout_ms\":20}",1);CHECK(now()-started<2);CHECK(strstr(out,"deadline_exceeded")&&strstr(out,"\"operation_id\":\"slow\"")&&!strstr(out,"entries"));free(out);int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&!WEXITSTATUS(status));CHECK(!unsetenv("SAMOSA_INVENTORY_TEST_MARKER"));CHECK(!unsetenv("SAMOSA_INVENTORY_TEST_RELEASE"));
    out=call(&cfg,"{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\",\"timeout_ms\":60000}",0);CHECK(strstr(out,"doc.txt"));free(out);out=call(&cfg,"{\"contract_version\":1,\"op\":\"describe\"}",0);CHECK(strstr(out,"request_correlation")&&strstr(out,"request_budgets"));free(out);
    free(evidence);free(id);printf("PASS Source request control: %u checks; correlation, owner refusal, budget validation, expired/cancelled ingress, delayed inventory and survivor\n",checks);return 0;
}
