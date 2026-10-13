#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "sources.h"
#include "json.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
extern char **environ;
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *run(const SourcesConfig *cfg,const char *path,int expected) {
    char *input=NULL,*out=NULL;size_t length,n;FILE *stream=open_memstream(&input,&length);CHECK(stream);fputs("{\"contract_version\":1,\"op\":\"inventory\",\"path\":",stream);json_quote(stream,path,strlen(path));fputc('}',stream);CHECK(!fclose(stream));
    stream=open_memstream(&out,&n);CHECK(stream);CHECK(sources_dispatch(cfg,input,length,stream)==expected);CHECK(!fclose(stream));free(input);Json result;CHECK(json_parse(&result,out,n));return out;
}
static void file(const char *path){FILE *out=fopen(path,"w");CHECK(out);fputs("7319",out);CHECK(!fclose(out));}
static size_t count(const char *out){Json j;CHECK(json_parse(&j,out,strlen(out)));int entries=json_get(&j,0,"entries");CHECK(entries>=0&&j.tokens[entries].type==JT_ARRAY);return (size_t)j.tokens[entries].count;}
int main(int argc,char **argv) {
    if(argc==3&&!strcmp(argv[1],"--change-worker")){SourcesConfig cfg={.root=argv[2]};const char *request="{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\"}";return sources_dispatch(&cfg,request,strlen(request),stdout);}
    char fixture[]="/tmp/samosa-native-inventory-XXXXXX";CHECK(mkdtemp(fixture));char root[1024],path[4096],selected[1024];snprintf(root,sizeof(root),"%s/root",fixture);CHECK(!mkdir(root,0700));
    SourcesConfig cfg={.root=root,.host_id="fixture-host"};char *out=run(&cfg,"",0);CHECK(count(out)==0);CHECK(strstr(out,"\"status\":\"complete\""));CHECK(strstr(out,"\"content_coverage\":\"uninspected\""));CHECK(strstr(out,"fixture-host"));free(out);
    snprintf(path,sizeof(path),"%s/a",root);CHECK(!mkdir(path,0700));snprintf(path,sizeof(path),"%s/b",root);CHECK(!mkdir(path,0700));snprintf(path,sizeof(path),"%s/a/duplicate.txt",root);file(path);snprintf(path,sizeof(path),"%s/b/duplicate.txt",root);file(path);
    snprintf(path,sizeof(path),"%s/link",root);CHECK(!symlink("a",path));snprintf(path,sizeof(path),"%s/pipe",root);CHECK(!mkfifo(path,0600));
    out=run(&cfg,"",0);CHECK(count(out)==6);CHECK(strstr(out,"a/duplicate.txt")&&strstr(out,"b/duplicate.txt"));CHECK(strstr(out,"\"excluded_nonregular\":2"));CHECK(strstr(out,"\"content_version\":null"));CHECK(!strstr(out,"\"text\":"));free(out);
    out=run(&cfg,"a",0);CHECK(count(out)==1);CHECK(strstr(out,"a/duplicate.txt"));free(out);
    const char *bad[]={"../root","/etc","a/../b","a//b","a/","link","a/duplicate.txt"};for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++){out=run(&cfg,bad[i],1);CHECK(strstr(out,"unauthorized_path"));free(out);}
    snprintf(path,sizeof(path),"%s/deep",root);CHECK(!mkdir(path,0700));snprintf(selected,sizeof(selected),"deep");
    for(size_t i=0;i<6;i++){size_t used=strlen(selected);snprintf(selected+used,sizeof(selected)-used,"/d%zu",i);snprintf(path,sizeof(path),"%s/%s",root,selected);CHECK(!mkdir(path,0700));}
    snprintf(path,sizeof(path),"%s/%s/last.txt",root,selected);file(path);out=run(&cfg,"deep",0);CHECK(strstr(out,"\"status\":\"partial\""));CHECK(strstr(out,"\"total_entries\":null"));CHECK(!strstr(out,"last.txt"));free(out);
    snprintf(path,sizeof(path),"%s/many",root);CHECK(!mkdir(path,0700));for(size_t i=0;i<300;i++){snprintf(path,sizeof(path),"%s/many/file%03zu",root,i);file(path);}out=run(&cfg,"many",0);CHECK(count(out)==256);CHECK(strstr(out,"\"status\":\"partial\""));CHECK(strstr(out,"\"total_entries\":null"));free(out);
#ifndef __APPLE__
    snprintf(path,sizeof(path),"%s/invalid",root);CHECK(!mkdir(path,0700));snprintf(path,sizeof(path),"%s/invalid/\xff",root);file(path);out=run(&cfg,"invalid",0);CHECK(count(out)==0);CHECK(strstr(out,"\"invalid_names\":1"));CHECK(strstr(out,"\"status\":\"partial\""));free(out);
#endif
    char changing[1024],marker[1024],release[1024],result_path[1024];snprintf(changing,sizeof(changing),"%s/changing",fixture);CHECK(!mkdir(changing,0700));snprintf(marker,sizeof(marker),"%s/marker",fixture);snprintf(release,sizeof(release),"%s/release",fixture);snprintf(result_path,sizeof(result_path),"%s/result",fixture);
    CHECK(!setenv("SAMOSA_INVENTORY_TEST_MARKER",marker,1));CHECK(!setenv("SAMOSA_INVENTORY_TEST_RELEASE",release,1));char *worker_args[]={argv[0],"--change-worker",changing,NULL};posix_spawn_file_actions_t actions;CHECK(!posix_spawn_file_actions_init(&actions));CHECK(!posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,result_path,O_CREAT|O_WRONLY|O_TRUNC,0600));pid_t worker;CHECK(!posix_spawnp(&worker,argv[0],&actions,NULL,worker_args,environ));posix_spawn_file_actions_destroy(&actions);CHECK(!unsetenv("SAMOSA_INVENTORY_TEST_MARKER"));CHECK(!unsetenv("SAMOSA_INVENTORY_TEST_RELEASE"));
    int ready=0;for(int i=0;i<500;i++){if(access(marker,F_OK)==0){ready=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}CHECK(ready);snprintf(path,sizeof(path),"%s/new-file",changing);file(path);file(release);int worker_status;CHECK(waitpid(worker,&worker_status,0)==worker);CHECK(WIFEXITED(worker_status)&&WEXITSTATUS(worker_status)==1);FILE *result=fopen(result_path,"r");CHECK(result);char error[1024];size_t bytes=fread(error,1,sizeof(error)-1,result);error[bytes]=0;CHECK(!fclose(result));CHECK(strstr(error,"source_changed"));CHECK(!strstr(error,"entries"));
    printf("PASS native inventory: %u checks; bounded read-only/no state/model, duplicate locators, symlink/FIFO, explicit incomplete/uninspected\n",checks);
    char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child;CHECK(!posix_spawnp(&child,"find",NULL,NULL,cleanup,environ));int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);return 0;
}
