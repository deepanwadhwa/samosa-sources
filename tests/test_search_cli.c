#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "json.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const char *query="a \"quoted\" query {api_key}",*secret="private-credential/cli";
static void write_all(int fd,const void *data,size_t n){const char *p=data;while(n){ssize_t k=write(fd,p,n);if(k<0&&errno==EINTR)continue;assert(k>0);p+=k;n-=(size_t)k;}}
static void file(const char *path,const void *bytes,size_t n){int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);write_all(fd,bytes,n);assert(!close(fd));}
static pid_t server(unsigned short *port){
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    assert(!bind(fd,(struct sockaddr*)&addr,sizeof(addr))&&!listen(fd,2));socklen_t size=sizeof(addr);
    assert(!getsockname(fd,(struct sockaddr*)&addr,&size));*port=ntohs(addr.sin_port);
    pid_t pid=fork();assert(pid>=0);
    if(!pid){
        alarm(60);
        for(unsigned i=0;i<2;i++){
            int peer=accept(fd,NULL,NULL);assert(peer>=0);char request[16384]={0};size_t used=0;char *end=NULL;
            while(!(end=strstr(request,"\r\n\r\n"))){ssize_t got=read(peer,request+used,sizeof(request)-1-used);assert(got>0);used+=(size_t)got;assert(used<sizeof(request)-1);}
            if(!i){
                assert(!strncmp(request,"POST /search HTTP/",18));
                assert(strstr(request,"Authorization: Bearer private-credential/cli\r\n"));
                char *length=strstr(request,"Content-Length: ");assert(length);
                size_t n=strtoul(length+16,NULL,10),header=(size_t)(end-request)+4;assert(n<sizeof(request)-header);
                while(used<header+n){ssize_t got=read(peer,request+used,sizeof(request)-1-used);assert(got>0);used+=(size_t)got;}
                Json j;assert(json_parse(&j,request+header,n));char *posted=json_string(&j,json_get(&j,0,"query"));assert(posted&&!strcmp(posted,query));free(posted);
            }else{
                const char expected[]="GET /search?q=a%20%22quoted%22%20query%20%7Bapi_key%7D HTTP/";
                assert(!strncmp(request,expected,sizeof(expected)-1));
                assert(strstr(request,"X-Subscription-Token: private-credential/cli\r\n"));
            }
            const char *body=!i?"{\"results\":[{\"title\":\"Actual CLI result\",\"url\":\"https://example.test/result\",\"description\":\"private-credential/cli\"}]}":"{\"web\":{\"results\":[{\"title\":\"GET preset result\",\"url\":\"https://example.test/get\",\"description\":\"private-credential/cli\"}]}}";
            char headers[256];int n=snprintf(headers,sizeof(headers),"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",strlen(body));
            write_all(peer,headers,(size_t)n);write_all(peer,body,strlen(body));assert(!close(peer));
        }
        close(fd);_exit(0);
    }
    close(fd);return pid;
}
typedef struct {char out[16384],err[16384];int code;} Reply;
static Reply run(const char *cli,const char *state,char *const *options,const char *request){
    int input[2],output[2],error[2];assert(!pipe(input)&&!pipe(output)&&!pipe(error));
    pid_t pid=fork();assert(pid>=0);
    if(!pid){
        alarm(15);close(input[1]);close(output[0]);close(error[0]);
        assert(dup2(input[0],0)==0&&dup2(output[1],1)==1&&dup2(error[1],2)==2);
        close(input[0]);close(output[1]);close(error[1]);
        char *args[32]={(char*)cli,"--state",(char*)state,"--owner","cli-owner"};size_t n=5;
        if(options)for(size_t i=0;options[i];i++){assert(n<31);args[n++]=options[i];}
        args[n]=NULL;execv(cli,args);_exit(125);
    }
    close(input[0]);close(output[1]);close(error[1]);write_all(input[1],request,strlen(request));close(input[1]);
    Reply reply={0};struct pollfd fds[2]={{output[0],POLLIN,0},{error[0],POLLIN,0}};size_t sizes[2]={0};unsigned active=2;
    while(active){int ready=poll(fds,2,16000);if(ready<0&&errno==EINTR)continue;assert(ready>0);
        for(unsigned i=0;i<2;i++)if(fds[i].fd>=0&&fds[i].revents){char *buffer=i?reply.err:reply.out;
            ssize_t got=read(fds[i].fd,buffer+sizes[i],sizeof(reply.out)-1-sizes[i]);assert(got>=0);
            if(!got){close(fds[i].fd);fds[i].fd=-1;active--;}else{sizes[i]+=(size_t)got;assert(sizes[i]<sizeof(reply.out)-1);}
        }
    }
    int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status));reply.code=WEXITSTATUS(status);
    assert(!strstr(reply.out,secret)&&!strstr(reply.err,secret));return reply;
}
static void refused(Reply r){assert(r.code==2&&!r.out[0]);}
static void redacted(Reply r){if(r.code)fprintf(stderr,"CLI: %s%s\n",r.out,r.err);assert(!r.code);Json j;assert(json_parse(&j,r.out,strlen(r.out)));int search=json_get(&j,0,"search"),items=json_get(&j,search,"items");assert(items>=0&&j.tokens[items].count==1);char *description=json_string(&j,json_get(&j,items+1,"description"));assert(description&&!strcmp(description,"<redacted>"));free(description);}
int main(int argc,char **argv){
    assert(argc==3);signal(SIGPIPE,SIG_IGN);char root[]="/tmp/samosa-search-cli-XXXXXX";assert(mkdtemp(root));
    char state[512],config[512],values[512],link[512],origin[128],url[256],declaration[2048];
    snprintf(state,sizeof(state),"%s/state",root);snprintf(config,sizeof(config),"%s/search.json",root);snprintf(values,sizeof(values),"%s/values.json",root);snprintf(link,sizeof(link),"%s/config-link.json",root);
    unsigned short port;pid_t fixture=server(&port);snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);snprintf(url,sizeof(url),"%s/search?q={query}",origin);
    int n=snprintf(declaration,sizeof(declaration),"{\"provider\":\"fixture\",\"origin\":\"%s\",\"url_template\":\"%s/search\",\"headers\":{\"Authorization\":\"Bearer {api_key}\"},\"body\":{\"query\":\"{query}\"},\"results_path\":\"results\",\"title_path\":\"title\",\"url_path\":\"url\",\"description_path\":\"description\"}",origin,origin);
    file(config,declaration,(size_t)n);const char *credentials="{\"api_key\":\"private-credential/cli\"}";file(values,credentials,strlen(credentials));
    const char *request="{\"contract_version\":1,\"op\":\"search\",\"query\":\"a \\\"quoted\\\" query {api_key}\",\"operation_id\":\"cli-search\",\"timeout_ms\":5000}";
    char *options[]={"--search-config-file",config,"--search-values-file",values,"--fetch-helper",argv[2],NULL};
    Reply accepted=run(argv[1],state,options,request);redacted(accepted);
    Reply replay=run(argv[1],state,NULL,request);assert(!replay.code&&!strcmp(accepted.out,replay.out));
    Json j;assert(json_parse(&j,accepted.out,strlen(accepted.out)));char *id=json_string(&j,json_get(&j,0,"source_id")),*version=json_string(&j,json_get(&j,0,"version"));assert(id&&version);
    char read[512];snprintf(read,sizeof(read),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":100}",id,version);
    Reply evidence=run(argv[1],state,NULL,read);assert(!evidence.code);free(id);free(version);
    const char *paths[]={config,values};for(unsigned i=0;i<2;i++){assert(!chmod(paths[i],0644));refused(run(argv[1],state,options,request));assert(!chmod(paths[i],0600));}
    assert(!symlink(config,link));char *linked[]={"--search-config-file",link,NULL};refused(run(argv[1],state,linked,request));
    char oversized[8193];memset(oversized,'x',sizeof(oversized));file(config,oversized,sizeof(oversized));refused(run(argv[1],state,options,request));
    file(config,declaration,(size_t)n+1);refused(run(argv[1],state,options,request));
    file(config,"{\"unknown\":true}",16);refused(run(argv[1],state,options,request));
    char *get[]={"--search-provider","brave","--search-values-file",values,"--search-origin",origin,"--search-url",url,"--fetch-helper",argv[2],NULL};
    const char *get_request="{\"contract_version\":1,\"op\":\"search\",\"query\":\"a \\\"quoted\\\" query {api_key}\",\"operation_id\":\"cli-get\",\"timeout_ms\":5000}";
    redacted(run(argv[1],state,get,get_request));int status;assert(waitpid(fixture,&status,0)==fixture&&WIFEXITED(status)&&!WEXITSTATUS(status));
    puts("PASS native search CLI: exact POST/GET query/headers, private declarations/credentials, redaction, offline replay/read and permission/symlink/size/NUL/schema refusal");return 0;
}
