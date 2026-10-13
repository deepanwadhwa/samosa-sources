#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "native.h"
#include "control.h"
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
/* The enclosing native worker supplies process-group death, parent monitoring,
 * descriptor isolation and cancellation. This adapter only transports public JSON. */
static int channel(int pair[2]){
    if(socketpair(AF_UNIX,SOCK_STREAM,0,pair))return 0;
    for(size_t i=0;i<2;i++){
        int fd=fcntl(pair[i],F_DUPFD_CLOEXEC,4);
        if(fd<0){close(pair[0]);close(pair[1]);return 0;}
        close(pair[i]);pair[i]=fd;
#ifdef SO_NOSIGPIPE
        int enabled=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled));
#endif
    }
    return 1;
}
const char *sources_inference_process_worker(const SourcesConfig *cfg,const void *input,size_t length,FILE *out){
    Json j;if(length>50331648u||!json_parse(&j,input,length)||j.tokens[0].type!=JT_OBJECT)return "invalid_configuration";
    size_t temporal=0;
    if(!json_uint(&j,json_get(&j,0,"temporal_frames"),&temporal)||temporal>1||
       length>(temporal?50331648u:4194304u))return "invalid_configuration";
    char *executable=json_string(&j,json_get(&j,0,"executable")),*request=json_string(&j,json_get(&j,0,"request"));
    int args=json_get(&j,0,"arguments");char *argv[66]={0};size_t count=1;argv[0]=executable;
    const char *error=NULL;
    if(!executable||executable[0]!='/'||strlen(executable)>4095||!request||!strlen(request)||strlen(request)>(temporal?25165824u:2097152u)||args<0||j.tokens[args].type!=JT_ARRAY)error="invalid_configuration";
    if(!error)for(int t=args+1;t<j.tokens[args].next;t=j.tokens[t].next){
        if(count==65){error="invalid_configuration";break;}
        char *arg=json_string(&j,t);if(!arg||strlen(arg)>4095){free(arg);error="invalid_configuration";break;}argv[count++]=arg;
    }
    int send_pair[2]={-1,-1},receive_pair[2]={-1,-1};pid_t pid=-1;int status=0,reaped=0;
    if(!error&&(!channel(send_pair)||!channel(receive_pair)))error="inference_unavailable";
    if(!error){
        posix_spawn_file_actions_t actions;int rc=posix_spawn_file_actions_init(&actions);
        int initialized=!rc;
        if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,send_pair[1],0);
        if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,receive_pair[1],1);
        if(!rc)rc=posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
        /* Protocol fd3 and socket endpoints are CLOEXEC. */
        if(!rc)rc=posix_spawn(&pid,executable,&actions,NULL,argv,environ);
        if(initialized)posix_spawn_file_actions_destroy(&actions);
        if(rc)error="inference_unavailable";
    }
    if(send_pair[1]>=0){close(send_pair[1]);send_pair[1]=-1;}
    if(receive_pair[1]>=0){close(receive_pair[1]);receive_pair[1]=-1;}
    char *response=NULL;size_t used=0,sent=0;int eof=0,finished_input=0;
    if(!error){response=malloc(262145u);if(!response)error="resource_limit";}
    if(!error&&(fcntl(send_pair[0],F_SETFL,O_NONBLOCK)<0||fcntl(receive_pair[0],F_SETFL,O_NONBLOCK)<0))error="inference_unavailable";
    size_t request_bytes=request?strlen(request):0;
    while(!error&&(!eof||!reaped)){
        error=sources_interrupted(cfg);if(error)break;
        if(!finished_input&&sent==request_bytes){shutdown(send_pair[0],SHUT_WR);finished_input=1;}
        struct pollfd ports[2]={{finished_input?-1:send_pair[0],POLLOUT,0},{eof?-1:receive_pair[0],POLLIN,0}};
        int rc=poll(ports,2,20);if(rc<0&&errno!=EINTR){error="inference_unavailable";break;}
        if(ports[0].revents&(POLLOUT|POLLERR|POLLHUP)){
            ssize_t n=send(send_pair[0],request+sent,request_bytes-sent,
#ifdef MSG_NOSIGNAL
                MSG_NOSIGNAL
#else
                0
#endif
            );
            if(n>0)sent+=(size_t)n;
            else if(n<0&&errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK)error="inference_unavailable";
        }
        if(!error&&(ports[1].revents&(POLLIN|POLLERR|POLLHUP))){
            ssize_t n=recv(receive_pair[0],response+used,262145u-used,0);
            if(n>0){used+=(size_t)n;if(used>262144u)error="inference_response_invalid";}
            else if(!n)eof=1;
            else if(errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK)error="inference_unavailable";
        }
        if(!reaped){pid_t got=waitpid(pid,&status,WNOHANG);if(got==pid)reaped=1;else if(got<0&&errno!=EINTR)error="inference_unavailable";}
    }
    if(pid>0&&!reaped){kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}
    for(size_t i=0;i<2;i++){if(send_pair[i]>=0)close(send_pair[i]);if(receive_pair[i]>=0)close(receive_pair[i]);}
    if(!error&&(!finished_input||!WIFEXITED(status)||WEXITSTATUS(status)>1||!used||memchr(response,0,used)))error="inference_response_invalid";
    if(!error){fputs("{\"response\":",out);if(fwrite(response,1,used,out)!=used)error="resource_limit";fputc('}',out);}
    free(response);free(request);for(size_t i=1;i<count;i++)free(argv[i]);free(executable);return error;
}
