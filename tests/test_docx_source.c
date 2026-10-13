#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include "sha256.h"
#include <fcntl.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL DOCX source %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#include "docx_fixture.h"
static char *call(const SourcesConfig *cfg,const char *req,int expected){char *out=NULL;size_t n;FILE *f=open_memstream(&out,&n);CHECK(f);int rc=sources_dispatch(cfg,req,strlen(req),f);CHECK(!fclose(f));if(rc!=expected)fprintf(stderr,"response: %s\n",out);CHECK(rc==expected);Json j;CHECK(json_parse(&j,out,n));return out;}
static char *field(const char *out,const char *name){Json j;CHECK(json_parse(&j,out,strlen(out)));char *value=json_string(&j,json_get(&j,0,name));CHECK(value);return value;}
static void error(const SourcesConfig *cfg,const char *req,const char *code){char *out=call(cfg,req,1);Json j;CHECK(json_parse(&j,out,strlen(out)));char *v=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));if(!v||strcmp(v,code))fprintf(stderr,"error response: %s\n",out);CHECK(v&&!strcmp(v,code));free(v);free(out);}
static unsigned caches(const char *state){char path[600];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;sqlite3_stmt *s=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_prepare_v2(db,"SELECT count(*) FROM evidence_cache",-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_ROW);unsigned n=(unsigned)sqlite3_column_int(s,0);CHECK(!sqlite3_finalize(s));CHECK(!sqlite3_close(db));return n;}
static void refusal(const SourcesConfig *cfg,const char *req,const char *code){unsigned before=caches(cfg->state);error(cfg,req,code);CHECK(caches(cfg->state)==before);}
static void write_file(const char *root,const void *bytes,size_t n){char path[512];snprintf(path,sizeof(path),"%s/file.docx",root);int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);CHECK(write(fd,bytes,n)==(ssize_t)n);CHECK(!close(fd));}
static char *register_zip(const SourcesConfig *cfg,const char *xml,unsigned method){size_t n;unsigned char *zip=docx_package(xml,NULL,method,&n);write_file(cfg->root,zip,n);char digest[65];hash_bytes(zip,n,digest);free(zip);char *out=call(cfg,"{\"contract_version\":1,\"op\":\"register_docx\",\"path\":\"file.docx\"}",0);char *id=field(out,"source_id");CHECK(!strcmp(id,digest));CHECK(strstr(out,"wordprocessingml.document"));free(out);return id;}
static void request(char req[1024],const char *id,unsigned start,unsigned end){snprintf(req,1024,"{\"contract_version\":1,\"op\":\"read_docx\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":%u,\"end_byte\":%u}",id,id,start,end);}
typedef struct {unsigned count;int expire;} Probe;
static int after_parse(void *context){Probe *p=context;if(++p->count!=4)return 0;if(p->expire){struct timespec t={1,0};nanosleep(&t,NULL);return 0;}return 1;}
int main(void){char root[]="/tmp/samosa-docx-source.XXXXXX";CHECK(mkdtemp(root));char state[512];snprintf(state,sizeof(state),"%s/state",root);SourcesConfig cfg={.root=root,.state=state,.owner="alice"};const char *xml="<w:document xmlns:w='x'><w:body><w:p><w:r><w:t>Code &amp; café 7319</w:t></w:r><w:del><w:r><w:t>POISON</w:t></w:r></w:del></w:p></w:body></w:document>";char req[1024];
    char *id=register_zip(&cfg,xml,0),*deflated=register_zip(&cfg,xml,8);CHECK(strcmp(id,deflated));
    for(unsigned i=0;i<2;i++){request(req,i?deflated:id,0,17);char *out=call(&cfg,req,0),*text=field(out,"text");CHECK(!strcmp(text,"Code & café 7319"));CHECK(!strstr(out,"POISON")&&strstr(out,"extracted_utf8")&&strstr(out,"ooxml_readable_text_only")&&strstr(out,"docx-readable-v1"));char *again=call(&cfg,req,0);CHECK(!strcmp(out,again));free(out);free(text);free(again);}
    request(req,id,0,17);SourcesConfig other=cfg;other.owner="other";error(&other,req,"unknown_source");write_file(root,"changed",7);char *out=call(&cfg,req,0);CHECK(strstr(out,"7319"));free(out);request(req,id,11,12);refusal(&cfg,req,"utf8_boundary");request(req,id,0,99);refusal(&cfg,req,"invalid_range");
    snprintf(req,sizeof(req),"{\"contract_version\":1,\"op\":\"read_docx\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":4,\"operation_id\":\"docx-read\",\"parent_operation_id\":\"parent\",\"timeout_ms\":1000}",id,id);out=call(&cfg,req,0);SourcesConfig offline=cfg;offline.root=NULL;char *again=call(&offline,req,0);CHECK(!strcmp(out,again));free(out);free(again);
    Probe cancel={0};SourcesConfig controlled=cfg;controlled.cancelled=after_parse;controlled.cancellation=&cancel;request(req,id,5,6);refusal(&controlled,req,"cancelled");CHECK(cancel.count==4);out=call(&cfg,req,0);free(out);
    struct timespec received;CHECK(!clock_gettime(CLOCK_MONOTONIC,&received));Probe expired={.expire=1};controlled.cancellation=&expired;controlled.deadline_monotonic=(double)received.tv_sec+(double)received.tv_nsec/1e9+0.5;request(req,id,7,10);refusal(&controlled,req,"deadline_exceeded");CHECK(expired.count==4);out=call(&cfg,req,0);free(out);
    char *bad=register_zip(&cfg,"<!DOCTYPE w:document><w:document/>",0);request(req,bad,0,0);refusal(&cfg,req,"docx_xml_doctype_unsupported");free(bad);
    char *large=malloc(70100);CHECK(large);const char *prefix="<w:document><w:t>",*suffix="</w:t></w:document>";size_t lead=strlen(prefix);memcpy(large,prefix,lead);memset(large+lead,'A',70000);strcpy(large+lead+70000,suffix);bad=register_zip(&cfg,large,0);free(large);request(req,bad,0,4);refusal(&cfg,req,"docx_text_output_limit");free(bad);
    char path[600];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;sqlite3_stmt *s=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_prepare_v2(db,"UPDATE snapshots SET bytes=? WHERE id=?",-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_bind_blob(s,1,"bad",3,SQLITE_STATIC)==SQLITE_OK);CHECK(sqlite3_bind_text(s,2,id,-1,SQLITE_STATIC)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_DONE);CHECK(!sqlite3_finalize(s));CHECK(!sqlite3_close(db));request(req,id,0,17);refusal(&cfg,req,"corrupt_snapshot");free(id);free(deflated);printf("PASS DOCX public source: %u checks; stored/Deflate, original identity, owner, ranges, cache, durable retry, active cancel/deadline and corrupt snapshot\n",checks);return 0;
}
