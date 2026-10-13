#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "embedding.h"
#include "ocr.h"
#include "native.h"
#include "watch.h"
#include "search.h"
#include "service.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static int ca_same(const struct stat *a,const struct stat *b){
    if(a->st_dev!=b->st_dev||a->st_ino!=b->st_ino||a->st_size!=b->st_size||a->st_mode!=b->st_mode)return 0;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec==b->st_mtimespec.tv_sec&&a->st_mtimespec.tv_nsec==b->st_mtimespec.tv_nsec&&a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec&&a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
#endif
}
static int host_text(const char *path,char *output,size_t capacity,int private){
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);if(fd<0)return 0;
    struct stat before={0},after={0},named={0};size_t n=0;int bad=fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_size<1||(size_t)before.st_size>capacity;
    if(!bad&&private&&(before.st_uid!=geteuid()||(before.st_mode&0077)))bad=1;
    while(!bad&&n<=capacity){ssize_t got=read(fd,output+n,capacity+1-n);if(got<0&&errno==EINTR)continue;if(got<0){bad=1;break;}if(!got)break;n+=(size_t)got;}
    if(fstat(fd,&after)||lstat(path,&named)||!ca_same(&before,&after)||!ca_same(&before,&named)||n!=(size_t)before.st_size||n>capacity||memchr(output,0,n))bad=1;
    close(fd);if(bad)return 0;output[n]=0;return 1;
}
static int search_config(const char *path,SourcesSearchConfig *c,char storage[10][8193]){
    char input[8193];if(!host_text(path,input,8192,1))return 0;Json j;if(!json_parse(&j,input,strlen(input))||j.tokens[0].type!=JT_OBJECT)return 0;
    const char *names[]={"provider","origin","url_template","headers","body","values","results_path","title_path","url_path","description_path"};
    const char **fields[]={&c->provider,&c->origin,&c->url_template,&c->headers,&c->body,&c->values,&c->results_path,&c->title_path,&c->url_path,&c->description_path};
    *c=(SourcesSearchConfig){0};
    for(int t=1;t<j.tokens[0].next;t=j.tokens[j.tokens[t].next].next){char *name=json_string(&j,t);int index=-1;for(int i=0;i<10;i++)if(name&&!strcmp(name,names[i]))index=i;free(name);if(index<0)return 0;int value=j.tokens[t].next;
        if(index>=3&&index<=5){if(index!=4&&j.tokens[value].type!=JT_OBJECT)return 0;size_t n=j.tokens[value].end-j.tokens[value].begin;memcpy(storage[index],j.text+j.tokens[value].begin,n);storage[index][n]=0;}
        else{char *text=json_string(&j,value);if(!text)return 0;snprintf(storage[index],8193,"%s",text);free(text);}*fields[index]=storage[index];
    }
    return c->provider&&c->origin&&c->url_template&&c->results_path&&c->title_path&&c->url_path&&c->description_path;
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"--sources-native-worker"))return sources_native_worker_main(argc,argv);
    const char *watch_id=NULL;size_t interval=1000,index_interval=1000;int index_run=0;
    int serve=0,auth_given=0;unsigned serve_port=8643;SourcesHttpAuth auth={0};
    SourcesConfig cfg={0};SourcesOcrConfig ocr={0};char semantic_profile[4097],ca_pem[8193],search_values[4097],search_storage[10][8193];SourcesSearchConfig search={0};umask(077);
    char native_self[4096];cfg.native_helper=sources_native_self(native_self,sizeof(native_self),argv[0]);
    SourcesInferenceConfig inference={0};SourcesInferenceProcess process={.sources=&cfg};
    char inference_arguments[8193];
    SourcesInferenceConfig visual_inference={0};SourcesInferenceProcess visual_process={.sources=&cfg};
    SourcesInferenceConfig temporal_inference={0};SourcesInferenceProcess temporal_process={.sources=&cfg,.temporal_frames=1};
    char temporal_arguments[8193]={0};
    char visual_arguments[8193];
    SourcesInferenceConfig summary_inference={0};SourcesInferenceProcess summary_process={.sources=&cfg};
    char summary_arguments[8193];
    SourcesEmbeddingConfig embedding={0};SourcesInferenceProcess embedding_process={.sources=&cfg};
    char embedding_arguments[8193],embedding_config[4097];int embedding_given=0;
    if(!cfg.native_helper)return 2;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--serve"))serve=1;
        else if(!strcmp(argv[i],"--serve-port")&&i+1<argc){char *end;unsigned long value=strtoul(argv[++i],&end,10);if(!*argv[i]||*end||value>65535)return 2;serve_port=(unsigned)value;}
        else if(!strcmp(argv[i],"--serve-auth-file")&&i+1<argc){char body[4097];const char *path=argv[++i];if(auth_given||path[0]!='/'||!host_text(path,body,4096,1)||!sources_http_auth_parse(body,strlen(body),&auth))return 2;auth_given=1;}
        else if(!strcmp(argv[i],"--index-run"))index_run=1;
        else if(!strcmp(argv[i],"--discovery-interval-ms")&&i+1<argc){char *end;unsigned long value=strtoul(argv[++i],&end,10);if(!*argv[i]||*end||value<100||value>60000)return 2;cfg.discovery_interval_ms=(size_t)value;}
        else if(!strcmp(argv[i],"--index-interval-ms")&&i+1<argc){char *end;unsigned long value=strtoul(argv[++i],&end,10);if(!*argv[i]||*end||value<100||value>60000)return 2;index_interval=(size_t)value;}
        else if(!strcmp(argv[i],"--watch-id")&&i+1<argc)watch_id=argv[++i];
        else if(!strcmp(argv[i],"--watch-interval-ms")&&i+1<argc){char *end;unsigned long value=strtoul(argv[++i],&end,10);if(!*argv[i]||*end||value<100||value>60000)return 2;interval=(size_t)value;}
        else if(!strcmp(argv[i],"--inference-executable")&&i+1<argc)process.executable=argv[++i];
        else if(!strcmp(argv[i],"--inference-arguments-file")&&i+1<argc){if(!host_text(argv[++i],inference_arguments,8192,1))return 2;process.arguments_json=inference_arguments;}
        else if(!strcmp(argv[i],"--inference-provider-id")&&i+1<argc)inference.provider_id=argv[++i];
        else if(!strcmp(argv[i],"--inference-engine-sha256")&&i+1<argc)inference.engine_sha256=argv[++i];
        else if(!strcmp(argv[i],"--inference-model-sha256")&&i+1<argc)inference.model_sha256=argv[++i];
        else if(!strcmp(argv[i],"--visual-inference-executable")&&i+1<argc)visual_process.executable=argv[++i];
        else if(!strcmp(argv[i],"--temporal-inference-executable")&&i+1<argc)temporal_process.executable=argv[++i];
        else if(!strcmp(argv[i],"--temporal-inference-arguments-file")&&i+1<argc){if(!host_text(argv[++i],temporal_arguments,8192,1))return 2;temporal_process.arguments_json=temporal_arguments;}
        else if(!strcmp(argv[i],"--temporal-inference-provider-id")&&i+1<argc)temporal_inference.provider_id=argv[++i];
        else if(!strcmp(argv[i],"--temporal-inference-engine-sha256")&&i+1<argc)temporal_inference.engine_sha256=argv[++i];
        else if(!strcmp(argv[i],"--temporal-inference-model-sha256")&&i+1<argc)temporal_inference.model_sha256=argv[++i];
        else if(!strcmp(argv[i],"--visual-inference-arguments-file")&&i+1<argc){if(!host_text(argv[++i],visual_arguments,8192,1))return 2;visual_process.arguments_json=visual_arguments;}
        else if(!strcmp(argv[i],"--visual-inference-provider-id")&&i+1<argc)visual_inference.provider_id=argv[++i];
        else if(!strcmp(argv[i],"--visual-inference-engine-sha256")&&i+1<argc)visual_inference.engine_sha256=argv[++i];
        else if(!strcmp(argv[i],"--visual-inference-model-sha256")&&i+1<argc)visual_inference.model_sha256=argv[++i];
        else if(!strcmp(argv[i],"--embedding-inference-config-file")&&i+1<argc){if(embedding_given||argv[i+1][0]!='/'||!host_text(argv[++i],embedding_config,4096,1)||!sources_embedding_config_parse(embedding_config,strlen(embedding_config),&embedding))return 2;embedding_given=1;}
        else if(!strcmp(argv[i],"--embedding-inference-executable")&&i+1<argc)embedding_process.executable=argv[++i];
        else if(!strcmp(argv[i],"--embedding-inference-arguments-file")&&i+1<argc){if(!host_text(argv[++i],embedding_arguments,8192,1))return 2;embedding_process.arguments_json=embedding_arguments;}
        else if(!strcmp(argv[i],"--summary-inference-executable")&&i+1<argc)summary_process.executable=argv[++i];
        else if(!strcmp(argv[i],"--summary-inference-arguments-file")&&i+1<argc){if(!host_text(argv[++i],summary_arguments,8192,1))return 2;summary_process.arguments_json=summary_arguments;}
        else if(!strcmp(argv[i],"--summary-inference-provider-id")&&i+1<argc)summary_inference.provider_id=argv[++i];
        else if(!strcmp(argv[i],"--summary-inference-engine-sha256")&&i+1<argc)summary_inference.engine_sha256=argv[++i];
        else if(!strcmp(argv[i],"--summary-inference-model-sha256")&&i+1<argc)summary_inference.model_sha256=argv[++i];
        else if(!strcmp(argv[i],"--root") && i+1<argc)cfg.root=argv[++i];
        else if(!strcmp(argv[i],"--state") && i+1<argc)cfg.state=argv[++i];
        else if(!strcmp(argv[i],"--owner") && i+1<argc)cfg.owner=argv[++i];
        else if(!strcmp(argv[i],"--host-id") && i+1<argc)cfg.host_id=argv[++i];
        else if(!strcmp(argv[i],"--pdfium-library") && i+1<argc)cfg.pdfium_library=argv[++i];
        else if(!strcmp(argv[i],"--chutni-library")&&i+1<argc)cfg.chutni_library=argv[++i];
        else if(!strcmp(argv[i],"--chutni-store")&&i+1<argc)cfg.chutni_store=argv[++i];
        else if(!strcmp(argv[i],"--chutni-helper")&&i+1<argc)cfg.chutni_helper=argv[++i];
        else if(!strcmp(argv[i],"--chutni-semantic-profile-file")&&i+1<argc){if(!host_text(argv[++i],semantic_profile,4096,1))return 2;cfg.chutni_semantic_profile=semantic_profile;}
        else if(!strcmp(argv[i],"--chutni-store-id")&&i+1<argc)cfg.chutni_store_id=argv[++i];
        else if(!strcmp(argv[i],"--chutni-publish"))cfg.chutni_publish=1;
        else if(!strcmp(argv[i],"--search-provider")&&i+1<argc){if(sources_search_preset(argv[++i],&search))return 2;cfg.search=&search;}
        else if(!strcmp(argv[i],"--search-config-file")&&i+1<argc){if(!search_config(argv[++i],&search,search_storage))return 2;cfg.search=&search;}
        else if(!strcmp(argv[i],"--search-values-file")&&i+1<argc){if(!host_text(argv[++i],search_values,4096,1))return 2;search.values=search_values;}
        else if(!strcmp(argv[i],"--search-origin")&&i+1<argc)search.origin=argv[++i];
        else if(!strcmp(argv[i],"--search-url")&&i+1<argc)search.url_template=argv[++i];
        else if(!strcmp(argv[i],"--fetch-ca-file")&&i+1<argc){if(!host_text(argv[++i],ca_pem,8192,0))return 2;cfg.fetch_ca_pem=ca_pem;}
        else if(!strcmp(argv[i],"--fetch-origin") && i+1<argc)cfg.fetch_origin=argv[++i];
        else if(!strcmp(argv[i],"--fetch-public-only"))cfg.fetch_public_only=1;
        else if(!strcmp(argv[i],"--fetch-public-web")){cfg.fetch_public_only=1;cfg.fetch_public_web=1;}
        else if(!strcmp(argv[i],"--fetch-offline"))cfg.fetch_offline=1;
        else if(!strcmp(argv[i],"--fetch-helper") && i+1<argc)cfg.fetch_helper=argv[++i];
        else if(!strcmp(argv[i],"--media-helper") && i+1<argc)cfg.media_helper=argv[++i];
        else if(!strcmp(argv[i],"--tesseract-library") && i+1<argc){ocr.tesseract_library=argv[++i];cfg.ocr=&ocr;}
        else if(!strcmp(argv[i],"--leptonica-library") && i+1<argc){ocr.leptonica_library=argv[++i];cfg.ocr=&ocr;}
        else if(!strcmp(argv[i],"--ocr-traineddata") && i+1<argc){ocr.traineddata_file=argv[++i];cfg.ocr=&ocr;}
        else if(!strcmp(argv[i],"--ocr-language") && i+1<argc){ocr.language=argv[++i];cfg.ocr=&ocr;}
        else { fputs("usage: samosa-sources [--root DIRECTORY --state PRIVATE_DIRECTORY]\n",stderr);return 2; }
    }
    if(inference.provider_id||inference.engine_sha256||inference.model_sha256||process.executable||process.arguments_json){
        if(!inference.provider_id||!inference.engine_sha256||!inference.model_sha256||!!process.executable!=!!process.arguments_json)return 2;
        if(process.executable){inference.call=sources_inference_process_call;inference.context=&process;}
        cfg.inference=&inference;
    }
    if(visual_inference.provider_id||visual_inference.engine_sha256||visual_inference.model_sha256||visual_process.executable||visual_process.arguments_json){
        if(!visual_inference.provider_id||!visual_inference.engine_sha256||!visual_inference.model_sha256||!!visual_process.executable!=!!visual_process.arguments_json)return 2;
        if(visual_process.executable){visual_inference.call=sources_inference_process_call;visual_inference.context=&visual_process;}
        cfg.visual_inference=&visual_inference;
    }
    if(temporal_inference.provider_id||temporal_inference.engine_sha256||temporal_inference.model_sha256||temporal_process.executable||temporal_process.arguments_json){
        if(!temporal_inference.provider_id||!temporal_inference.engine_sha256||!temporal_inference.model_sha256||!!temporal_process.executable!=!!temporal_process.arguments_json)return 2;
        if(temporal_process.executable){temporal_inference.call=sources_inference_process_call;temporal_inference.context=&temporal_process;}
        cfg.temporal_inference=&temporal_inference;
    }
    if(summary_inference.provider_id||summary_inference.engine_sha256||summary_inference.model_sha256||summary_process.executable||summary_process.arguments_json){
        if(!summary_inference.provider_id||!summary_inference.engine_sha256||!summary_inference.model_sha256||!!summary_process.executable!=!!summary_process.arguments_json)return 2;
        if(summary_process.executable){summary_inference.call=sources_inference_process_call;summary_inference.context=&summary_process;}
        cfg.summary_inference=&summary_inference;
    }
    if(embedding_given||embedding_process.executable||embedding_process.arguments_json){
        if(!embedding_given||!!embedding_process.executable!=!!embedding_process.arguments_json)return 2;
        cfg.embedding=&embedding;char binding[65];if(sources_embedding_identity(&cfg,binding))return 2;
        if(embedding_process.executable){embedding.inference.call=sources_inference_process_call;embedding.inference.context=&embedding_process;}
    }
    if(index_run&&watch_id)return 2;
    if(serve){if(!auth_given||index_run||watch_id||cfg.owner)return 2;int rc=sources_service_run(&cfg,&auth,serve_port,index_interval,stdout);sources_embedding_config_free(&embedding);return rc;}
    if(auth_given)return 2;
    if(index_run)return sources_queue_run(&cfg,index_interval,stdout);
    if(watch_id)return sources_watch_run(&cfg,watch_id,interval,stdout);
    char request[SOURCES_REQUEST_MAX+1];size_t n=fread(request,1,sizeof(request),stdin);
    if(ferror(stdin))return 2;
    int rc=sources_dispatch(&cfg,request,n,stdout);sources_embedding_config_free(&embedding);return rc;
}
