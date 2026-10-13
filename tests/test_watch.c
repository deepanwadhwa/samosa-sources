#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "watch.h"
#include "json.h"
#include <assert.h>
#include <unistd.h>
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/file.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
extern char **environ;
static int cancel_step(void *context){unsigned *calls=context;return ++*calls>=3;}
static char *call(SourcesConfig *cfg,const char *request,int expected){
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);
    int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));
    if(rc!=expected)fprintf(stderr,"watch: %s\n",body);
    assert(rc==expected);return body;
}
int main(int argc,char **argv){
    if(argc==5&&!strcmp(argv[1],"--admit")){SourcesConfig cfg={.state=argv[2],.root=argv[3]};int directory=-1,runtime=-1;const char *error=sources_watch_admit(&cfg,argv[4],&directory,&runtime);if(runtime>=0)close(runtime);if(directory>=0)close(directory);return error?1:0;}
    if(argc==4&&!strcmp(argv[1],"--remove")){SourcesConfig cfg={.state=argv[2]};return sources_dispatch(&cfg,argv[3],strlen(argv[3]),stdout);}

    char root[]="/tmp/samosa-watch-XXXXXX";assert(mkdtemp(root));
    char state[512],path[512],request[1024];snprintf(state,sizeof(state),"%s/state",root);
    snprintf(path,sizeof(path),"%s/input.txt",root);FILE *file=fopen(path,"w");assert(file);
    assert(fputs("watch text",file)>=0&&!fclose(file));SourcesConfig cfg={.root=root,.state=state};
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"input.txt\"}",0);
    Json j;assert(json_parse(&j,body,strlen(body)));char *version=json_string(&j,json_get(&j,0,"version"));assert(version);free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch-create\",\"locators\":[{\"path\":\"input.txt\",\"expected_version\":\"%s\"}]}",version);
    char *saved=call(&cfg,request,0);assert(strstr(saved,"registered")&&strstr(saved,"not_running"));
    assert(json_parse(&j,saved,strlen(saved)));char *id=json_string(&j,json_get(&j,0,"watch_id"));assert(id);
    char remove[512];snprintf(remove,sizeof(remove),"{\"contract_version\":1,\"op\":\"locator_watch_remove\",\"watch_id\":\"%s\",\"operation_id\":\"remove-active\"}",id);
    body=call(&cfg,remove,1);assert(strstr(body,"watch_not_stopped"));free(body);
    char step[512];snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":0}",id);
    body=call(&cfg,step,0);assert(strstr(body,"\"status\":\"available\""));free(body);
    assert(!unlink(path));SourcesConfig offline=cfg;offline.root=NULL;
    body=call(&offline,request,0);assert(strstr(body,id)&&strstr(body,"registered"));free(body);free(saved);
    body=call(&offline,step,0);assert(strstr(body,"\"cycle\":1"));free(body);
    for(unsigned cycle=1;cycle<=260;cycle++){
        snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":%u}",id,cycle);
        body=call(&cfg,step,0);assert(strstr(body,"\"status\":\"missing\""));free(body);
    }
    body=call(&offline,step,0);assert(strstr(body,"\"cycle\":261"));free(body);
    const char *restored[]={"watch text","different text","watch text"};
    const char *statuses[]={"available","changed","available"};
    for(unsigned i=0;i<3;i++){
        file=fopen(path,"w");assert(file);assert(fputs(restored[i],file)>=0&&!fclose(file));
        snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":%u}",id,261+i);
        body=call(&cfg,step,0);char expected[64];snprintf(expected,sizeof(expected),"\"status\":\"%s\"",statuses[i]);assert(strstr(body,expected));free(body);
    }
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":264}",id);
    unsigned calls=0;SourcesConfig cancelled=cfg;cancelled.cancelled=cancel_step;cancelled.cancellation=&calls;
    body=call(&cancelled,step,1);assert(strstr(body,"cancelled"));free(body);
    snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":0}",id);
    body=call(&cfg,step,1);assert(strstr(body,"watch_cycle_conflict"));free(body);
    char *entry=strstr(request,"input.txt");assert(entry);entry[0]='o';
    body=call(&offline,request,1);assert(strstr(body,"idempotency_conflict"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_get\",\"watch_id\":\"%s\"}",id);
    body=call(&offline,request,0);assert(strstr(body,"registered")&&strstr(body,"\"cycle\":264"));free(body);
    SourcesConfig foreign=offline;foreign.owner="foreign";body=call(&foreign,request,1);assert(strstr(body,"unknown_watch"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_stop\",\"watch_id\":\"%s\"}",id);
    body=call(&offline,request,0);assert(strstr(body,"stopped"));free(body);
    body=call(&cfg,step,1);assert(strstr(body,"watch_stopped"));free(body);
    body=call(&offline,request,0);assert(strstr(body,"stopped"));free(body);
    for(unsigned i=1;i<=8;i++){
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch-%u\",\"locators\":[{\"path\":\"input.txt\",\"expected_version\":\"%s\"}]}",i,version);
        body=call(&cfg,request,i==8?1:0);assert(strstr(body,i==8?"watch_limit":"registered"));free(body);
    }
    char database[1024];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);
    assert(sqlite3_exec(db,"UPDATE watch_checkpoints SET digest='bad'",NULL,NULL,NULL)==SQLITE_OK);
    assert(sqlite3_close(db)==SQLITE_OK);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_get\",\"watch_id\":\"%s\"}",id);
    body=call(&offline,request,1);assert(json_parse(&j,body,strlen(body))&&strstr(body,"corrupt_locator")&&!strstr(body,"checkpoint"));free(body);
    char lease_path[1024];snprintf(lease_path,sizeof(lease_path),"%s/watch-%s.lock",state,id);
    int held=open(lease_path,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);assert(held>=0&&!flock(held,LOCK_EX|LOCK_NB));
    snprintf(remove,sizeof(remove),"{\"contract_version\":1,\"op\":\"locator_watch_remove\",\"watch_id\":\"%s\",\"operation_id\":\"remove-busy\"}",id);
    body=call(&offline,remove,1);assert(strstr(body,"watch_busy"));free(body);assert(!close(held));
    snprintf(remove,sizeof(remove),"{\"contract_version\":1,\"op\":\"locator_watch_remove\",\"watch_id\":\"%s\",\"operation_id\":\"remove-stopped\"}",id);
    body=call(&foreign,remove,1);assert(strstr(body,"unknown_watch"));free(body);
    saved=call(&offline,remove,0);assert(strstr(saved,"removed"));
    assert(access(lease_path,F_OK)<0&&errno==ENOENT);
    body=call(&offline,remove,0);assert(!strcmp(saved,body));free(body);free(saved);
    body=call(&offline,request,1);assert(strstr(body,"unknown_watch"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_create\",\"operation_id\":\"watch-create\",\"locators\":[{\"path\":\"input.txt\",\"expected_version\":\"%s\"}]}",version);
    body=call(&cfg,request,0);assert(strstr(body,"registered"));assert(json_parse(&j,body,strlen(body)));char *replacement=json_string(&j,json_get(&j,0,"watch_id"));assert(replacement&&strcmp(replacement,id));free(body);
    char admit_marker[512];snprintf(admit_marker,sizeof(admit_marker),"%s/admit-marker",root);
    for(unsigned stage=0;stage<2;stage++){
        assert(!setenv("SAMOSA_WATCH_ADMIT_MARKER",admit_marker,1)&&!setenv("SAMOSA_WATCH_ADMIT_STAGE",stage?"after":"before",1));
        char *admit_args[]={argv[0],"--admit",state,root,replacement,NULL};pid_t admitted;assert(!posix_spawnp(&admitted,argv[0],NULL,NULL,admit_args,environ));assert(!unsetenv("SAMOSA_WATCH_ADMIT_MARKER")&&!unsetenv("SAMOSA_WATCH_ADMIT_STAGE"));
        int admitted_ready=0;for(unsigned i=0;i<250;i++){if(!access(admit_marker,F_OK)){admitted_ready=1;break;}struct timespec delay={0,20000000};nanosleep(&delay,NULL);}assert(admitted_ready);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_stop\",\"watch_id\":\"%s\"}",replacement);body=call(&offline,request,1);assert(strstr(body,"busy"));free(body);
        assert(!kill(admitted,SIGKILL));int admitted_status;assert(waitpid(admitted,&admitted_status,0)==admitted&&WIFSIGNALED(admitted_status));assert(!unlink(admit_marker));
    }
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_stop\",\"watch_id\":\"%s\"}",replacement);body=call(&offline,request,0);free(body);
    snprintf(lease_path,sizeof(lease_path),"%s/watch-%s.lock",state,replacement);held=open(lease_path,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);assert(held>=0&&!close(held));
    char marker[512];snprintf(marker,sizeof(marker),"%s/committed-marker",root);assert(!setenv("SAMOSA_WATCH_REMOVE_COMMITTED",marker,1));
    snprintf(remove,sizeof(remove),"{\"contract_version\":1,\"op\":\"locator_watch_remove\",\"watch_id\":\"%s\"}",replacement);
    char *args[]={argv[0],"--remove",state,remove,NULL};pid_t child;assert(!posix_spawnp(&child,argv[0],NULL,NULL,args,environ));assert(!unsetenv("SAMOSA_WATCH_REMOVE_COMMITTED"));
    int ready=0;for(unsigned i=0;i<250;i++){if(!access(marker,F_OK)){ready=1;break;}struct timespec delay={0,20000000};nanosleep(&delay,NULL);}assert(ready);assert(!kill(child,SIGKILL));int status;assert(waitpid(child,&status,0)==child&&WIFSIGNALED(status));assert(!access(lease_path,F_OK));
    body=call(&foreign,"{\"contract_version\":1,\"op\":\"locator_watch_cleanup\"}",0);assert(strstr(body,"\"cleaned\":0"));free(body);
    held=open(lease_path,O_RDWR|O_NOFOLLOW|O_CLOEXEC);assert(held>=0&&!flock(held,LOCK_EX|LOCK_NB));
    body=call(&offline,"{\"contract_version\":1,\"op\":\"locator_watch_cleanup\"}",0);assert(strstr(body,"\"retained\":1"));free(body);assert(!close(held));
    body=call(&offline,"{\"contract_version\":1,\"op\":\"locator_watch_cleanup\"}",0);assert(strstr(body,"\"cleaned\":1"));free(body);assert(access(lease_path,F_OK)<0&&errno==ENOENT);
    body=call(&offline,"{\"contract_version\":1,\"op\":\"locator_watch_cleanup\"}",0);assert(strstr(body,"\"cleaned\":0"));free(body);int rejected_directory=-1,rejected_runtime=-1;const char *rejected=sources_watch_admit(&cfg,replacement,&rejected_directory,&rejected_runtime);assert(rejected&&!strcmp(rejected,"unknown_watch")&&rejected_directory<0&&rejected_runtime<0&&access(lease_path,F_OK)<0);free(replacement);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":10}",version,version);
    body=call(&offline,request,0);assert(strstr(body,"watch text"));free(body);
    free(id);free(version);puts("PASS watch registry: bound owner/root registration, offline retry/conflict/status, stop and eight-watch refusal; monitoring remains not_running");return 0;
}
