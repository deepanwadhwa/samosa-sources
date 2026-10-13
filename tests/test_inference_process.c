#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
static double now(void){struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int stopped(void *p){return now()>*(double*)p;}
int main(int argc,char **argv){
    if(argc==2||(argc==3&&!strcmp(argv[1],"record"))){
        char *input=malloc(25165825u);if(!input)return 2;size_t n=fread(input,1,25165825u,stdin);free(input);if(n<1)return 2;
        if(!strcmp(argv[1],"stall")){for(;;)pause();}
        if(!strcmp(argv[1],"record")){FILE *marker=fopen(argv[2],"w");if(!marker)return 2;fprintf(marker,"%ld",(long)getpid());fclose(marker);for(;;)pause();}
        if(!strcmp(argv[1],"big")){for(size_t i=0;i<262145;i++)fputc('x',stdout);return 0;}
        if(!strcmp(argv[1],"nul")){fwrite("{}\0",1,3,stdout);return 0;}
        if(!strcmp(argv[1],"busy")){fputs("{\"contract_version\":1,\"error\":{\"code\":\"busy\"}}",stdout);return 1;}
        if(!strcmp(argv[1],"exit"))return 7;
        if(!strcmp(argv[1],"limits")){struct rlimit cpu,address;CHECK(!getrlimit(RLIMIT_CPU,&cpu)&&!getrlimit(RLIMIT_AS,&address));fprintf(stdout,"{\"contract_version\":1,\"cpu_seconds\":%llu,\"address_bytes\":%llu}",(unsigned long long)cpu.rlim_cur,(unsigned long long)address.rlim_cur);return 0;}
        fprintf(stdout,"{\"contract_version\":1,\"received\":%zu}",n);return 0;
    }
    CHECK(argc==3);SourcesConfig cfg={.native_helper=argv[1]};SourcesInferenceProcess process={.sources=&cfg,.executable=argv[2],.arguments_json="[\"normal\"]"};SourcesInferenceControl control={.deadline_monotonic=now()+5};
    char *request=malloc(1280100);CHECK(request);memset(request,'a',1280000);request[1280000]=0;
    char *response=NULL;size_t bytes=0;
    CHECK(!sources_inference_process_call(&process,&control,request,1280000,&response,&bytes));CHECK(response&&bytes==strlen(response));Json j;CHECK(json_parse(&j,response,bytes));size_t received=0;CHECK(json_uint(&j,json_get(&j,0,"received"),&received)&&received==1280000);free(response);
    char *temporal_request=malloc(25165826u);CHECK(temporal_request);
    memset(temporal_request,'a',25165825u);temporal_request[25165825u]=0;
    const char *limit_error=sources_inference_process_call(&process,&control,temporal_request,25165824u,&response,&bytes);
    CHECK(limit_error&&!strcmp(limit_error,"invalid_configuration"));
    process.temporal_frames=1;control.deadline_monotonic=now()+20;
    CHECK(!sources_inference_process_call(&process,&control,temporal_request,25165824u,&response,&bytes));
    CHECK(json_parse(&j,response,bytes));CHECK(json_uint(&j,json_get(&j,0,"received"),&received)&&received==25165824u);free(response);
    limit_error=sources_inference_process_call(&process,&control,temporal_request,25165825u,&response,&bytes);
    CHECK(limit_error&&!strcmp(limit_error,"invalid_configuration"));
    free(temporal_request);process.temporal_frames=0;
    const char *modes[]={"[\"busy\"]","[\"big\"]","[\"nul\"]","[\"exit\"]","[\"stall\"]","[\"stall\"]"};
    for(size_t i=0;i<12;i++){
        size_t mode=i%6;process.temporal_frames=i>=6;process.arguments_json=modes[mode];control.deadline_monotonic=now()+(mode>=4?.15:5);double cancel=now()+.05;
        control.cancelled=mode==5?stopped:NULL;control.cancellation=&cancel;
        const char *error=sources_inference_process_call(&process,&control,"{}",2,&response,&bytes);
        if(!mode){CHECK(!error&&strstr(response,"busy"));free(response);}
        else{const char *expected=mode==5?"cancelled":mode==4?"deadline_exceeded":"inference_response_invalid";if(!error||strcmp(error,expected))fprintf(stderr,"mode %zu error %s\n",i,error?error:"none");CHECK(error&&!strcmp(error,expected));CHECK(!response&&!bytes);}
    }
    for(int profile=0;profile<2;profile++){
    process.temporal_frames=profile;process.arguments_json="[\"limits\"]";control.cancelled=NULL;control.deadline_monotonic=now()+5;
    CHECK(!sources_inference_process_call(&process,&control,"{}",2,&response,&bytes));CHECK(json_parse(&j,response,bytes));size_t cpu_seconds=0;
    CHECK(json_uint(&j,json_get(&j,0,"cpu_seconds"),&cpu_seconds)&&cpu_seconds==(profile?120u:32u));
#ifdef __linux__
    if(!strstr(argv[1],"sanitize")){size_t address_bytes=0;CHECK(json_uint(&j,json_get(&j,0,"address_bytes"),&address_bytes)&&address_bytes==(profile?8ull:2ull)*1024ull*1024ull*1024ull);}
#endif
    free(response);
    char marker[]="/tmp/samosa-inference-process-pid-XXXXXX";int marker_fd=mkstemp(marker);CHECK(marker_fd>=0);CHECK(!close(marker_fd));
    char arguments[512];snprintf(arguments,sizeof(arguments),"[\"record\",\"%s\"]",marker);process.arguments_json=arguments;
    pid_t requester=fork();CHECK(requester>=0);
    if(!requester){control.deadline_monotonic=now()+10;control.cancelled=NULL;const char *error=sources_inference_process_call(&process,&control,"{}",2,&response,&bytes);free(response);_exit(error?1:0);}
    long provider=0;double expires=now()+5;
    while(!provider&&now()<expires){FILE *file=fopen(marker,"r");CHECK(file);int scanned=fscanf(file,"%ld",&provider);fclose(file);if(scanned!=1)provider=0;struct timespec delay={0,10000000};nanosleep(&delay,NULL);}
    CHECK(provider>0);CHECK(!kill(requester,SIGKILL));int status;CHECK(waitpid(requester,&status,0)==requester&&WIFSIGNALED(status));
    int gone=0;expires=now()+5;
    while(!gone&&now()<expires){gone=kill((pid_t)provider,0)<0&&errno==ESRCH;
#ifdef __linux__
        char path[128],state=0;snprintf(path,sizeof(path),"/proc/%ld/stat",provider);FILE *file=fopen(path,"r");if(file){char line[1024];if(fgets(line,sizeof(line),file)){char *end=strrchr(line,')');if(end&&end[1]==' ')state=end[2];}fclose(file);}if(state=='Z')gone=1;
#endif
        struct timespec delay={0,10000000};nanosleep(&delay,NULL);}
    if(!gone)kill((pid_t)provider,SIGKILL);
    CHECK(gone);CHECK(!unlink(marker));
    }
    free(request);printf("PASS public Inference process transport: %u checks; full window payload, provider error, bounded output, process failure, deadline/cancellation and requester death\n",checks);return 0;
}
