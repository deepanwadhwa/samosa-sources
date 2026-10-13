#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "json.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {int (*create)(const char *,const char *,void **);int (*open)(const char *,int,void **);int (*call)(void *,const char *,const char *,char **);void (*free)(void *);void (*close)(void *);} Sdk;
typedef struct {const char *cli,*worker,*library,*store,*state;} Host;
static void symbol(void *library,const char *name,void *function,size_t size){void *p=dlsym(library,name);CHECK(p&&size==sizeof(p));memcpy(function,&p,size);}
static void parse(const char *body,Json *j){CHECK(body&&json_parse(j,body,strlen(body))&&j->tokens[0].type==JT_OBJECT);}
static char *string(const Json *j,int object,const char *key){char *s=json_string(j,json_get(j,object,key));CHECK(s);return s;}
static int equal(const Json *j,int object,const char *key,const char *expected){char *s=json_string(j,json_get(j,object,key));int yes=s&&!strcmp(s,expected);free(s);return yes;}
static char *slice(const Json *j,int t){CHECK(t>=0);char *s=strndup(j->text+j->tokens[t].begin,j->tokens[t].end-j->tokens[t].begin);CHECK(s);return s;}
static char *sdk_call(Sdk *sdk,void *store,const char *op,const char *arguments){char *raw=NULL;int rc=sdk->call(store,op,arguments,&raw);if(rc)fprintf(stderr,"SDK %s status %d: %s\n",op,rc,raw?raw:"");CHECK(!rc&&raw);Json j;parse(raw,&j);char *body=strdup(raw);CHECK(body);sdk->free(raw);return body;}
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static char *run(const Host *h,const char *request,int configured,const char *owner,const char *library,const char *error){
    FILE *input=tmpfile(),*output=tmpfile();CHECK(input&&output);CHECK(fwrite(request,1,strlen(request),input)==strlen(request));CHECK(!fflush(input));rewind(input);
    char *args[20]={(char*)h->cli,"--state",(char*)h->state,"--owner",(char*)(owner?owner:"memory-owner"),NULL};size_t n=5;
    if(configured){args[n++]="--chutni-library";args[n++]=(char*)(library?library:h->library);args[n++]="--chutni-store";args[n++]=(char*)h->store;args[n++]="--chutni-helper";args[n++]=(char*)h->worker;}args[n]=NULL;
    pid_t pid=fork();CHECK(pid>=0);if(!pid){if(dup2(fileno(input),0)<0||dup2(fileno(output),1)<0)_exit(2);for(int fd=3;fd<128;fd++)close(fd);execv(args[0],args);_exit(2);}
    int status=0,done=0;double deadline=now()+20;while(!done&&now()<deadline){pid_t got=waitpid(pid,&status,WNOHANG);if(got==pid)done=1;else CHECK(got==0||(got<0&&errno==EINTR));struct timespec delay={0,10000000};nanosleep(&delay,NULL);}
    if(!done){kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}CHECK(done);CHECK(!fseek(output,0,SEEK_END));long size=ftell(output);CHECK(size>0&&size<=2097152);rewind(output);char *body=malloc((size_t)size+1);CHECK(body);CHECK(fread(body,1,(size_t)size,output)==(size_t)size);body[size]=0;CHECK(!fclose(input)&&!fclose(output));
    if(!WIFEXITED(status)||WEXITSTATUS(status)!=(error?1:0))fprintf(stderr,"response: %s\n",body);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==(error?1:0));Json j;parse(body,&j);
    if(error){int e=json_get(&j,0,"error");if(!equal(&j,e,"code",error))fprintf(stderr,"response: %s\n",body);CHECK(equal(&j,e,"code",error));}return body;
}
static char *read_request(const char *store,const char *source,const char *artifact,const char *id,const char *range){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);fprintf(out,"{\"contract_version\":1,\"op\":\"memory_read\",\"store_id\":\"%s\",\"external_source_id\":\"%s\",\"external_artifact_id\":\"%s\",\"operation_id\":\"%s\",\"timeout_ms\":10000%s}",store,source,artifact,id,range?range:"");CHECK(!fclose(out));return body;}
static char *search_request(const char *id,const char *query,size_t timeout){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);fprintf(out,"{\"contract_version\":1,\"op\":\"memory_search\",\"operation_id\":\"%s\",\"timeout_ms\":%zu,\"query\":",id,timeout);json_quote(out,query,strlen(query));fputc('}',out);CHECK(!fclose(out));return body;}
static void claim(const Host *h,const char *captured){Json j;parse(captured,&j);char *source=string(&j,0,"source_id"),*version=string(&j,0,"version");char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":256}",source,version);free(source);free(version);free(run(h,request,0,NULL,NULL,NULL));free(run(h,request,0,"different-owner",NULL,"unknown_source"));}
static void range_check(const char *body,size_t from,size_t to,size_t total,const char *coverage){Json j;parse(body,&j);int result=json_get(&j,json_get(&j,0,"memory"),"result"),range=json_get(&j,result,"range");size_t a=0,b=0,c=0;CHECK(json_uint(&j,json_get(&j,range,"start_byte"),&a)&&a==from);CHECK(json_uint(&j,json_get(&j,range,"end_byte"),&b)&&b==to);CHECK(json_uint(&j,json_get(&j,range,"total_bytes"),&c)&&c==total);CHECK(equal(&j,json_get(&j,result,"coverage"),"status",coverage));char *text=string(&j,result,"text");CHECK(strlen(text)==to-from);for(size_t i=0;i<to-from;i++)CHECK((unsigned char)text[i]==(i%2?0xa1:0xc3));free(text);}
static int remove_file(const char *p,const struct stat *s,int kind,struct FTW *walk){(void)s;(void)kind;(void)walk;return remove(p);}
int main(int argc,char **argv){
    CHECK(argc==4);for(int i=1;i<argc;i++)CHECK(argv[i][0]=='/');void *library=dlopen(argv[3],RTLD_NOW|RTLD_LOCAL);CHECK(library);Sdk sdk={0};symbol(library,"chutni_create",&sdk.create,sizeof(sdk.create));symbol(library,"chutni_open",&sdk.open,sizeof(sdk.open));symbol(library,"chutni_call",&sdk.call,sizeof(sdk.call));symbol(library,"chutni_free",&sdk.free,sizeof(sdk.free));symbol(library,"chutni_close",&sdk.close,sizeof(sdk.close));
    char root[]="/tmp/samosa-chutni-native-XXXXXX";CHECK(mkdtemp(root));char path[512],state[512],missing[512];snprintf(path,sizeof(path),"%s/memory.chutni",root);snprintf(state,sizeof(state),"%s/sources",root);snprintf(missing,sizeof(missing),"%s/missing-sdk",root);Host host={argv[1],argv[2],argv[3],path,state};void *store=NULL;CHECK(!sdk.create(path,"Sources integration fixture",&store));
    for(size_t i=0;i<12;i++){char request[512];snprintf(request,sizeof(request),"{\"memory_kind\":\"note\",\"title\":\"Note %zu\",\"text\":\"Coriander evidence from note %zu.\",\"producer\":{\"producer_kind\":\"human\",\"name\":\"integration-fixture\"},\"operation\":\"test-memory\"}",i,i);free(sdk_call(&sdk,store,"put_memory",request));}
    char *large_text=malloc(360001);CHECK(large_text);for(size_t i=0;i<360000;i++)large_text[i]=(char)(i%2?0xa1:0xc3);large_text[360000]=0;
    char *arguments=NULL;size_t n;FILE *out=open_memstream(&arguments,&n);CHECK(out);fputs("{\"memory_kind\":\"note\",\"title\":\"Large text\",\"text\":",out);json_quote(out,large_text,360000);fputs(",\"producer\":{\"producer_kind\":\"human\",\"name\":\"integration-fixture\"},\"operation\":\"test-large-memory\"}",out);CHECK(!fclose(out));char *large=sdk_call(&sdk,store,"put_memory",arguments);free(arguments);free(large_text);Json j;parse(large,&j);char *large_source=string(&j,0,"source_id"),*large_artifact=string(&j,0,"artifact_id");free(large);
    char *before=sdk_call(&sdk,store,"store_info","{}");parse(before,&j);char *store_id=string(&j,0,"store_id"),*counts=slice(&j,json_get(&j,0,"counts"));free(before);sdk.close(store);store=NULL;
    char *query=search_request("memory-attempt","Coriander",10000),*accepted=run(&host,query,1,NULL,NULL,NULL);CHECK(!strstr(accepted,path));parse(accepted,&j);int memory=json_get(&j,0,"memory"),coverage=json_get(&j,memory,"coverage"),result=json_get(&j,memory,"result"),items=json_get(&j,result,"items");CHECK(equal(&j,coverage,"status","partial")&&equal(&j,coverage,"reason","retrieval_metadata"));CHECK(equal(&j,result,"store_id",store_id));CHECK(items>=0&&j.tokens[items].type==JT_ARRAY&&j.tokens[items].count==8);
    for(int t=items+1;t<j.tokens[items].next;t=j.tokens[t].next){CHECK(equal(&j,t,"freshness","current")&&equal(&j,t,"score_type","bm25_fts5_negated"));char *snippet=string(&j,t,"snippet");CHECK(strstr(snippet,"Coriander"));free(snippet);}
    char *source=string(&j,items+1,"external_source_id"),*artifact=string(&j,items+1,"external_artifact_id");char *request=read_request(store_id,source,artifact,"read-artifact",NULL),*captured=run(&host,request,1,NULL,NULL,NULL);CHECK(!strstr(captured,path));parse(captured,&j);result=json_get(&j,json_get(&j,0,"memory"),"result");CHECK(equal(&j,result,"external_artifact_id",artifact)&&equal(&j,json_get(&j,result,"coverage"),"status","complete"));char *text=string(&j,result,"text");CHECK(strstr(text,"Coriander"));free(text);CHECK(equal(&j,json_get(&j,json_get(&j,result,"provenance"),"producer"),"name","integration-fixture"));CHECK(equal(&j,result,"semantic_validation","not_performed"));char *retry=run(&host,request,0,NULL,NULL,NULL);CHECK(!strcmp(retry,captured));free(retry);claim(&host,captured);free(captured);free(request);
    const char *wrong="01234567-89ab-cdef-0123-456789abcdef";request=read_request(wrong,source,artifact,"read-wrong-store",NULL);free(run(&host,request,1,NULL,NULL,"chutni_store_changed"));free(request);request=read_request(store_id,source,wrong,"read-wrong-artifact",NULL);free(run(&host,request,1,NULL,NULL,"chutni_not_found"));free(request);
    request=read_request(store_id,large_source,large_artifact,"read-large-later",",\"start_byte\":262144,\"end_byte\":327679");char *later=run(&host,request,1,NULL,NULL,NULL);range_check(later,262144,327678,360000,"partial");free(later);free(request);
    request=read_request(store_id,large_source,large_artifact,"read-split-character",",\"start_byte\":262145,\"end_byte\":327679");/* Preserve the prior SDK refusal of a split UTF-8 start. */
    free(run(&host,request,1,NULL,NULL,"chutni_failed"));free(request);
    request=read_request(store_id,large_source,large_artifact,"read-tail",",\"start_byte\":327678,\"end_byte\":360000");char *tail=run(&host,request,1,NULL,NULL,NULL);range_check(tail,327678,360000,360000,"partial");free(tail);free(request);
    request=read_request(store_id,large_source,large_artifact,"read-excessive",",\"start_byte\":262144,\"end_byte\":400000");free(run(&host,request,1,NULL,NULL,"invalid_request"));free(request);
    retry=run(&host,query,0,NULL,NULL,NULL);CHECK(!strcmp(retry,accepted));free(retry);claim(&host,accepted);free(accepted);free(query);
    query=search_request("absent","Coriander",10000);free(run(&host,query,0,NULL,NULL,"chutni_unavailable"));free(query);query=search_request("bad-sdk","Coriander",10000);char *bad=run(&host,query,1,NULL,missing,"chutni_sdk_unavailable");CHECK(!strstr(bad,root));free(bad);free(query);
    free(run(&host,"{\"contract_version\":1,\"op\":\"memory_search\",\"query\":\"Coriander\",\"store_path\":\"/other\"}",1,NULL,NULL,"unknown_field"));
    query=search_request("empty","nonexistentuniquephrase",10000);char *empty=run(&host,query,1,NULL,NULL,NULL);parse(empty,&j);items=json_get(&j,json_get(&j,json_get(&j,0,"memory"),"result"),"items");CHECK(items>=0&&!j.tokens[items].count);free(empty);free(query);
    char too_long[514];memset(too_long,'x',513);too_long[513]=0;query=search_request("invalid",too_long,10000);free(run(&host,query,1,NULL,NULL,"invalid_request"));free(query);query=search_request("deadline","Coriander",1);free(run(&host,query,1,NULL,NULL,"deadline_exceeded"));free(query);
    CHECK(!sdk.open(path,1,&store));char *after=sdk_call(&sdk,store,"store_info","{}");parse(after,&j);char *after_counts=slice(&j,json_get(&j,0,"counts"));CHECK(!strcmp(counts,after_counts));free(after_counts);free(after);sdk.close(store);CHECK(!dlclose(library));free(counts);free(store_id);free(source);free(artifact);free(large_source);free(large_artifact);CHECK(!nftw(root,remove_file,32,FTW_DEPTH|FTW_PHYS));
    printf("PASS native actual Chutni SDK retrieval: %u checks; limits, ownership, offline replay/read, read-only store, artifact ranges/provenance and deadline\n",checks);return 0;
}
