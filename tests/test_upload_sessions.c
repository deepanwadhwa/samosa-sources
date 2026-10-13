#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include "sha256.h"
#include <assert.h>
#include <unistd.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
static char *call(SourcesConfig *cfg,const char *request,int expected){char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);assert(out);int result=sources_dispatch(cfg,request,strlen(request),out);assert(!fclose(out));if(result!=expected)fprintf(stderr,"session: %s\n",body);assert(result==expected);return body;}
typedef struct {SourcesConfig cfg;unsigned calls;int deadline,fired;} PublicControl;
static int public_control(void *context){PublicControl *control=context;if(++control->calls==6){control->fired=1;
 if(control->deadline){struct timespec delay={1,100000000};while(nanosleep(&delay,&delay)){assert(errno==EINTR);}}
 else{char *body=call(&control->cfg,"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"cancel-upload\"}",0);assert(strstr(body,"\"cancel_requested\":true"));free(body);}}
 return 0;}
typedef struct {unsigned calls;int crash;} PublicationControl;
static int publication_control(void *context){PublicationControl *control=context;control->calls++;if(control->calls==6){if(control->crash){assert(!kill(getpid(),SIGKILL));_exit(99);}return 1;}return 0;}
static int paused_control(void *context){unsigned *calls=context;if(++*calls==6){assert(write(STDOUT_FILENO,"R",1)==1);char go;assert(read(STDIN_FILENO,&go,1)==1&&go=='x');}return 0;}
typedef struct {pid_t pid;int output,gate,input;} Process;
static Process launch_mode(const char *executable,const char *state,const char *request,const char *mode){
 int output[2],gate[2],input[2]={-1,-1};int paused=!strcmp(mode,"--paused");assert(!pipe(output)&&!pipe(gate));if(paused)assert(!pipe(input));pid_t pid=fork();assert(pid>=0);
 if(!pid){if(paused){close(input[1]);assert(dup2(input[0],STDIN_FILENO)==STDIN_FILENO);close(input[0]);}close(output[0]);close(gate[1]);char go;assert(read(gate[0],&go,1)==1);close(gate[0]);assert(dup2(output[1],STDOUT_FILENO)==STDOUT_FILENO);close(output[1]);execl(executable,executable,mode,state,request,(char*)NULL);_exit(127);}
 close(output[1]);close(gate[0]);if(paused)close(input[0]);return (Process){pid,output[0],gate[1],input[1]};
}
static Process launch(const char *executable,const char *state,const char *request){return launch_mode(executable,state,request,"--request");}
static void start(Process p){assert(write(p.gate,"x",1)==1);close(p.gate);}
static char *collect_result(Process p,int expected){char *body=calloc(1,4096);assert(body);size_t n=0;ssize_t got;while((got=read(p.output,body+n,4095-n))>0){n+=(size_t)got;assert(n<4095);}assert(got==0);close(p.output);int status;assert(waitpid(p.pid,&status,0)==p.pid&&WIFEXITED(status)&&WEXITSTATUS(status)==expected);return body;}
static char *collect(Process p){return collect_result(p,0);}
static char *process_call(const char *executable,const char *state,const char *request){Process p=launch(executable,state,request);start(p);return collect(p);}
static void committed_death(void *context,int pid,const char *event,SourcesConfig *cfg){
 (void)context;(void)pid;(void)cfg;
 if(!strcmp(event,"upload_committed")){assert(!kill(getpid(),SIGKILL));_exit(99);}
}
static void postcommit_recovery(const char *executable){
 char state[]="/tmp/samosa-upload-committed-XXXXXX";assert(mkdtemp(state));
 char *body=process_call(executable,state,"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"committed\",\"format\":\"text\",\"bytes\":5}");
 Json j;assert(json_parse(&j,body,strlen(body)));char *id=json_string(&j,json_get(&j,0,"upload_id"));assert(id);free(body);
 char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);
 body=process_call(executable,state,request);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);
 Process p=launch_mode(executable,state,request,"--committed-crash");start(p);
 char discarded;assert(read(p.output,&discarded,1)==0);close(p.output);int status;
 assert(waitpid(p.pid,&status,0)==p.pid&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
 char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;sqlite3_stmt *stmt=NULL;
 assert(sqlite3_open(database,&db)==SQLITE_OK);
 assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM source_claims),(SELECT count(*) FROM upload_receipts),(SELECT count(*) FROM upload_chunks)",-1,&stmt,NULL)==SQLITE_OK);
 assert(sqlite3_step(stmt)==SQLITE_ROW&&sqlite3_column_int(stmt,0)==1&&sqlite3_column_int(stmt,1)==1&&sqlite3_column_int(stmt,2)==1&&sqlite3_column_int(stmt,3)==0);
 assert(sqlite3_finalize(stmt)==SQLITE_OK&&sqlite3_close(db)==SQLITE_OK);
 body=process_call(executable,state,request);char *retry=process_call(executable,state,request);assert(!strcmp(body,retry));free(retry);
 assert(json_parse(&j,body,strlen(body)));char *version=json_string(&j,json_get(&j,0,"version"));assert(version);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":5}",version,version);
 body=process_call(executable,state,request);assert(strstr(body,"hello"));free(body);free(version);free(id);
 puts("PASS upload postcommit SIGKILL: no response, one snapshot/claim/receipt, no staging and exact fresh-process commit retry/evidence");
}
int main(int argc,char **argv){
 if(argc==4&&(!strcmp(argv[1],"--request")||!strcmp(argv[1],"--crash")||!strcmp(argv[1],"--paused")||!strcmp(argv[1],"--committed-crash"))){unsigned paused_calls=0;PublicationControl control={.crash=!strcmp(argv[1],"--crash")};SourcesConfig child={.state=argv[2],.owner="alice",.snapshot_limit=1,.byte_limit=5};if(control.crash){child.cancelled=publication_control;child.cancellation=&control;}if(!strcmp(argv[1],"--paused")){child.cancelled=paused_control;child.cancellation=&paused_calls;}if(!strcmp(argv[1],"--committed-crash"))child.native_test_event=committed_death;return sources_dispatch(&child,argv[3],strlen(argv[3]),stdout);}
 assert(argc==1);
 postcommit_recovery(argv[0]);
 char state[]="/tmp/samosa-upload-session-XXXXXX";assert(mkdtemp(state));SourcesConfig cfg={.state=state,.owner="alice",.snapshot_limit=1,.byte_limit=5};
 char *description=call(&cfg,"{\"contract_version\":1,\"op\":\"describe\"}",0);assert(strstr(description,"upload_begin")&&strstr(description,"upload_append")&&strstr(description,"upload_commit")&&strstr(description,"\"upload_chunk_bytes\":8192")&&strstr(description,"\"upload_sessions\":8"));free(description);
 const char *begin="{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"first\",\"format\":\"text\",\"bytes\":5}";
 char *body=call(&cfg,begin,0);Json j;assert(json_parse(&j,body,strlen(body)));char *id=json_string(&j,json_get(&j,0,"upload_id"));assert(id);char *retry=call(&cfg,begin,0);assert(!strcmp(body,retry));free(body);free(retry);
 body=call(&cfg,"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"first\",\"format\":\"html\",\"bytes\":5}",1);assert(strstr(body,"idempotency_conflict"));free(body);
 char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_status\",\"upload_id\":\"%s\"}",id);SourcesConfig foreign=cfg;foreign.owner="bob";body=call(&foreign,request,1);assert(strstr(body,"unknown_upload"));free(body);body=call(&cfg,request,0);assert(strstr(body,"\"status\":\"open\"")&&strstr(body,"\"received_bytes\":0"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);
 body=call(&cfg,request,0);assert(strstr(body,"\"received_bytes\":5"));retry=call(&cfg,request,0);assert(!strcmp(body,retry));free(body);free(retry);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"d29ybGQ=\"}",id);body=call(&cfg,request,1);assert(strstr(body,"idempotency_conflict"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"YR==\"}",id);body=call(&cfg,request,1);assert(strstr(body,"invalid_upload_chunk"));free(body);
 char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);
 assert(sqlite3_exec(db,"UPDATE upload_sessions SET received=4",NULL,NULL,NULL)==SQLITE_OK);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_status\",\"upload_id\":\"%s\"}",id);body=call(&cfg,request,1);assert(strstr(body,"corrupt_upload"));free(body);
 assert(sqlite3_exec(db,"UPDATE upload_sessions SET received=5; UPDATE upload_chunks SET digest='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'",NULL,NULL,NULL)==SQLITE_OK);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);body=call(&cfg,request,1);assert(strstr(body,"corrupt_upload"));free(body);
 assert(sqlite3_exec(db,"UPDATE upload_chunks SET digest='2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824'",NULL,NULL,NULL)==SQLITE_OK);assert(sqlite3_close(db)==SQLITE_OK);
 size_t n;FILE *out=open_memstream(&body,&n);assert(out);assert(sources_upload(&cfg,SOURCES_UPLOAD_TEXT,"hello",5,out)==1);assert(!fclose(out));assert(strstr(body,"storage_limit"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_abort\",\"upload_id\":\"%s\"}",id);body=call(&cfg,request,0);assert(strstr(body,"aborted")&&strstr(body,"\"received_bytes\":0"));free(body);body=call(&cfg,begin,0);assert(strstr(body,"aborted"));free(body);
 out=open_memstream(&body,&n);assert(out);assert(!sources_upload(&cfg,SOURCES_UPLOAD_TEXT,"hello",5,out));assert(!fclose(out));free(body);free(id);
 char capacity_state[]="/tmp/samosa-upload-capacity-XXXXXX";assert(mkdtemp(capacity_state));SourcesConfig capacity={.state=capacity_state};
 for(unsigned i=0;i<8;i++){snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"session-%u\",\"format\":\"text\",\"bytes\":1}",i);body=call(&capacity,request,0);free(body);}
 body=call(&capacity,"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"ninth\",\"format\":\"text\",\"bytes\":1}",1);assert(strstr(body,"upload_capacity"));free(body);
 char publish_state[]="/tmp/samosa-upload-publish-XXXXXX";assert(mkdtemp(publish_state));SourcesConfig publish={.state=publish_state,.owner="alice",.snapshot_limit=1,.byte_limit=5};
 body=call(&publish,begin,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"upload_id"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);body=call(&publish,request,1);assert(strstr(body,"upload_incomplete"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);body=call(&publish,request,0);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);body=call(&publish,request,0);assert(strstr(body,"source_id"));retry=call(&publish,request,0);assert(!strcmp(body,retry));free(retry);assert(json_parse(&j,body,strlen(body)));char *source=json_string(&j,json_get(&j,0,"source_id"));assert(source);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":5}",source,source);body=call(&publish,request,0);assert(strstr(body,"hello"));free(body);free(source);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_status\",\"upload_id\":\"%s\"}",id);body=call(&publish,request,0);assert(strstr(body,"completed"));free(body);
 snprintf(database,sizeof(database),"%s/sources.sqlite3",publish_state);assert(sqlite3_open(database,&db)==SQLITE_OK);sqlite3_stmt *receipt_row=NULL;assert(sqlite3_prepare_v2(db,"SELECT response FROM upload_receipts",-1,&receipt_row,NULL)==SQLITE_OK);assert(sqlite3_step(receipt_row)==SQLITE_ROW);char *saved_receipt=strdup((const char*)sqlite3_column_text(receipt_row,0));assert(saved_receipt);sqlite3_finalize(receipt_row);
 assert(sqlite3_exec(db,"UPDATE upload_receipts SET response='{}'",NULL,NULL,NULL)==SQLITE_OK);body=call(&publish,request,1);assert(strstr(body,"corrupt_upload"));free(body);
 char receipt_digest[65];hash_bytes("{}",2,receipt_digest);assert(sqlite3_prepare_v2(db,"UPDATE upload_receipts SET digest=?",-1,&receipt_row,NULL)==SQLITE_OK);sqlite3_bind_text(receipt_row,1,receipt_digest,-1,SQLITE_TRANSIENT);assert(sqlite3_step(receipt_row)==SQLITE_DONE);sqlite3_finalize(receipt_row);body=call(&publish,request,1);assert(strstr(body,"corrupt_upload"));free(body);
 hash_bytes(saved_receipt,strlen(saved_receipt),receipt_digest);assert(sqlite3_prepare_v2(db,"UPDATE upload_receipts SET response=?,digest=?",-1,&receipt_row,NULL)==SQLITE_OK);sqlite3_bind_text(receipt_row,1,saved_receipt,-1,SQLITE_TRANSIENT);sqlite3_bind_text(receipt_row,2,receipt_digest,-1,SQLITE_TRANSIENT);assert(sqlite3_step(receipt_row)==SQLITE_DONE);sqlite3_finalize(receipt_row);free(saved_receipt);
 assert(sqlite3_exec(db,"DELETE FROM source_claims",NULL,NULL,NULL)==SQLITE_OK);body=call(&publish,request,1);assert(strstr(body,"corrupt_upload"));free(body);
 assert(sqlite3_exec(db,"INSERT INTO source_claims SELECT 'alice',id,'text/plain','upload:' FROM snapshots",NULL,NULL,NULL)==SQLITE_OK);assert(sqlite3_close(db)==SQLITE_OK);body=call(&publish,request,0);assert(strstr(body,"completed"));free(body);free(id);
 snprintf(database,sizeof(database),"%s/sources.sqlite3",publish_state);assert(sqlite3_open(database,&db)==SQLITE_OK);sqlite3_stmt *check=NULL;assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM upload_chunks),(SELECT count(*) FROM upload_receipts)",-1,&check,NULL)==SQLITE_OK);assert(sqlite3_step(check)==SQLITE_ROW);assert(sqlite3_column_int(check,0)==1&&sqlite3_column_int(check,1)==0&&sqlite3_column_int(check,2)==1);sqlite3_finalize(check);assert(sqlite3_close(db)==SQLITE_OK);
 char invalid_state[]="/tmp/samosa-upload-invalid-XXXXXX";assert(mkdtemp(invalid_state));SourcesConfig invalid={.state=invalid_state};
 body=call(&invalid,"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"bad-pdf\",\"format\":\"pdf\",\"bytes\":5}",0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"upload_id"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);body=call(&invalid,request,0);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);body=call(&invalid,request,1);assert(strstr(body,"unsupported_source"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_status\",\"upload_id\":\"%s\"}",id);body=call(&invalid,request,0);assert(strstr(body,"open")&&strstr(body,"\"received_bytes\":5"));free(body);free(id);
 snprintf(database,sizeof(database),"%s/sources.sqlite3",invalid_state);assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM upload_chunks),(SELECT count(*) FROM upload_receipts)",-1,&check,NULL)==SQLITE_OK);assert(sqlite3_step(check)==SQLITE_ROW);assert(sqlite3_column_int(check,0)==0&&sqlite3_column_int(check,1)==1&&sqlite3_column_int(check,2)==0);sqlite3_finalize(check);assert(sqlite3_close(db)==SQLITE_OK);
 char process_state[]="/tmp/samosa-upload-process-XXXXXX";assert(mkdtemp(process_state));
 body=process_call(argv[0],process_state,begin);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"upload_id"));assert(id);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);
 Process first=launch(argv[0],process_state,request),second=launch(argv[0],process_state,request);start(first);start(second);body=collect(first);retry=collect(second);assert(!strcmp(body,retry)&&strstr(body,"\"received_bytes\":5"));free(body);free(retry);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_status\",\"upload_id\":\"%s\"}",id);body=process_call(argv[0],process_state,request);assert(strstr(body,"open")&&strstr(body,"\"received_bytes\":5"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);first=launch(argv[0],process_state,request);second=launch(argv[0],process_state,request);start(first);start(second);body=collect(first);retry=collect(second);assert(!strcmp(body,retry)&&strstr(body,"source_id"));free(retry);assert(json_parse(&j,body,strlen(body)));source=json_string(&j,json_get(&j,0,"source_id"));assert(source);free(body);
 body=process_call(argv[0],process_state,request);assert(strstr(body,source));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":5}",source,source);body=process_call(argv[0],process_state,request);assert(strstr(body,"hello"));free(body);free(source);free(id);
 snprintf(database,sizeof(database),"%s/sources.sqlite3",process_state);assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM upload_chunks),(SELECT count(*) FROM upload_receipts),(SELECT count(*) FROM source_claims)",-1,&check,NULL)==SQLITE_OK);assert(sqlite3_step(check)==SQLITE_ROW);assert(sqlite3_column_int(check,0)==1&&sqlite3_column_int(check,1)==0&&sqlite3_column_int(check,2)==1&&sqlite3_column_int(check,3)==1);sqlite3_finalize(check);assert(sqlite3_close(db)==SQLITE_OK);
 char recovery_state[]="/tmp/samosa-upload-recovery-XXXXXX";assert(mkdtemp(recovery_state));SourcesConfig recovery={.state=recovery_state,.owner="alice",.snapshot_limit=1,.byte_limit=5};
 body=call(&recovery,begin,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"upload_id"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);body=call(&recovery,request,0);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);
 /* Checkpoint6 is the final scoped control sample after the receipt insert. */
 PublicationControl cancel={0};recovery.cancelled=publication_control;recovery.cancellation=&cancel;body=call(&recovery,request,1);assert(cancel.calls==6&&strstr(body,"cancelled"));free(body);recovery.cancelled=NULL;recovery.cancellation=NULL;
 snprintf(database,sizeof(database),"%s/sources.sqlite3",recovery_state);assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM upload_chunks),(SELECT count(*) FROM upload_receipts),status,received FROM upload_sessions",-1,&check,NULL)==SQLITE_OK);assert(sqlite3_step(check)==SQLITE_ROW);assert(sqlite3_column_int(check,0)==0&&sqlite3_column_int(check,1)==1&&sqlite3_column_int(check,2)==0&&sqlite3_column_int(check,4)==5&&!strcmp((const char*)sqlite3_column_text(check,3),"open"));sqlite3_finalize(check);assert(sqlite3_close(db)==SQLITE_OK);
 Process crash=launch_mode(argv[0],recovery_state,request,"--crash");start(crash);char discarded[128];assert(read(crash.output,discarded,sizeof(discarded))==0);close(crash.output);int crash_status;assert(waitpid(crash.pid,&crash_status,0)==crash.pid&&WIFSIGNALED(crash_status)&&WTERMSIG(crash_status)==SIGKILL);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_status\",\"upload_id\":\"%s\"}",id);body=process_call(argv[0],recovery_state,request);assert(strstr(body,"open")&&strstr(body,"\"received_bytes\":5"));free(body);
 assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM upload_chunks),(SELECT count(*) FROM upload_receipts)",-1,&check,NULL)==SQLITE_OK);assert(sqlite3_step(check)==SQLITE_ROW);assert(sqlite3_column_int(check,0)==0&&sqlite3_column_int(check,1)==1&&sqlite3_column_int(check,2)==0);sqlite3_finalize(check);assert(sqlite3_close(db)==SQLITE_OK);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);body=process_call(argv[0],recovery_state,request);retry=process_call(argv[0],recovery_state,request);assert(strstr(body,"source_id")&&!strcmp(body,retry));free(body);free(retry);free(id);
 char controlled_state[]="/tmp/samosa-upload-controlled-XXXXXX";assert(mkdtemp(controlled_state));SourcesConfig controlled={.state=controlled_state,.owner="alice"};body=call(&controlled,begin,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"upload_id"));assert(id);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\",\"operation_id\":\"append-controlled\",\"timeout_ms\":60000}",id);body=call(&controlled,request,0);assert(strstr(body,"append-controlled")&&strstr(body,"\"received_bytes\":5"));retry=call(&controlled,request,0);assert(!strcmp(body,retry));free(body);free(retry);
 body=call(&controlled,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"append-controlled\"}",0);assert(strstr(body,"\"status\":\"completed\""));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\",\"operation_id\":\"commit-controlled\",\"timeout_ms\":60000}",id);body=call(&controlled,request,0);assert(strstr(body,"commit-controlled")&&strstr(body,"source_id"));retry=call(&controlled,request,0);assert(!strcmp(body,retry));free(body);free(retry);
 body=call(&controlled,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"commit-controlled\"}",0);assert(strstr(body,"\"status\":\"completed\""));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_status\",\"upload_id\":\"%s\",\"operation_id\":\"commit-controlled\",\"timeout_ms\":60000}",id);body=call(&controlled,request,1);assert(strstr(body,"idempotency_conflict"));free(body);free(id);
 char public_state[]="/tmp/samosa-upload-public-controls-XXXXXX";assert(mkdtemp(public_state));SourcesConfig public_cfg={.state=public_state,.owner="alice"};body=call(&public_cfg,begin,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"upload_id"));assert(id);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);body=call(&public_cfg,request,0);free(body);
 PublicControl public_cancel={.cfg=public_cfg};public_cfg.cancelled=public_control;public_cfg.cancellation=&public_cancel;
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\",\"operation_id\":\"cancel-upload\",\"timeout_ms\":60000}",id);body=call(&public_cfg,request,1);assert(public_cancel.fired&&strstr(body,"cancelled"));free(body);public_cfg.cancelled=NULL;public_cfg.cancellation=NULL;
 body=call(&public_cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"cancel-upload\"}",0);assert(strstr(body,"\"status\":\"cancelled\""));free(body);body=call(&public_cfg,request,1);assert(strstr(body,"cancelled"));free(body);
 PublicControl public_deadline={.cfg=public_cfg,.deadline=1};public_cfg.cancelled=public_control;public_cfg.cancellation=&public_deadline;
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\",\"operation_id\":\"deadline-upload\",\"timeout_ms\":1000}",id);body=call(&public_cfg,request,1);assert(public_deadline.fired&&strstr(body,"deadline_exceeded"));free(body);public_cfg.cancelled=NULL;public_cfg.cancellation=NULL;
 body=call(&public_cfg,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"deadline-upload\"}",0);assert(strstr(body,"\"status\":\"failed\""));free(body);
 snprintf(database,sizeof(database),"%s/sources.sqlite3",public_state);assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM upload_receipts),(SELECT count(*) FROM upload_chunks),status,received FROM upload_sessions",-1,&check,NULL)==SQLITE_OK);assert(sqlite3_step(check)==SQLITE_ROW);assert(sqlite3_column_int(check,0)==0&&sqlite3_column_int(check,1)==0&&sqlite3_column_int(check,2)==1&&sqlite3_column_int(check,4)==5&&!strcmp((const char*)sqlite3_column_text(check,3),"open"));sqlite3_finalize(check);assert(sqlite3_close(db)==SQLITE_OK);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\",\"operation_id\":\"survivor-upload\",\"timeout_ms\":60000}",id);body=call(&public_cfg,request,0);assert(strstr(body,"source_id"));free(body);free(id);
 char external_state[]="/tmp/samosa-upload-external-XXXXXX";assert(mkdtemp(external_state));SourcesConfig external={.state=external_state,.owner="alice"};body=call(&external,begin,0);assert(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"upload_id"));free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_append\",\"upload_id\":\"%s\",\"offset\":0,\"data\":\"aGVsbG8=\"}",id);body=call(&external,request,0);free(body);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\",\"operation_id\":\"external-upload\",\"timeout_ms\":60000}",id);Process paused=launch_mode(argv[0],external_state,request,"--paused");start(paused);char ready;assert(read(paused.output,&ready,1)==1&&ready=='R');
 body=call(&external,"{\"contract_version\":1,\"op\":\"operation_get\",\"operation_id\":\"external-upload\"}",0);assert(strstr(body,"\"status\":\"running\""));free(body);
 body=call(&external,"{\"contract_version\":1,\"op\":\"operation_cancel\",\"operation_id\":\"external-upload\"}",0);assert(strstr(body,"\"cancel_requested\":true"));free(body);assert(write(paused.input,"x",1)==1);close(paused.input);body=collect_result(paused,1);assert(strstr(body,"cancelled"));free(body);
 snprintf(database,sizeof(database),"%s/sources.sqlite3",external_state);assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM upload_receipts),(SELECT count(*) FROM upload_chunks)",-1,&check,NULL)==SQLITE_OK);assert(sqlite3_step(check)==SQLITE_ROW);assert(sqlite3_column_int(check,0)==0&&sqlite3_column_int(check,1)==0&&sqlite3_column_int(check,2)==1);sqlite3_finalize(check);assert(sqlite3_close(db)==SQLITE_OK);
 snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"upload_commit\",\"upload_id\":\"%s\"}",id);body=process_call(argv[0],external_state,request);assert(strstr(body,"source_id"));free(body);free(id);
 char corrupt_state[]="/tmp/samosa-upload-reserved-XXXXXX";assert(mkdtemp(corrupt_state));SourcesConfig corrupt_cfg={.state=corrupt_state};body=call(&corrupt_cfg,begin,0);free(body);
 snprintf(database,sizeof(database),"%s/sources.sqlite3",corrupt_state);assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_exec(db,"UPDATE upload_sessions SET bytes=-5; INSERT INTO upload_sessions VALUES('local-owner','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','balance','text',5,0,'open')",NULL,NULL,NULL)==SQLITE_OK);assert(sqlite3_close(db)==SQLITE_OK);
 body=call(&corrupt_cfg,"{\"contract_version\":1,\"op\":\"upload_begin\",\"operation_id\":\"another\",\"format\":\"text\",\"bytes\":1}",1);assert(strstr(body,"corrupt_upload"));free(body);
 out=open_memstream(&body,&n);assert(out);assert(sources_upload(&corrupt_cfg,SOURCES_UPLOAD_TEXT,"hello",5,out)==1);assert(!fclose(out));assert(strstr(body,"corrupt_upload"));free(body);
 puts("PASS upload sessions: persisted begin/retry/status, owner isolation, parameter conflict, reserved snapshot quota, chunk retries/conflicts, corrupt offset/digest rejection, abort release, active-session cap, atomic publication/retry/read format rollback, fresh-process resume, concurrent append/commit and corrupt receipt/claim rejection, precommit cancellation SIGKILL recovery and durable upload request controls and public cancellation/deadline rollback");return 0;
}
