#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "summary.h"
#include "json.h"
#include "sha256.h"
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *call(const SourcesConfig *cfg,const char *request,const char *error){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);int rc=sources_dispatch(cfg,request,strlen(request),out);CHECK(!fclose(out));if(rc!=(error?1:0))fprintf(stderr,"request %s response %s\n",request,body);CHECK(rc==(error?1:0));if(error){Json j;CHECK(json_parse(&j,body,n));char *code=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));if(!code||strcmp(code,error))fprintf(stderr,"request %s response %s\n",request,body);CHECK(code&&!strcmp(code,error));free(code);}return body;}
static char *field(const char *body,const char *name){Json j;CHECK(json_parse(&j,body,strlen(body)));char *text=json_string(&j,json_get(&j,0,name));CHECK(text);return text;}
#include <fcntl.h>
static void file_sha(const char *path,char out[65]){int fd=open(path,O_RDONLY);CHECK(fd>=0);Sha256 h;sha256_init(&h);unsigned char bytes[8192];ssize_t n;while((n=read(fd,bytes,sizeof(bytes)))>0)sha256_update(&h,bytes,(size_t)n);CHECK(!n);CHECK(!close(fd));unsigned char digest[32];sha256_final(&h,digest);sha256_hex(digest,out);}
static int remove_file(const char *path,const struct stat *s,int type,struct FTW *walk){(void)s;(void)type;(void)walk;return remove(path);}
int main(int argc,char **argv){
    CHECK(argc==5);for(int i=1;i<argc;i++)CHECK(argv[i][0]=='/');
    char root[]="/tmp/samosa-source-summary-real-XXXXXX";CHECK(mkdtemp(root));char state[512],path[512],admission[512];snprintf(state,sizeof(state),"%s/state",root);snprintf(path,sizeof(path),"%s/article.txt",root);snprintf(admission,sizeof(admission),"%s/inference-admission",root);
    const char *input="The city council approved a new public library. Construction begins in March.";
    FILE *f=fopen(path,"w");CHECK(f);CHECK(fwrite(input,1,strlen(input),f)==strlen(input));CHECK(!fclose(f));
    char engine_sha[65],model_sha[65];file_sha(argv[3],engine_sha);file_sha(argv[4],model_sha);
    CHECK(!strcmp(model_sha,"30c73e0b2ebd4f65538d87f5157aa7193261e8ff362e441df86c6941f3e9265c"));
    char *args=NULL;size_t args_bytes=0;f=open_memstream(&args,&args_bytes);CHECK(f);
    const char *items[]={"--summary-cli",argv[3],"--summary-model",argv[4],"--summary-cli-sha256",engine_sha,"--summary-model-sha256",model_sha,"--admission-state",admission};fputc('[',f);for(size_t i=0;i<sizeof(items)/sizeof(items[0]);i++){if(i)fputc(',',f);json_quote(f,items[i],strlen(items[i]));}fputc(']',f);CHECK(!fclose(f));
    SourcesConfig cfg={.root=root,.state=state,.owner="actual-summary",.native_helper=argv[1]};
    SourcesInferenceProcess process={.sources=&cfg,.executable=argv[2],.arguments_json=args};
    SourcesInferenceConfig inference={.provider_id="native-t5-summary-v1",.engine_sha256=engine_sha,.model_sha256=model_sha,.call=sources_inference_process_call,.context=&process};cfg.summary_inference=&inference;
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_text\",\"path\":\"article.txt\"}",NULL),*source=field(body,"source_id");free(body);
    char request[1024];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":%zu}",source,source,strlen(input));body=call(&cfg,request,NULL);char *evidence=field(body,"evidence_id");free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"summarize_evidence\",\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"}",source,source,evidence);
    body=call(&cfg,request,NULL);char *text=field(body,"text");CHECK(strstr(text,"library")&&strstr(text,"March"));free(text);CHECK(strstr(body,engine_sha)&&strstr(body,model_sha)&&strstr(body,"opening_sample_and_generated_summary_uncertain"));
    CHECK(!sources_summary_capture_validate(&cfg,body,strlen(body)));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"summarize_evidence_opening\",\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"}",source,source,evidence);
    body=call(&cfg,request,NULL);CHECK(!sources_summary_capture_validate(&cfg,body,strlen(body)));CHECK(strstr(body,"native_token_chunks")&&strstr(body,"opening-map-reduce-v1"));
    char *indexed=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"real-enrichment-index\"}",NULL),*job=field(indexed,"job_id");free(indexed);
    char index_request[512];snprintf(index_request,sizeof(index_request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"real-enrichment-capture\",\"max_items\":4}",job);indexed=call(&cfg,index_request,NULL);free(indexed);
    indexed=call(&cfg,"{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}",NULL);CHECK(strstr(indexed,"\"phase\":\"captured\""));free(indexed);
    snprintf(index_request,sizeof(index_request),"{\"contract_version\":1,\"op\":\"index_enrichment_status\",\"job_id\":\"%s\"}",job);indexed=call(&cfg,index_request,NULL);CHECK(strstr(indexed,"library")&&strstr(indexed,"March")&&strstr(indexed,"completed")&&strstr(indexed,model_sha));free(indexed);free(job);
    CHECK(!unlink(path));inference.call=NULL;char *cached=call(&cfg,request,NULL);CHECK(!strcmp(body,cached));free(cached);free(body);free(source);free(evidence);free(args);
    /* Cleanup runs only after the transport has reaped its owned workers. */
    CHECK(!nftw(root,remove_file,32,FTW_DEPTH|FTW_PHYS));
    printf("PASS actual Sources -> public Inference -> native T5 summary: %u checks; provenance and origin/model-free cache\n",checks);return 0;
}
