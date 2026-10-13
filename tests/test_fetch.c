#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "fetch.h"
#include "native.h"
#include "control.h"
#include "json.h"
#include "sha256.h"
#include <assert.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);abort();}}while(0)
static double monotonic(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int stop_later(void *context){return monotonic()>=*(double *)context;}
static int stop(void *context){(void)context;return 1;}
static void send_all(int fd,const void *bytes,size_t n){
    const unsigned char *p=bytes;
    while(n){ssize_t sent=send(fd,p,n,0);if(sent<=0)_exit(0);p+=sent;n-=(size_t)sent;}
}
static pid_t fixture(unsigned short *port,int mode){
    int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    CHECK(!bind(fd,(struct sockaddr *)&address,sizeof(address)));CHECK(!listen(fd,1));
    socklen_t length=sizeof(address);CHECK(!getsockname(fd,(struct sockaddr *)&address,&length));*port=ntohs(address.sin_port);
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        signal(SIGPIPE,SIG_IGN);int peer=accept(fd,NULL,NULL);if(peer<0)_exit(1);close(fd);
        char request[4096]={0};size_t received=0;
        while(!strstr(request,"\r\n\r\n")){ssize_t got=recv(peer,request+received,sizeof(request)-1-received,0);if(got<=0)_exit(1);received+=(size_t)got;if(received==sizeof(request)-1)_exit(1);}
        if(mode==4||mode==7){struct timespec delay={.tv_sec=10};nanosleep(&delay,NULL);_exit(0);}
        if(mode==8){
            send_all(peer,"HTTP/1.1 200 OK\r\n",17);
            char field[1024];memset(field,'x',sizeof(field));memcpy(field,"X-Fill: ",8);field[1022]='\r';field[1023]='\n';
            for(int i=0;i<65;i++)send_all(peer,field,sizeof(field));
            send_all(peer,"Content-Length: 0\r\n\r\n",21);close(peer);_exit(0);
        }
        const char *header=mode==1?"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/private\r\nContent-Length: 0\r\nConnection: close\r\n\r\n":
            mode==2?"HTTP/1.1 200 OK\r\nContent-Length: 5242881\r\nConnection: close\r\n\r\n":
            mode==3?"HTTP/1.1 200 OK\r\nContent-Length: 50\r\nConnection: close\r\n\r\n":
            mode==5?"HTTP/1.1 200 OK\r\nContent-Length: 5242880\r\nConnection: close\r\n\r\n":
            mode==6?"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n":
            "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: close\r\n\r\n";
        send_all(peer,header,strlen(header));
        if(mode==0||mode==3)send_all(peer,"fetch 7510 ok",13);
        if(mode==6)send_all(peer,"d\r\nfetch 7510 ok\r\n0\r\n\r\n",23);
        if(mode==2||mode==5){char block[4096];memset(block,'x',sizeof(block));for(size_t i=0;i<SOURCES_FETCH_MAX;i+=sizeof(block))send_all(peer,block,sizeof(block));if(mode==2)send_all(peer,"x",1);}
        close(peer);_exit(0);
    }
    close(fd);return child;
}
static char *acquire(SourcesConfig *cfg,const char *url,int expected){
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);
    int result=sources_fetch(cfg,SOURCES_UPLOAD_TEXT,url,out);CHECK(!fclose(out));
    if(result!=expected)fprintf(stderr,"unexpected fetch: %s\n",body);
    CHECK(result==expected);return body;
}
static void validation(void){
    const char *url="https://example.test/source";
    SourcesConfig cfg={.fetch_origin="https://example.test",.fetch_helper="worker"};
    CHECK(!sources_fetch_authorize(&cfg,url));CHECK(!sources_fetch_authorize(&cfg,"https://example.test?query=1"));
    const char *invalid[]={"https://example.test.evil/source","https://example.test@evil/source","https://example.test:443/source","https://example.test/#fragment","https://example.test/\\evil","https://example.test/%zz","https://example.test/%","https://example.test/ bad","http://example.test/source"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++)CHECK(sources_fetch_authorize(&cfg,invalid[i]));
    cfg.fetch_origin="http://example.test";CHECK(sources_fetch_authorize(&cfg,"http://example.test/a"));
    cfg.fetch_origin="https://example.test:0";CHECK(sources_fetch_authorize(&cfg,url));
    char hash[65],body[512];hash_bytes("f",1,hash);
    snprintf(body,sizeof(body),"{\"url\":\"%s\",\"status\":200,\"bytes\":1,\"digest\":\"%s\",\"data\":\"Zg==\"}",url,hash);
    unsigned char *bytes=NULL;size_t n=0;
    CHECK(!sources_fetch_validate(url,body,strlen(body),&bytes,&n));CHECK(n==1&&bytes[0]=='f');free(bytes);
    char *pad=strstr(body,"Zg==");CHECK(pad);pad[1]='h';CHECK(sources_fetch_validate(url,body,strlen(body),&bytes,&n));CHECK(!bytes&&!n);pad[1]='g';
    CHECK(sources_fetch_validate("https://foreign.test",body,strlen(body),&bytes,&n));
    char *digest=strstr(body,hash);CHECK(digest);digest[0]=digest[0]=='a'?'b':'a';CHECK(sources_fetch_validate(url,body,strlen(body),&bytes,&n));
    const char *bad[]={"{}","[]","{\"url\":\"x\",\"url\":\"x\"}","{\"url\":\"x\",\"status\":200,\"bytes\":1048577,\"digest\":\"x\",\"data\":\"\"}"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(sources_fetch_validate(url,bad[i],strlen(bad[i]),&bytes,&n));
}
static char *dispatch(SourcesConfig *cfg,const char *request,int expected){
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);
    int rc=sources_dispatch(cfg,request,strlen(request),out);CHECK(!fclose(out));
    if(rc!=expected)fprintf(stderr,"unexpected public fetch: %s\n",body);
    CHECK(rc==expected);return body;
}
static char *cli_call(const char *cli,const char *worker,const char *origin,const char *state,const char *request){
    int input[2],output[2];CHECK(!pipe(input));CHECK(!pipe(output));pid_t child=fork();CHECK(child>=0);
    if(!child){
        close(input[1]);close(output[0]);if(dup2(input[0],0)<0||dup2(output[1],1)<0)_exit(125);close(input[0]);close(output[1]);
        if(worker)execl(cli,cli,"--state",state,"--owner","cli-owner","--fetch-helper",worker,"--fetch-origin",origin,(char *)NULL);
        else execl(cli,cli,"--state",state,"--owner","cli-owner",(char *)NULL);
        _exit(125);
    }
    close(input[0]);close(output[1]);size_t offset=0,n=strlen(request);
    while(offset<n){ssize_t wrote=write(input[1],request+offset,n-offset);CHECK(wrote>0);offset+=(size_t)wrote;}close(input[1]);
    char body[4096]={0};offset=0;for(;;){ssize_t got=read(output[0],body+offset,sizeof(body)-1-offset);CHECK(got>=0);if(!got)break;offset+=(size_t)got;CHECK(offset<sizeof(body)-1);}close(output[0]);
    int status;CHECK(waitpid(child,&status,0)==child);if(!WIFEXITED(status)||WEXITSTATUS(status))fprintf(stderr,"CLI: %s\n",body);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));char *copy=strdup(body);CHECK(copy);return copy;
}
static void cli_fetch(const char *cli,const char *worker){
    char state[]="/tmp/samosa-cli-fetch-XXXXXX";CHECK(mkdtemp(state));unsigned short port;pid_t server=fixture(&port,0);
    char origin[128],request[1024];snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"url\":\"%s/source\",\"format\":\"text\",\"acquisition_id\":\"cli-capture\",\"operation_id\":\"cli-attempt-1\"}",origin);
    char *body=cli_call(cli,worker,origin,state,request);CHECK(strstr(body,"cli-capture"));free(body);int status;CHECK(waitpid(server,&status,0)==server);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"url\":\"%s/source\",\"format\":\"text\",\"acquisition_id\":\"cli-capture\",\"operation_id\":\"cli-attempt-2\"}",origin);
    body=cli_call(cli,NULL,NULL,state,request);CHECK(strstr(body,"cli-capture"));CHECK(strstr(body,"cli-attempt-2"));free(body);
}
static void public_fetch(const char *worker){
    char state[]="/tmp/samosa-public-fetch-XXXXXX";CHECK(mkdtemp(state));
    unsigned short port;pid_t server=fixture(&port,0);char origin[128],url[256],request[1024];
    snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);snprintf(url,sizeof(url),"%s/source",origin);
    SourcesConfig cfg={.state=state,.owner="public-fetch-owner",.fetch_helper=worker,.fetch_origin=origin};
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"capture-1\",\"url\":\"%s\",\"format\":\"text\",\"operation_id\":\"attempt-1\",\"parent_operation_id\":\"parent-1\",\"timeout_ms\":5000}",url);
    char *first=dispatch(&cfg,request,0);CHECK(strstr(first,"capture-1"));CHECK(strstr(first,"parent-1"));CHECK(strstr(first,url));int status;CHECK(waitpid(server,&status,0)==server);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    cfg.fetch_origin=NULL;cfg.fetch_helper=NULL;
    char *body=dispatch(&cfg,request,0);CHECK(!strcmp(first,body));free(body);
    snprintf(request,sizeof(request),"{\"format\":\"text\",\"url\":\"%s\",\"acquisition_id\":\"capture-1\",\"op\":\"fetch\",\"contract_version\":1,\"operation_id\":\"attempt-2\"}",url);
    body=dispatch(&cfg,request,0);CHECK(strstr(body,"attempt-2"));CHECK(strstr(body,"capture-1"));free(body);
    body=dispatch(&cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"attempt-1\"}",0);CHECK(strstr(body,"completed"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"capture-1\",\"url\":\"%s/changed\",\"format\":\"text\"}",url);
    body=dispatch(&cfg,request,1);CHECK(strstr(body,"idempotency_conflict"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"capture-1\",\"url\":\"%s\",\"format\":\"text\"}",url);
    cfg.owner="other-owner";body=dispatch(&cfg,request,1);CHECK(strstr(body,"fetch_unavailable"));free(body);cfg.owner="public-fetch-owner";
    body=dispatch(&cfg,"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"missing\",\"url\":\"https://example.test\",\"format\":\"text\",\"fetch_helper\":\"client-selected\"}",1);CHECK(strstr(body,"unknown_field"));free(body);
    char path[512];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"UPDATE fetch_receipts SET digest=printf('%064d',0)",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
    body=dispatch(&cfg,request,1);CHECK(strstr(body,"corrupt_acquisition"));free(body);free(first);
}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
static void receipt_boundary(const char *worker,int mode){
    int after=mode==1||mode==4;
    fprintf(stderr,"fetch receipt boundary mode %d\n",mode);
    char state[]="/tmp/samosa-fetch-boundary-XXXXXX";CHECK(mkdtemp(state));
    unsigned short port;pid_t server=fixture(&port,0);char origin[128],url[256],request[1024],marker[512];
    snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);snprintf(url,sizeof(url),"%s/source",origin);snprintf(marker,sizeof(marker),"%s/ready",state);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"durable-capture\",\"url\":\"%s\",\"format\":\"text\",\"operation_id\":\"killed-attempt\"}",url);
    SourcesConfig cfg={.state=state,.owner="boundary-owner",.fetch_origin=origin,.fetch_helper=worker};
    if(mode==3)cfg.deadline_monotonic=monotonic()+5;
    pid_t child=fork();CHECK(child>=0);
    if(!child){CHECK(!setenv(after?"SAMOSA_FETCH_TEST_POSTCOMMIT_MARKER":"SAMOSA_FETCH_TEST_PRECOMMIT_MARKER",marker,1));char *body=dispatch(&cfg,request,mode==2||mode==3?1:0);if(mode==2)CHECK(strstr(body,"cancelled"));if(mode==3)CHECK(strstr(body,"deadline_exceeded"));free(body);_exit(0);}
    double end=monotonic()+10;while(access(marker,F_OK)&&monotonic()<end){struct timespec delay={0,10000000};nanosleep(&delay,NULL);}
    CHECK(!access(marker,F_OK));int status=0;
    if(mode<2)CHECK(!kill(child,SIGKILL));
    else{
        if(mode==2||mode==4){char *cancel=dispatch(&cfg,"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"killed-attempt\"}",0);free(cancel);}
        if(mode==3){while(monotonic()<cfg.deadline_monotonic+0.05){struct timespec delay={0,10000000};nanosleep(&delay,NULL);}cfg.deadline_monotonic=0;}
        char release[600];snprintf(release,sizeof(release),"%s.release",marker);int fd=open(release,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(!close(fd));
    }
    CHECK(waitpid(child,&status,0)==child);
    if(mode<2)CHECK(WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);else CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(waitpid(server,&status,0)==server);
    char path[512];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);sqlite3_stmt *s=NULL;
    CHECK(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM fetch_receipts),(SELECT count(*) FROM snapshots),(SELECT count(*) FROM source_claims)",-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_ROW);for(int i=0;i<3;i++)CHECK(sqlite3_column_int(s,i)==after);sqlite3_finalize(s);CHECK(sqlite3_close(db)==SQLITE_OK);
    char *body=dispatch(&cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"killed-attempt\"}",0);CHECK(strstr(body,mode<2?"interrupted":mode==2?"cancelled":mode==3?"failed":"completed"));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"durable-capture\",\"url\":\"%s\",\"format\":\"text\",\"operation_id\":\"recovery-attempt\"}",url);
    if(after){cfg.fetch_helper=NULL;cfg.fetch_origin=NULL;body=dispatch(&cfg,request,0);CHECK(strstr(body,"durable-capture"));CHECK(strstr(body,"recovery-attempt"));free(body);}
    if(after){cfg.fetch_offline=1;snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"durable-capture\",\"url\":\"%s\",\"format\":\"text\",\"operation_id\":\"offline-attempt\"}",url);body=dispatch(&cfg,request,0);CHECK(strstr(body,"durable-capture"));free(body);cfg.fetch_offline=0;}
    else{body=dispatch(&cfg,request,1);CHECK(strstr(body,"fetch_failed"));free(body);}
}
#endif
int main(int argc,char **argv){
    CHECK(argc==3);signal(SIGPIPE,SIG_IGN);validation();public_fetch(argv[1]);cli_fetch(argv[2],argv[1]);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    for(int mode=0;mode<5;mode++)receipt_boundary(argv[1],mode);
#endif
    char state[]="/tmp/samosa-fetch-XXXXXX";CHECK(mkdtemp(state));
    SourcesConfig cfg={.state=state,.owner="fetch-owner",.fetch_helper=argv[1]};
    char origin[128],url[256],id[65]={0};
    for(int mode=0;mode<=8;mode++){
        unsigned short port=0;pid_t child=fixture(&port,mode);
        snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);snprintf(url,sizeof(url),"%s/source",origin);cfg.fetch_origin=origin;
        cfg.deadline_monotonic=mode==4?monotonic()+0.3:0;
        double cancel_at=monotonic()+0.3;
        cfg.cancelled=mode==7?stop_later:NULL;cfg.cancellation=&cancel_at;
        fprintf(stderr,"fetch fixture mode %d\n",mode);
        char *body=acquire(&cfg,url,mode==0||mode==5||mode==6?0:1);
        if(mode==0){Json j;CHECK(json_parse(&j,body,strlen(body)));char *value=json_string(&j,json_get(&j,0,"source_id"));CHECK(value&&strlen(value)==64);memcpy(id,value,65);free(value);}
        if(mode==1)CHECK(strstr(body,"fetch_http_status"));
        if(mode==2)CHECK(strstr(body,"fetch_too_large"));
        if(mode==3)CHECK(strstr(body,"fetch_failed"));
        if(mode==4){CHECK(strstr(body,"deadline_exceeded"));CHECK(!kill(child,SIGTERM));}
        if(mode==5){
            CHECK(strstr(body,"5242880"));Sha256 hash;sha256_init(&hash);
            char block[4096];memset(block,'x',sizeof(block));
            for(size_t i=0;i<SOURCES_FETCH_MAX;i+=sizeof(block))sha256_update(&hash,block,sizeof(block));
            unsigned char digest[32];char expected[65];sha256_final(&hash,digest);sha256_hex(digest,expected);
            Json j;CHECK(json_parse(&j,body,strlen(body)));char *version=json_string(&j,json_get(&j,0,"version"));CHECK(version&&!strcmp(version,expected));
            char selection[512];snprintf(selection,sizeof(selection),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":%u,\"end_byte\":%u}",version,version,SOURCES_FETCH_MAX-5,SOURCES_FETCH_MAX);
            char *tail=dispatch(&cfg,selection,0);CHECK(strstr(tail,"xxxxx"));free(tail);free(version);
        }
        if(mode==7){CHECK(strstr(body,"cancelled"));CHECK(!kill(child,SIGTERM));}
        if(mode==8)CHECK(strstr(body,"fetch_header_limit"));
        free(body);int status=0;CHECK(waitpid(child,&status,0)==child);if(mode!=4&&mode!=7)CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    cfg.cancelled=NULL;cfg.cancellation=NULL;cfg.deadline_monotonic=0;char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":13}",id,id);
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);CHECK(!sources_dispatch(&cfg,request,strlen(request),out));CHECK(!fclose(out));CHECK(strstr(body,"fetch 7510 ok"));CHECK(strstr(body,"http://127.0.0.1:"));free(body);
    cfg.owner="foreign-owner";out=open_memstream(&body,&n);CHECK(out);CHECK(sources_dispatch(&cfg,request,strlen(request),out)==1);CHECK(!fclose(out));CHECK(strstr(body,"unknown_source"));free(body);
    cfg.owner="fetch-owner";cfg.cancelled=stop;body=acquire(&cfg,url,1);CHECK(strstr(body,"cancelled"));free(body);cfg.cancelled=NULL;
    body=acquire(&cfg,"https://foreign.test/source",1);CHECK(strstr(body,"unauthorized_url"));free(body);
    printf("PASS fetch host API/isolated HTTP/boundary validation: %u checks\n",checks);return 0;
}
