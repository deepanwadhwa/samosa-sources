#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include "sha256.h"
#include <assert.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
extern char **environ;

static unsigned checks;
#define CHECK(x) do { checks++; if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1); } } while(0)
static char *call(const SourcesConfig *cfg,const char *request,int expected) {
    char *response=NULL;size_t n=0;FILE *out=open_memstream(&response,&n);CHECK(out!=NULL);
    int result=sources_dispatch(cfg,request,strlen(request),out);CHECK(fclose(out)==0);CHECK(result==expected);
    Json parsed;CHECK(json_parse(&parsed,response,n));return response;
}
static void expect_error(const SourcesConfig *cfg,const char *req,const char *code) {
    char *response=call(cfg,req,1);CHECK(strstr(response,code)!=NULL);free(response);
}
static void write_file(const char *path,const void *data,size_t length) {
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);
    CHECK(write(fd,data,length)==(ssize_t)length);CHECK(close(fd)==0);
}
static void read_req(char *out,size_t n,const char *id,const char *version,size_t start,size_t end) {
    snprintf(out,n,"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":%zu,\"end_byte\":%zu}",id,version,start,end);
}
static char *registered(const SourcesConfig *cfg,const char *path) {
    char req[512];snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"%s\"}",path);
    char *response=call(cfg,req,0);Json j;CHECK(json_parse(&j,response,strlen(response)));
    char *id=json_string(&j,json_get(&j,0,"source_id"));CHECK(id && strlen(id)==64);free(response);return id;
}
int main(int argc,char **argv) {
    if(argc==5&&!strcmp(argv[1],"--quota-worker")) {
        SourcesConfig worker={.root=argv[2],.state=argv[3],.snapshot_limit=1};
        char request[256];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"%s\"}",argv[4]);
        return sources_dispatch(&worker,request,strlen(request),stdout);
    }
    char temp[]="/tmp/samosa-native-s01-XXXXXX";CHECK(mkdtemp(temp)!=NULL);
    char root[512],state[512],path[1024],req[2048];
    snprintf(root,sizeof(root),"%s/root",temp);snprintf(state,sizeof(state),"%s/state",temp);
    CHECK(mkdir(root,0700)==0);SourcesConfig cfg={.root=root,.state=state};
    const char *invalid[]={"", "{", "{\"op\":}", "{} trailing", "[1,]", "{\"x\":01}", "{\"x\":truex}", "{\"x\":\"\\u0\"}", "{\"x\":\"\\ud800\"}", "{\"x\":\"\\udc00\"}", "{\"x\":\"\\u0000\"}", "{\"x\":\"\\q\"}", "{\"x\":1,\"\\u0078\":2}"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++)expect_error(&cfg,invalid[i],"invalid_json");
    expect_error(&cfg,"{\"contract_version\":2,\"op\":\"describe\"}","unsupported_contract");
    expect_error(&cfg,"{\"contract_version\":1,\"op\":\"describe\",\"root\":\"/\"}","unknown_field");
    Json unicode;CHECK(json_parse(&unicode,"\"\\ud83d\\ude00\"",14));char *smile=json_string(&unicode,0);CHECK(smile && !strcmp(smile,"\xf0\x9f\x98\x80"));free(smile);
    char digest[65];hash_bytes("",0,digest);CHECK(!strcmp(digest,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    hash_bytes("abc",3,digest);CHECK(!strcmp(digest,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    snprintf(path,sizeof(path),"%s/text.txt",root);const char text[]="hello \xc3\xa9\n7319";write_file(path,text,sizeof(text)-1);
    char *id=registered(&cfg,"text.txt");read_req(req,sizeof(req),id,id,0,sizeof(text)-1);
    char *response=call(&cfg,req,0);CHECK(strstr(response,"7319") && strstr(response,"complete") && strstr(response,"literal"));free(response);
    read_req(req,sizeof(req),id,id,6,8);response=call(&cfg,req,0);CHECK(strstr(response,"\xc3\xa9") && strstr(response,"partial"));free(response);
    read_req(req,sizeof(req),id,id,7,8);expect_error(&cfg,req,"utf8_boundary");
    read_req(req,sizeof(req),id,id,0,7);expect_error(&cfg,req,"utf8_boundary");
    read_req(req,sizeof(req),id,id,0,100);expect_error(&cfg,req,"invalid_range");
    read_req(req,sizeof(req),id,id,10,9);expect_error(&cfg,req,"invalid_range");
    read_req(req,sizeof(req),id,id,0,SOURCES_READ_MAX+1);expect_error(&cfg,req,"invalid_range");
    read_req(req,sizeof(req),id,digest,0,1);expect_error(&cfg,req,"stale_source");
    write_file(path,"changed",7);read_req(req,sizeof(req),id,id,0,sizeof(text)-1);
    response=call(&cfg,req,0);CHECK(strstr(response,"7319") && !strstr(response,"changed"));free(response);
    char *new_id=registered(&cfg,"text.txt");CHECK(strcmp(id,new_id));free(new_id);
    const char *badpaths[]={"../text.txt","/etc/passwd","./text.txt","text.txt/","sub//text.txt"};
    for(size_t i=0;i<sizeof(badpaths)/sizeof(*badpaths);i++) { snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"%s\"}",badpaths[i]);expect_error(&cfg,req,"unauthorized_path"); }
    snprintf(path,sizeof(path),"%s/link",root);CHECK(symlink("text.txt",path)==0);
    expect_error(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"link\"}","unauthorized_path");
    snprintf(path,sizeof(path),"%s/pipe",root);CHECK(mkfifo(path,0600)==0);
    expect_error(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"pipe\"}","unsupported_source");
    snprintf(path,sizeof(path),"%s/bad",root);write_file(path,"\xc0\x80",2);
    expect_error(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"bad\"}","invalid_utf8");
    write_file(path,"a\0b",3);expect_error(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"bad\"}","invalid_utf8");
    int fd=open(path,O_WRONLY|O_TRUNC);CHECK(fd>=0);CHECK(ftruncate(fd,SOURCES_INPUT_MAX+1)==0);close(fd);
    expect_error(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"bad\"}","source_too_large");
    char large[SOURCES_REQUEST_MAX+1];memset(large,' ',sizeof(large));FILE *sink=tmpfile();CHECK(sink);CHECK(sources_dispatch(&cfg,large,sizeof(large),sink)==1);fclose(sink);
    /* Corruption never becomes qualified evidence or a successful duplicate. */
    snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    snprintf(req,sizeof(req),"UPDATE snapshots SET bytes=x'78' WHERE id='%s'",id);CHECK(sqlite3_exec(db,req,NULL,NULL,NULL)==SQLITE_OK);sqlite3_close(db);
    read_req(req,sizeof(req),id,id,0,1);expect_error(&cfg,req,"corrupt_snapshot");free(id);
    /* Shared count quota is serialized across actual independent processes. */
    snprintf(state,sizeof(state),"%s/quota",temp);cfg.snapshot_limit=1;
    for(int i=0;i<8;i++) { snprintf(path,sizeof(path),"%s/item%d",root,i);char data=(char)('A'+i);write_file(path,&data,1); }
    response=call(&cfg,"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"bad\"}",1);free(response); /* initialize schema */
    pid_t children[8];char items[8][16];
    for(int i=0;i<8;i++) {
        snprintf(items[i],sizeof(items[i]),"item%d",i);
        char *worker_argv[]={argv[0],"--quota-worker",root,state,items[i],NULL};
        posix_spawn_file_actions_t actions;CHECK(posix_spawn_file_actions_init(&actions)==0);
        CHECK(posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0)==0);
        CHECK(posix_spawnp(&children[i],argv[0],&actions,NULL,worker_argv,environ)==0);
        posix_spawn_file_actions_destroy(&actions);
    }
    int successes=0;for(int i=0;i<8;i++){int status;CHECK(waitpid(children[i],&status,0)==children[i]);
        if(!WIFEXITED(status))fprintf(stderr,"quota child %d terminated by signal %d\n",i,WTERMSIG(status));
        CHECK(WIFEXITED(status));if(WEXITSTATUS(status)==0)successes++;}
    CHECK(successes==1);
    snprintf(path,sizeof(path),"%s/sources.sqlite3",state);CHECK(sqlite3_open(path,&db)==SQLITE_OK);
    sqlite3_stmt *stmt;CHECK(sqlite3_prepare_v2(db,"SELECT count(*) FROM snapshots",-1,&stmt,NULL)==SQLITE_OK);CHECK(sqlite3_step(stmt)==SQLITE_ROW);CHECK(sqlite3_column_int(stmt,0)==1);sqlite3_finalize(stmt);sqlite3_close(db);
    /* Cache quota is filled through successful public reads; hits remain usable
     * at capacity and changed selectors cannot evict prior evidence. */
    snprintf(state,sizeof(state),"%s/cache-quota",temp);cfg.snapshot_limit=0;cfg.cache_limit=3;
    snprintf(path,sizeof(path),"%s/cache-text",root);write_file(path,"cache bytes 7319",16);char *cache_id=registered(&cfg,"cache-text");char *first_cached=NULL;
    for(size_t i=0;i<3;i++){read_req(req,sizeof(req),cache_id,cache_id,i,i+1);response=call(&cfg,req,0);if(!i)first_cached=response;else free(response);}
    read_req(req,sizeof(req),cache_id,cache_id,3,4);expect_error(&cfg,req,"cache_limit");
    read_req(req,sizeof(req),cache_id,cache_id,0,1);response=call(&cfg,req,0);CHECK(!strcmp(response,first_cached));free(response);
    Json j;CHECK(json_parse(&j,first_cached,strlen(first_cached)));char *cache_key_id=json_string(&j,json_get(&j,0,"evidence_id"));CHECK(cache_key_id);
    snprintf(path,sizeof(path),"%s/sources.sqlite3",state);CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_prepare_v2(db,"SELECT count(*) FROM evidence_cache",-1,&stmt,NULL)==SQLITE_OK);CHECK(sqlite3_step(stmt)==SQLITE_ROW);CHECK(sqlite3_column_int(stmt,0)==3);sqlite3_finalize(stmt);
    snprintf(req,sizeof(req),"UPDATE evidence_cache SET response='{}' WHERE instr(response,'%s')>0",cache_key_id);CHECK(sqlite3_exec(db,req,NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_changes(db)==1);sqlite3_close(db);
    read_req(req,sizeof(req),cache_id,cache_id,0,1);expect_error(&cfg,req,"corrupt_cache");free(first_cached);free(cache_key_id);free(cache_id);
    /* Byte quota admits one real result and refuses a second without eviction. */
    snprintf(state,sizeof(state),"%s/cache-bytes",temp);cfg.cache_limit=0;cfg.cache_byte_limit=1024;cache_id=registered(&cfg,"cache-text");
    read_req(req,sizeof(req),cache_id,cache_id,0,1);first_cached=call(&cfg,req,0);
    read_req(req,sizeof(req),cache_id,cache_id,1,2);expect_error(&cfg,req,"cache_limit");
    read_req(req,sizeof(req),cache_id,cache_id,0,1);response=call(&cfg,req,0);CHECK(!strcmp(first_cached,response));free(response);free(first_cached);free(cache_id);
    printf("PASS native Sources: %u checks; concurrent quota 1 accepted/7 refused\n",checks);
    /* All test artifacts are underneath this owned mkdtemp path. */
    char *args[]={"find",temp,"-depth","-delete",NULL};pid_t cleanup=fork();CHECK(cleanup>=0);
    if(!cleanup){execvp(args[0],args);_exit(127);}int status;CHECK(waitpid(cleanup,&status,0)==cleanup);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
    return 0;
}
