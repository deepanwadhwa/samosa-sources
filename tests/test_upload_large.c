#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include "sha256.h"
#include <assert.h>
#include <sqlite3.h>
#include <unistd.h>
static char *call(SourcesConfig *cfg,const char *request){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);int rc=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));if(rc)fprintf(stderr,"large upload: %s\n",body);assert(!rc);return body;}
static void encode(const unsigned char *bytes,size_t n,char *out){const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";size_t k=0;for(size_t i=0;i<n;i+=3){unsigned value=(unsigned)bytes[i]<<16;if(i+1<n)value|=(unsigned)bytes[i+1]<<8;if(i+2<n)value|=bytes[i+2];out[k++]=alphabet[value>>18];out[k++]=alphabet[(value>>12)&63];out[k++]=i+1<n?alphabet[(value>>6)&63]:'=';out[k++]=i+2<n?alphabet[value&63]:'=';}out[k]=0;}
int main(void){
 char empty[65];hash_bytes("",0,empty);assert(!strcmp(empty,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
 char state[]="/tmp/samosa-upload-large-XXXXXX";assert(mkdtemp(state));SourcesConfig cfg={.state=state,.owner="large",.snapshot_limit=1,.byte_limit=SOURCES_INPUT_MAX};
 char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"large\",\"format\":\"text\",\"bytes\":67108864}");Json j;assert(json_parse(&j,body,strlen(body)));char *id=json_string(&j,json_get(&j,0,"upload_id"));assert(id);free(body);
 unsigned char chunk[8192];memset(chunk,'A',sizeof(chunk));char encoded[10925],request[12000];Sha256 hash;sha256_init(&hash);
 for(size_t offset=0;offset<SOURCES_INPUT_MAX;offset+=sizeof(chunk)){
  if(offset+sizeof(chunk)==SOURCES_INPUT_MAX)memcpy(chunk+sizeof(chunk)-5,"tail!",5);
  sha256_update(&hash,chunk,sizeof(chunk));encode(chunk,sizeof(chunk),encoded);
  snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":%zu,\"data\":\"%s\"}",id,offset,encoded);assert(strlen(request)<SOURCES_REQUEST_MAX);body=call(&cfg,request);size_t received=0;assert(json_parse(&j,body,strlen(body))&&json_uint(&j,json_get(&j,0,"received_bytes"),&received)&&received==offset+sizeof(chunk));free(body);
  if((offset+sizeof(chunk))%1048576==0){fprintf(stderr,"staged %zu MiB\n",(offset+sizeof(chunk))/1048576);fflush(stderr);}
 }
 unsigned char digest[32];char expected[65];sha256_final(&hash,digest);sha256_hex(digest,expected);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);body=call(&cfg,request);assert(json_parse(&j,body,strlen(body)));char *source=json_string(&j,json_get(&j,0,"source_id"));assert(source&&!strcmp(source,expected));char *retry=call(&cfg,request);assert(!strcmp(body,retry));free(body);free(retry);free(id);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":67108859,\"end_byte\":67108864}",source,source);body=call(&cfg,request);assert(strstr(body,"tail!"));free(body);free(source);
 char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);sqlite3_stmt *stmt=NULL;assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM upload_chunks),(SELECT count(*) FROM snapshots),(SELECT sum(length(bytes)) FROM snapshots),(SELECT count(*) FROM upload_receipts)",-1,&stmt,NULL)==SQLITE_OK);assert(sqlite3_step(stmt)==SQLITE_ROW);assert(sqlite3_column_int64(stmt,0)==0&&sqlite3_column_int64(stmt,1)==1&&sqlite3_column_int64(stmt,2)==SOURCES_INPUT_MAX&&sqlite3_column_int64(stmt,3)==1);sqlite3_finalize(stmt);assert(sqlite3_close(db)==SQLITE_OK);
 puts("PASS 64MiB staged upload: public8192-byte appends, independent SHA256, publication/retry, tail evidence and staging cleanup");return 0;
}
