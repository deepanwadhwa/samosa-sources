#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "native.h"
#include "json.h"
#include "pdf_fixture.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static char *run(const SourcesConfig *cfg,const char *req,int expected) {
    char *out=NULL;size_t n;FILE *stream=open_memstream(&out,&n);CHECK(stream);
    CHECK(sources_dispatch(cfg,req,strlen(req),stream)==expected);CHECK(!fclose(stream));Json j;CHECK(json_parse(&j,out,n));return out;
}
static void request(char *out,size_t n,const char *id,size_t lo,size_t hi) {
    snprintf(out,n,"{\"contract_version\":1,\"op\":\"read_pdf\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_page\":%zu,\"end_page\":%zu}",id,id,lo,hi);
}
static int spy_count(const char *path,const char *kind,int page) {
    FILE *file=fopen(path,"r");if(!file)return 0;int count=0,number;char name[16];
    while(fscanf(file,"%15s %d",name,&number)==2)if(!strcmp(name,kind)&&(page<0||number==page))count++;
    fclose(file);return count;
}
typedef struct {const char *path;int page,before;} CancelAtPage;
static int cancel_at_page(void *opaque){CancelAtPage *c=opaque;return spy_count(c->path,"page",c->page)>c->before;}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"--sources-native-worker"))return sources_native_worker_main(argc,argv);
    CHECK(argc==3);CHECK(setenv("SAMOSA_TEST_REAL_PDFIUM",argv[1],1)==0);
    char fixture[]="/tmp/samosa-native-pdf-XXXXXX";CHECK(mkdtemp(fixture));char root[512],state[512],path[1024],log[512],req[512];
    snprintf(root,sizeof(root),"%s/root",fixture);snprintf(state,sizeof(state),"%s/state",fixture);snprintf(log,sizeof(log),"%s/spy",fixture);
    CHECK(mkdir(root,0700)==0);CHECK(setenv("SAMOSA_TEST_PDF_SPY_LOG",log,1)==0);
    snprintf(path,sizeof(path),"%s/fixture.pdf",root);CHECK(pdf_fixture(path,100));
    char native_self[4096];SourcesConfig cfg={.root=root,.state=state,.pdfium_library=argv[2],.native_helper=sources_native_self(native_self,sizeof(native_self),argv[0])};CHECK(cfg.native_helper);
    char *out=run(&cfg,"{\"contract_version\":1,\"op\":\"register_pdf\",\"path\":\"fixture.pdf\"}",0);Json j;CHECK(json_parse(&j,out,strlen(out)));char *id=json_string(&j,json_get(&j,0,"source_id"));CHECK(id);free(out);
    request(req,sizeof(req),id,97,97);out=run(&cfg,req,0);CHECK(strstr(out,"7319"));CHECK(strstr(out,"\"page\":97"));CHECK(strstr(out,"\"total_pages\":100"));CHECK(strstr(out,"\"status\":\"partial\""));CHECK(strstr(out,"text_layer_only"));CHECK(!strstr(out,"absolute page 1."));free(out);
    CHECK(spy_count(log,"page",-1)==1);CHECK(spy_count(log,"page",97)==1);CHECK(spy_count(log,"text",-1)==1);
    request(req,sizeof(req),id,1,5);out=run(&cfg,req,1);CHECK(strstr(out,"invalid_range"));free(out);CHECK(spy_count(log,"page",-1)==1);
    request(req,sizeof(req),id,101,101);out=run(&cfg,req,1);CHECK(strstr(out,"invalid_range"));free(out);CHECK(spy_count(log,"page",-1)==1);
    request(req,sizeof(req),id,0,1);out=run(&cfg,req,1);CHECK(strstr(out,"invalid_range"));free(out);CHECK(spy_count(log,"page",-1)==1);
    request(req,sizeof(req),id,96,98);out=run(&cfg,req,0);CHECK(strstr(out,"7319"));CHECK(strstr(out,"\"page\":96"));CHECK(strstr(out,"\"page\":98"));free(out);CHECK(spy_count(log,"page",-1)==4);CHECK(spy_count(log,"text",-1)==4);
    request(req,sizeof(req),id,2,2);out=run(&cfg,req,0);CHECK(strstr(out,"empty_text_layer"));CHECK(strstr(out,"text_layer_only"));free(out);
    FILE *changed=fopen(path,"w");CHECK(changed);fputs("%PDF-not-a-valid-document",changed);fclose(changed);
    request(req,sizeof(req),id,97,97);out=run(&cfg,req,0);CHECK(strstr(out,"7319"));free(out); /* Retained bytes, no live re-read. */
    cfg.pdfium_library="/not-installed/pdfium";out=run(&cfg,req,1);CHECK(strstr(out,"pdf_unavailable"));free(out);
    cfg.pdfium_library=argv[2];out=run(&cfg,"{\"contract_version\":1,\"op\":\"register_pdf\",\"path\":\"fixture.pdf\"}",0);CHECK(json_parse(&j,out,strlen(out)));char *badid=json_string(&j,json_get(&j,0,"source_id"));free(out);
    request(req,sizeof(req),badid,1,1);out=run(&cfg,req,1);CHECK(strstr(out,"pdf_malformed"));free(out);free(badid);
    /* Artifact fingerprint is public provenance and evidence identity. A proxy
     * and the actual extractor must never share the same qualified cache key. */
    request(req,sizeof(req),id,97,97);cfg.pdfium_library=argv[2];out=run(&cfg,req,0);
    CHECK(json_parse(&j,out,strlen(out)));char *first_evidence=json_string(&j,json_get(&j,0,"evidence_id"));int provenance=json_get(&j,0,"provenance");char *first_fingerprint=json_string(&j,json_get(&j,provenance,"extractor_fingerprint"));CHECK(first_evidence&&first_fingerprint&&strlen(first_fingerprint)==64);
    int cached_page_loads=spy_count(log,"page",-1);char *repeated=run(&cfg,req,0);CHECK(!strcmp(out,repeated));CHECK(spy_count(log,"page",-1)==cached_page_loads);free(out);free(repeated);
    cfg.pdfium_library=argv[1];out=run(&cfg,req,0);CHECK(strstr(out,"7319"));CHECK(json_parse(&j,out,strlen(out)));char *other_evidence=json_string(&j,json_get(&j,0,"evidence_id"));provenance=json_get(&j,0,"provenance");char *other_fingerprint=json_string(&j,json_get(&j,provenance,"extractor_fingerprint"));CHECK(other_evidence&&other_fingerprint);CHECK(strcmp(first_fingerprint,other_fingerprint));CHECK(strcmp(first_evidence,other_evidence));free(first_evidence);free(first_fingerprint);free(other_evidence);free(other_fingerprint);free(out);
    request(req,sizeof(req),id,95,95); /* uncached selection for mutation fixture */
    char moved[1024];snprintf(path,sizeof(path),"%s/reader-copy",root);snprintf(moved,sizeof(moved),"%s/reader-moved",root);
    FILE *original=fopen(argv[2],"rb"),*copy=fopen(path,"wb");CHECK(original&&copy);char binary[65536];size_t bytes;
    while((bytes=fread(binary,1,sizeof(binary),original))>0)CHECK(fwrite(binary,1,bytes,copy)==bytes);
    CHECK(!ferror(original));CHECK(fclose(original)==0);CHECK(fclose(copy)==0);
    CHECK(setenv("SAMOSA_TEST_MUTATE_READER",path,1)==0);CHECK(setenv("SAMOSA_TEST_MUTATE_DESTINATION",moved,1)==0);cfg.pdfium_library=path;
    out=run(&cfg,req,1);CHECK(strstr(out,"pdf_adapter_changed"));CHECK(!strstr(out,"evidence_id"));free(out);CHECK(unsetenv("SAMOSA_TEST_MUTATE_READER")==0);CHECK(unsetenv("SAMOSA_TEST_MUTATE_DESTINATION")==0);
    CHECK(rename(moved,path)==0);out=run(&cfg,req,0);CHECK(strstr(out,"\"page\":95"));free(out); /* failed changed extraction was never cached */
    int loads_before_limit=spy_count(log,"page",-1);cfg.pdfium_library=argv[2];cfg.cache_limit=1;request(req,sizeof(req),id,99,99);out=run(&cfg,req,1);CHECK(strstr(out,"cache_limit"));free(out);CHECK(spy_count(log,"page",-1)==loads_before_limit);cfg.cache_limit=0;

    snprintf(path,sizeof(path),"%s/reader-fifo",root);CHECK(mkfifo(path,0600)==0);cfg.pdfium_library=path;out=run(&cfg,req,1);CHECK(strstr(out,"pdf_adapter_invalid"));free(out);
    snprintf(path,sizeof(path),"%s/reader-large",root);FILE *large=fopen(path,"w");CHECK(large);CHECK(ftruncate(fileno(large),128*1048576+1)==0);CHECK(fclose(large)==0);cfg.pdfium_library=path;out=run(&cfg,req,1);CHECK(strstr(out,"pdf_adapter_invalid"));free(out);
    cfg.owner="alice";cfg.pdfium_library=argv[1];snprintf(path,sizeof(path),"%s/alice.pdf",root);CHECK(pdf_fixture(path,100));out=run(&cfg,"{\"contract_version\":1,\"op\":\"register_pdf\",\"path\":\"alice.pdf\"}",0);free(out);request(req,sizeof(req),id,97,97);char *alice_answer=run(&cfg,req,0);CHECK(strstr(alice_answer,"7319"));CHECK(strstr(alice_answer,"alice.pdf"));CHECK(!strstr(alice_answer,"fixture.pdf"));cfg.owner="bob";out=run(&cfg,req,1);CHECK(strstr(out,"unknown_source"));CHECK(!strstr(out,"alice.pdf"));free(out);snprintf(path,sizeof(path),"%s/bob.pdf",root);CHECK(pdf_fixture(path,100));out=run(&cfg,"{\"contract_version\":1,\"op\":\"register_pdf\",\"path\":\"bob.pdf\"}",0);free(out);out=run(&cfg,req,0);CHECK(strstr(out,"7319"));CHECK(strstr(out,"bob.pdf"));CHECK(!strstr(out,"alice.pdf"));free(out);cfg.owner="alice";out=run(&cfg,req,0);CHECK(!strcmp(out,alice_answer));free(out);free(alice_answer);
    /* A real native extraction that returns after its budget cannot publish a
     * fresh cache entry. The next healthy read must load the page again. */
    cfg.pdfium_library=argv[2];int loads_before_deadline=spy_count(log,"page",88);
    CHECK(!setenv("SAMOSA_TEST_PDF_PAGE_DELAY_MS","400",1));
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"read_pdf\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_page\":88,\"end_page\":88,\"operation_id\":\"delayed-pdf\",\"parent_operation_id\":\"root-run\",\"timeout_ms\":200}",id,id);
    out=run(&cfg,req,1);CHECK(strstr(out,"deadline_exceeded")&&strstr(out,"delayed-pdf")&&!strstr(out,"evidence_id"));free(out);CHECK(spy_count(log,"page",88)==loads_before_deadline+1);CHECK(!unsetenv("SAMOSA_TEST_PDF_PAGE_DELAY_MS"));
    request(req,sizeof(req),id,88,88);out=run(&cfg,req,0);CHECK(strstr(out,"\"page\":88"));CHECK(spy_count(log,"page",88)==loads_before_deadline+2);repeated=run(&cfg,req,0);CHECK(!strcmp(out,repeated));CHECK(spy_count(log,"page",88)==loads_before_deadline+2);free(out);free(repeated);
    /* A cancellation observed after the first native page prevents later pages
     * and never admits partial evidence; the healthy request extracts afresh. */
    CancelAtPage cancellation={log,80,spy_count(log,"page",80)};cfg.cancelled=cancel_at_page;cfg.cancellation=&cancellation;
    int later=spy_count(log,"page",81);
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"read_pdf\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_page\":80,\"end_page\":83,\"operation_id\":\"cancel-pages\",\"parent_operation_id\":\"root-run\"}",id,id);
    out=run(&cfg,req,1);CHECK(strstr(out,"\"code\":\"cancelled\"")&&!strstr(out,"evidence_id"));CHECK(spy_count(log,"page",80)==cancellation.before+1);CHECK(spy_count(log,"page",81)==later);free(out);cfg.cancelled=NULL;cfg.cancellation=NULL;
    out=run(&cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"cancel-pages\"}",0);CHECK(strstr(out,"\"status\":\"cancelled\"")&&strstr(out,"root-run"));free(out);
    cfg.pdfium_library=NULL;out=run(&cfg,req,1);CHECK(strstr(out,"cancelled"));CHECK(spy_count(log,"page",80)==cancellation.before+1);free(out);cfg.pdfium_library=argv[2];request(req,sizeof(req),id,80,83);out=run(&cfg,req,0);CHECK(strstr(out,"\"page\":83"));CHECK(spy_count(log,"page",80)==cancellation.before+2);CHECK(spy_count(log,"page",81)==later+1);free(out);
    char upload_state[512],upload_path[512];snprintf(upload_state,sizeof(upload_state),"%s/upload-state",fixture);snprintf(upload_path,sizeof(upload_path),"%s/upload.pdf",fixture);CHECK(pdf_fixture(upload_path,100));FILE *input=fopen(upload_path,"rb");CHECK(input&&!fseek(input,0,SEEK_END));long length=ftell(input);CHECK(length>0&&!fseek(input,0,SEEK_SET));unsigned char *upload_bytes=malloc((size_t)length);CHECK(upload_bytes&&fread(upload_bytes,1,(size_t)length,input)==(size_t)length&&!fclose(input));
    SourcesConfig upload_cfg={.state=upload_state,.owner="alice",.pdfium_library=argv[1],.native_helper=cfg.native_helper};size_t response_length;FILE *response=open_memstream(&out,&response_length);CHECK(response);CHECK(!sources_upload(&upload_cfg,SOURCES_UPLOAD_PDF,upload_bytes,(size_t)length,response)&&!fclose(response));free(upload_bytes);CHECK(!unlink(upload_path));CHECK(json_parse(&j,out,strlen(out)));char *upload_id=json_string(&j,json_get(&j,0,"source_id"));CHECK(upload_id);free(out);
    request(req,sizeof(req),upload_id,97,97);out=run(&upload_cfg,req,0);CHECK(strstr(out,"7319")&&strstr(out,"\"page\":97")&&strstr(out,"\"total_pages\":100"));char *cached=run(&upload_cfg,req,0);CHECK(!strcmp(out,cached));free(out);free(cached);upload_cfg.owner="bob";out=run(&upload_cfg,req,1);CHECK(strstr(out,"unknown_source"));free(out);free(upload_id);
    printf("PASS native PDFium: %u checks; real page97/100 sentinel7319; selected pages; deadline no-cache and survivor\n",checks);
    char *cleanup[]={"find",fixture,"-depth","-delete",NULL};pid_t child=fork();CHECK(child>=0);if(!child){execvp(cleanup[0],cleanup);_exit(127);}int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);free(id);return 0;
}
