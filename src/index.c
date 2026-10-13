#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "index.h"
#include "enrichment.h"
#include "index_embedding.h"
#include "pdf.h"
#include "audio_index.h"
#include "visual_index.h"
#include "chutni.h"
#include "control.h"
#include "inventory.h"
#include "locator.h"
#include "json.h"
#include "sha256.h"
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#define INDEX_BYTES_MAX (8u*1048576u)
typedef struct {const SourcesConfig *cfg;sqlite3 *db;const char *owner;int directory,lock,locked;} Index;
static int fail(FILE *out,const char *code){fputs("{\"contract_version\":1,\"error\":{\"code\":",out);json_quote(out,code,strlen(code));fputs("}}\n",out);return 1;}
static int valid(const char *value,size_t max){if(!value||!*value||strlen(value)>max)return 0;for(const char *p=value;*p;p++){if(!isalnum((unsigned char)*p)&&!strchr("-_:.",*p))return 0;}return 1;}
static int execsql(Index *index,const char *sql){return sqlite3_exec(index->db,sql,NULL,NULL,NULL)==SQLITE_OK;}
static int prepare(Index *index,sqlite3_stmt **stmt,const char *sql){return sqlite3_prepare_v2(index->db,sql,-1,stmt,NULL)==SQLITE_OK;}
static void text(sqlite3_stmt *stmt,int column,const char *value){if(value)sqlite3_bind_text(stmt,column,value,-1,SQLITE_TRANSIENT);else sqlite3_bind_null(stmt,column);}
static char *column(sqlite3_stmt *stmt,int number){const char *value=(const char*)sqlite3_column_text(stmt,number);return value?strdup(value):NULL;}
static void raw(FILE *stream,const Json *j,int token){fwrite(j->text+j->tokens[token].begin,1,j->tokens[token].end-j->tokens[token].begin,stream);}
const char *source_index_lease(int directory,int *lease){
    *lease=openat(directory,"index.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(*lease<0)return "state_unavailable";
    struct stat info;const char *error=NULL;if(fstat(*lease,&info)||!S_ISREG(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&077)||info.st_nlink!=1)error="state_unavailable";
    else if(flock(*lease,LOCK_EX|LOCK_NB))error=errno==EWOULDBLOCK||errno==EAGAIN?"busy":"state_unavailable";
    if(error){close(*lease);*lease=-1;}return error;
}
static const char *lock_index(Index *index){
    if(index->locked)return NULL;
    const char *error=source_index_lease(index->directory,&index->lock);if(!error)index->locked=1;return error;
}
static const char *schema(Index *index){return execsql(index,
    "CREATE TABLE IF NOT EXISTS index_jobs(id TEXT PRIMARY KEY,owner TEXT NOT NULL,path TEXT NOT NULL,manifest TEXT NOT NULL,cursor INTEGER NOT NULL DEFAULT 0,total INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS index_items(job TEXT NOT NULL,ordinal INTEGER NOT NULL,locator TEXT NOT NULL,identity TEXT NOT NULL,eligible INTEGER NOT NULL,status TEXT NOT NULL DEFAULT 'pending',text TEXT,evidence TEXT,coverage TEXT,digest TEXT,error TEXT,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_visual_history(job TEXT NOT NULL,ordinal INTEGER NOT NULL,source TEXT NOT NULL,visual_job TEXT NOT NULL,superseded_revision INTEGER NOT NULL,PRIMARY KEY(job,ordinal,superseded_revision));"
    "CREATE TABLE IF NOT EXISTS index_visual(job TEXT NOT NULL,ordinal INTEGER NOT NULL,source TEXT NOT NULL,visual_job TEXT NOT NULL,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_audio_history(job TEXT NOT NULL,ordinal INTEGER NOT NULL,source TEXT NOT NULL,audio_job TEXT NOT NULL,superseded_revision INTEGER NOT NULL,PRIMARY KEY(job,ordinal,superseded_revision));"
    "CREATE TABLE IF NOT EXISTS index_root_scopes(job TEXT PRIMARY KEY,scope TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS index_audio(job TEXT NOT NULL,ordinal INTEGER NOT NULL,source TEXT NOT NULL,audio_job TEXT NOT NULL,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_discovery(job TEXT PRIMARY KEY,cycle INTEGER NOT NULL,manifest TEXT NOT NULL,digest TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS index_freshness_schedule(job TEXT PRIMARY KEY,ordinal INTEGER NOT NULL,next_ms INTEGER NOT NULL,turn INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS index_repaired(job TEXT NOT NULL,ordinal INTEGER NOT NULL,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_recovery(job TEXT NOT NULL,ordinal INTEGER NOT NULL,identity TEXT NOT NULL,next_ms INTEGER NOT NULL DEFAULT 0,failures INTEGER NOT NULL DEFAULT 0,turn INTEGER NOT NULL DEFAULT 0,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_refresh_schedule(job TEXT PRIMARY KEY,scan_next INTEGER NOT NULL DEFAULT 0,scan_failures INTEGER NOT NULL DEFAULT 0,scan_turn INTEGER NOT NULL DEFAULT 0,initial_next INTEGER NOT NULL DEFAULT 0,initial_failures INTEGER NOT NULL DEFAULT 0,initial_turn INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS index_versions(job TEXT NOT NULL,ordinal INTEGER NOT NULL,generation INTEGER NOT NULL,identity TEXT NOT NULL,text TEXT NOT NULL,evidence TEXT NOT NULL,coverage TEXT NOT NULL,digest TEXT NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(job,ordinal,generation));"
    "CREATE TABLE IF NOT EXISTS index_retry(job TEXT NOT NULL,ordinal INTEGER NOT NULL,target TEXT NOT NULL,next_ms INTEGER NOT NULL,failures INTEGER NOT NULL,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_schedule(job TEXT PRIMARY KEY,turn INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS index_attempts(job TEXT NOT NULL,ordinal INTEGER NOT NULL,attempts INTEGER NOT NULL,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_pending(job TEXT NOT NULL,ordinal INTEGER NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(job,ordinal));"
    "CREATE TABLE IF NOT EXISTS index_operations(owner TEXT NOT NULL,operation TEXT NOT NULL,request TEXT NOT NULL,job TEXT NOT NULL,status TEXT NOT NULL,result TEXT,PRIMARY KEY(owner,operation));")?source_locator_schema(index->db):((sqlite3_errcode(index->db)==SQLITE_BUSY||sqlite3_errcode(index->db)==SQLITE_LOCKED)?"store_busy":"store_failed");}
static void retry_key(const char *identity,const char *hash,char key[65]){
    char material[160];int n=snprintf(material,sizeof(material),"retry-target-v1:%s:%s",identity&&strlen(identity)==64&&hash&&strlen(hash)==64?identity:"unavailable",hash&&strlen(hash)==64?hash:"");hash_bytes(material,(size_t)n,key);
}
static char *indexed_version(const char *evidence,const char *digest,const char *audio){
    char *version=NULL;
    if(audio)version=strdup(audio);
    else {char actual[65];Json j;if(!evidence||!digest)return NULL;hash_bytes(evidence,strlen(evidence),actual);if(strcmp(actual,digest)||!json_parse(&j,evidence,strlen(evidence)))return NULL;version=json_string(&j,json_get(&j,0,"version"));}
    if(version&&(strlen(version)!=64||strspn(version,"0123456789abcdef")!=64)){free(version);version=NULL;}return version;
}
const char *source_index_observed(sqlite3 *db,const char *owner,const char *scope,const char *path,sqlite3_int64 revision,const char *identity,const char *hash){
    Index index={.db=db,.owner=owner};sqlite3_stmt *stmt=NULL;const char *error=NULL;
    /* Observation-only stores need no index tables until a job exists. */
    if(!prepare(&index,&stmt,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='index_jobs'"))return "store_failed";
    int exists=sqlite3_step(stmt);sqlite3_finalize(stmt);stmt=NULL;
    if(exists==SQLITE_DONE)return NULL;
    if(exists!=SQLITE_ROW)return "store_failed";
    if(!execsql(&index,"CREATE TABLE IF NOT EXISTS index_visual(job TEXT NOT NULL,ordinal INTEGER NOT NULL,source TEXT NOT NULL,visual_job TEXT NOT NULL,PRIMARY KEY(job,ordinal));CREATE TABLE IF NOT EXISTS index_audio(job TEXT NOT NULL,ordinal INTEGER NOT NULL,source TEXT NOT NULL,audio_job TEXT NOT NULL,PRIMARY KEY(job,ordinal));CREATE TABLE IF NOT EXISTS index_versions(job TEXT NOT NULL,ordinal INTEGER NOT NULL,generation INTEGER NOT NULL,identity TEXT NOT NULL,text TEXT NOT NULL,evidence TEXT NOT NULL,coverage TEXT NOT NULL,digest TEXT NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(job,ordinal,generation));CREATE TABLE IF NOT EXISTS index_pending(job TEXT NOT NULL,ordinal INTEGER NOT NULL,revision INTEGER NOT NULL,PRIMARY KEY(job,ordinal));CREATE TABLE IF NOT EXISTS index_attempts(job TEXT NOT NULL,ordinal INTEGER NOT NULL,attempts INTEGER NOT NULL,PRIMARY KEY(job,ordinal));CREATE TABLE IF NOT EXISTS index_retry(job TEXT NOT NULL,ordinal INTEGER NOT NULL,target TEXT NOT NULL,next_ms INTEGER NOT NULL,failures INTEGER NOT NULL,PRIMARY KEY(job,ordinal))"))return "store_failed";
    if(!prepare(&index,&stmt,"SELECT i.job,i.ordinal,coalesce(v.evidence,i.evidence),coalesce(v.digest,i.digest),coalesce(q.source,z.source) FROM index_items i LEFT JOIN index_audio q ON q.job=i.job AND q.ordinal=i.ordinal LEFT JOIN index_visual z ON z.job=i.job AND z.ordinal=i.ordinal LEFT JOIN index_versions v ON v.job=i.job AND v.ordinal=i.ordinal AND v.generation=(SELECT max(generation) FROM index_versions WHERE job=i.job AND ordinal=i.ordinal) JOIN index_jobs j ON j.id=i.job JOIN index_root_scopes s ON s.job=j.id WHERE j.owner=? AND s.scope=? AND i.locator=? AND i.status IN ('indexed','queued_audio','queued_visual')"))return "store_failed";
    text(stmt,1,owner);text(stmt,2,scope);text(stmt,3,path);
    SourcesConfig cfg={.owner=owner};
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *job=(const char*)sqlite3_column_text(stmt,0),*evidence=(const char*)sqlite3_column_text(stmt,2),*digest=(const char*)sqlite3_column_text(stmt,3);Json j;char *version=NULL;
        version=indexed_version(evidence,digest,(const char*)sqlite3_column_text(stmt,4));
        if(!job||!version)error="corrupt_index";
        char *observed=NULL;size_t n;FILE *out=NULL;
        if(!error){out=open_memstream(&observed,&n);if(!out)error="resource_limit";else {error=source_locator_freshness(&cfg,db,scope,path,version,out);if(fclose(out)&&!error)error="resource_limit";}}
        char *status=NULL;if(!error&&(!json_parse(&j,observed,n)||!(status=json_string(&j,json_get(&j,0,"status")))))error="corrupt_locator";
        sqlite3_stmt *update=NULL;
        if(!error){
            int available=!strcmp(status,"available");
            if(!prepare(&index,&update,available?"DELETE FROM index_pending WHERE job=? AND ordinal=?":"INSERT INTO index_pending VALUES(?,?,?) ON CONFLICT(job,ordinal) DO UPDATE SET revision=excluded.revision"))error="store_failed";
            else {text(update,1,job);sqlite3_bind_int64(update,2,sqlite3_column_int64(stmt,1));if(!available)sqlite3_bind_int64(update,3,revision);if(sqlite3_step(update)!=SQLITE_DONE)error="store_failed";}
        }
        sqlite3_finalize(update);update=NULL;
        if(!error&&!strcmp(status,"available")){
            if(!prepare(&index,&update,"DELETE FROM index_attempts WHERE job=? AND ordinal=?"))error="store_failed";
            else {text(update,1,job);sqlite3_bind_int64(update,2,sqlite3_column_int64(stmt,1));if(sqlite3_step(update)!=SQLITE_DONE)error="store_failed";}
        }
        sqlite3_finalize(update);update=NULL;
        if(!error){
            char target[65];retry_key(identity,hash,target);
            if(!prepare(&index,&update,!strcmp(status,"available")?"DELETE FROM index_retry WHERE job=? AND ordinal=?":"DELETE FROM index_retry WHERE job=? AND ordinal=? AND target!=?"))error="store_failed";
            else {text(update,1,job);sqlite3_bind_int64(update,2,sqlite3_column_int64(stmt,1));if(strcmp(status,"available"))text(update,3,target);if(sqlite3_step(update)!=SQLITE_DONE)error="store_failed";}
        }
        sqlite3_finalize(update);free(status);free(observed);free(version);
    }
    sqlite3_finalize(stmt);return error;
}
static sqlite3_int64 scalar(Index *index,const char *sql){sqlite3_stmt *stmt=NULL;sqlite3_int64 result=-1;if(prepare(index,&stmt,sql)&&sqlite3_step(stmt)==SQLITE_ROW)result=sqlite3_column_int64(stmt,0);sqlite3_finalize(stmt);return result;}
static const char *discovery_report(Index *index,const char *job,const Json *original,char **report){
    *report=NULL;
    sqlite3_stmt *stmt=NULL;
    if(!prepare(index,&stmt,"SELECT cycle,manifest,digest FROM index_discovery WHERE job=?"))return "store_failed";
    text(stmt,1,job);
    int rc=sqlite3_step(stmt);
    if(rc==SQLITE_DONE){sqlite3_finalize(stmt);*report=strdup("null");return *report?NULL:"resource_limit";}
    const char *error=NULL;
    if(rc!=SQLITE_ROW)error="store_failed";
    Json scan;char actual[65],*scope=NULL,*expected=NULL;
    int entries=-1,coverage=-1;
    sqlite3_int64 cycle=sqlite3_column_int64(stmt,0);
    const char *manifest=(const char*)sqlite3_column_text(stmt,1),*digest=(const char*)sqlite3_column_text(stmt,2);
    int length=sqlite3_column_bytes(stmt,1);
    if(!error){
        if(sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||cycle<1||sqlite3_column_type(stmt,1)!=SQLITE_TEXT||sqlite3_column_type(stmt,2)!=SQLITE_TEXT||!manifest||!digest||sqlite3_column_bytes(stmt,2)!=64||length<1||length>524288||strlen(manifest)!=(size_t)length)error="corrupt_index";
        else {
            hash_bytes(manifest,(size_t)length,actual);
            if(strcmp(actual,digest)||!json_parse(&scan,manifest,(size_t)length))error="corrupt_index";
        }
    }
    if(!error){
        scope=json_string(&scan,json_get(&scan,0,"scope_identity"));
        expected=json_string(original,json_get(original,0,"scope_identity"));
        entries=json_get(&scan,0,"entries");coverage=json_get(&scan,0,"inventory_coverage");
        if(!scope||!expected||strcmp(scope,expected)||entries<0||scan.tokens[entries].type!=JT_ARRAY||scan.tokens[entries].count>256||coverage<0||scan.tokens[coverage].type!=JT_OBJECT)error="corrupt_index";
    }
    if(!error){
        size_t n=0;FILE *stream=open_memstream(report,&n);
        if(!stream)error="resource_limit";
        else {
            fprintf(stream,"{\"cycle\":%lld,\"observed_items\":%d,\"inventory_coverage\":",(long long)cycle,scan.tokens[entries].count);
            raw(stream,&scan,coverage);
            fputs(",\"freshness\":\"captured_snapshot\"}",stream);
            if(fclose(stream))error="resource_limit";
        }
    }
    free(scope);free(expected);sqlite3_finalize(stmt);
    if(error){free(*report);*report=NULL;}
    return error;
}
static const char *recovery_report(Index *index,const char *job,char **report){
    *report=NULL;sqlite3_stmt *stmt=NULL;
    if(!prepare(index,&stmt,"SELECT r.ordinal,r.identity,r.next_ms,r.failures,r.turn,i.status FROM index_recovery r LEFT JOIN index_items i ON i.job=r.job AND i.ordinal=r.ordinal WHERE r.job=?"))return "store_failed";
    text(stmt,1,job);const char *error=NULL;size_t pending=0;int highest=0;sqlite3_int64 earliest=INT64_MAX;
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *identity=(const char*)sqlite3_column_text(stmt,1),*status=(const char*)sqlite3_column_text(stmt,5);
        sqlite3_int64 ordinal=sqlite3_column_int64(stmt,0),next=sqlite3_column_int64(stmt,2),level=sqlite3_column_int64(stmt,3),turn=sqlite3_column_int64(stmt,4);
        if(++pending>256||sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||ordinal<0||ordinal>=256||sqlite3_column_type(stmt,1)!=SQLITE_TEXT||sqlite3_column_bytes(stmt,1)!=64||!identity||strspn(identity,"0123456789abcdef")!=64||sqlite3_column_type(stmt,2)!=SQLITE_INTEGER||next<0||sqlite3_column_type(stmt,3)!=SQLITE_INTEGER||level<0||level>7||sqlite3_column_type(stmt,4)!=SQLITE_INTEGER||turn<0||!status||sqlite3_column_type(stmt,5)!=SQLITE_TEXT||sqlite3_column_bytes(stmt,5)!=(int)strlen(status)||(strcmp(status,"failed")&&strcmp(status,"skipped")))error="corrupt_index";
        if(error)break;
        if(next<earliest)earliest=next;
        if(level>highest)highest=(int)level;
    }
    sqlite3_finalize(stmt);
    if(!error){
        size_t n;FILE *out=open_memstream(report,&n);
        if(!out)error="resource_limit";
        else {
            fprintf(out,"{\"pending_items\":%zu,\"highest_failure_level\":%d,\"earliest_retry_unix_ms\":",pending,highest);
            if(pending)fprintf(out,"%lld",(long long)earliest);else fputs("null",out);
            fputc('}',out);if(fclose(out))error="resource_limit";
        }
    }
    if(error){free(*report);*report=NULL;}return error;
}
static int match_ascii(const char *,const char *);
#define IM_KIND "audio"
#define IM_REPORT audio_report
#define IM_READ retained_audio_read
#define IM_HISTORY retained_audio_history
#define IM_COMMIT commit_audio_version
#define IM_SEARCH_TYPE AudioSearch
#define IM_CANDIDATE audio_candidate_reply
#define IM_SEARCH search_audio
#define IM_ADMIT sources_audio_index_admit
#define IM_QUEUE_REPORT sources_audio_index_report
#define IM_QUEUE_READ sources_audio_index_read_retained
#define IM_QUEUE_SCAN sources_audio_index_scan_retained
#define IM_MAX_WINDOWS 497
#define IM_FAULT_MARKER "SAMOSA_AUDIO_REINDEX_TEST_ADMITTED_MARKER"
#include "index_media_impl.h"
#undef IM_KIND
#undef IM_REPORT
#undef IM_READ
#undef IM_HISTORY
#undef IM_COMMIT
#undef IM_SEARCH_TYPE
#undef IM_CANDIDATE
#undef IM_SEARCH
#undef IM_ADMIT
#undef IM_QUEUE_REPORT
#undef IM_QUEUE_READ
#undef IM_QUEUE_SCAN
#undef IM_MAX_WINDOWS
#undef IM_FAULT_MARKER
#define IM_KIND "visual"
#define IM_REPORT visual_report
#define IM_READ retained_visual_read
#define IM_HISTORY retained_visual_history
#define IM_COMMIT commit_visual_version
#define IM_SEARCH_TYPE VisualSearch
#define IM_CANDIDATE visual_candidate_reply
#define IM_SEARCH search_visual
#define IM_ADMIT sources_visual_index_admit
#define IM_QUEUE_REPORT sources_visual_index_report
#define IM_QUEUE_READ sources_visual_index_read_retained
#define IM_QUEUE_SCAN sources_visual_index_scan_retained
#define IM_MAX_WINDOWS 480
#define IM_FAULT_MARKER "SAMOSA_VISUAL_REINDEX_TEST_ADMITTED_MARKER"
#include "index_media_impl.h"
#undef IM_KIND
#undef IM_REPORT
#undef IM_READ
#undef IM_HISTORY
#undef IM_COMMIT
#undef IM_SEARCH_TYPE
#undef IM_CANDIDATE
#undef IM_SEARCH
#undef IM_ADMIT
#undef IM_QUEUE_REPORT
#undef IM_QUEUE_READ
#undef IM_QUEUE_SCAN
#undef IM_MAX_WINDOWS
#undef IM_FAULT_MARKER
static int job_reply(Index *index,const char *id,FILE *out){
    sqlite3_stmt *stmt=NULL;if(!prepare(index,&stmt,"SELECT path,manifest,cursor,total,(SELECT count(*) FROM index_items WHERE job=index_jobs.id AND status='indexed'),(SELECT count(*) FROM index_items WHERE job=index_jobs.id AND status='failed'),(SELECT count(*) FROM index_items WHERE job=index_jobs.id AND status='indexed' AND coverage!='complete'),(SELECT count(*) FROM index_repaired WHERE job=index_jobs.id),(SELECT count(*) FROM index_items WHERE job=index_jobs.id AND status='queued_audio'),(SELECT count(*) FROM index_items WHERE job=index_jobs.id AND status='queued_visual') FROM index_jobs WHERE id=? AND owner=?"))return fail(out,"store_failed");
    text(stmt,1,id);text(stmt,2,index->owner);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW){sqlite3_finalize(stmt);return fail(out,rc==SQLITE_DONE?"unknown_index":"store_failed");}
    const char *manifest=(const char*)sqlite3_column_text(stmt,1);Json captured;if(!manifest||!json_parse(&captured,manifest,strlen(manifest))){sqlite3_finalize(stmt);return fail(out,"corrupt_index");}
    int coverage=json_get(&captured,0,"inventory_coverage");
    if(coverage<0||captured.tokens[coverage].type!=JT_OBJECT){sqlite3_finalize(stmt);return fail(out,"corrupt_index");}
    char *latest=NULL;const char *report_error=discovery_report(index,id,&captured,&latest);
    if(report_error){sqlite3_finalize(stmt);return fail(out,report_error);}
    char *queue=NULL;report_error=recovery_report(index,id,&queue);
    if(report_error){free(latest);sqlite3_finalize(stmt);return fail(out,report_error);}
    size_t cursor=(size_t)sqlite3_column_int64(stmt,2),total=(size_t)sqlite3_column_int64(stmt,3),indexed=(size_t)sqlite3_column_int64(stmt,4),failed=(size_t)sqlite3_column_int64(stmt,5),partial=(size_t)sqlite3_column_int64(stmt,6),repaired=(size_t)sqlite3_column_int64(stmt,7);
    if(repaired>total){free(queue);free(latest);sqlite3_finalize(stmt);return fail(out,"corrupt_index");}
    size_t audio_total=(size_t)sqlite3_column_int64(stmt,8),audio_completed=0;char *audio=NULL;
    report_error=audio_report(index,id,audio_total,&audio,&audio_completed);
    if(report_error){free(queue);free(latest);sqlite3_finalize(stmt);return fail(out,report_error);}
    size_t visual_total=(size_t)sqlite3_column_int64(stmt,9),visual_completed=0;char *visual=NULL;
    report_error=visual_report(index,id,visual_total,&visual,&visual_completed);
    if(report_error){free(audio);free(queue);free(latest);sqlite3_finalize(stmt);return fail(out,report_error);}
    fprintf(out,"{\"contract_version\":1,\"job_id\":\"%s\",\"status\":\"%s\",\"cursor\":%zu,\"total_items\":%zu,\"indexed_items\":%zu,\"failed_items\":%zu,\"scope\":",id,cursor==total?(audio_total>audio_completed?"waiting_audio":visual_total>visual_completed?"waiting_visual":"completed"):"ready",cursor,total,indexed,failed);const char *path=(const char*)sqlite3_column_text(stmt,0);json_quote(out,path,strlen(path));
    fputs(",\"inventory_coverage\":",out);raw(out,&captured,coverage);
    char *inventory_status=json_string(&captured,json_get(&captured,coverage,"status"));int captured_entries=json_get(&captured,0,"entries");int complete_inventory=inventory_status&&!strcmp(inventory_status,"complete")&&captured_entries>=0&&(size_t)captured.tokens[captured_entries].count==total;free(inventory_status);
    fprintf(out,",\"recovery_queue\":%s",queue);free(queue);
    fprintf(out,",\"recovered_items_total\":%zu",repaired);
    fprintf(out,",\"queued_audio_items\":%zu,\"completed_audio_items\":%zu,\"audio_jobs\":%s",audio_total-audio_completed,audio_completed,audio);free(audio);
    fprintf(out,",\"queued_visual_items\":%zu,\"completed_visual_items\":%zu,\"visual_jobs\":%s",visual_total-visual_completed,visual_completed,visual);free(visual);
    fprintf(out,",\"latest_discovery\":%s",latest);free(latest);
    fprintf(out,",\"content_coverage\":\"%s\",\"freshness\":\"captured_snapshot\"}\n",!cursor?"uninspected":cursor==total&&indexed==total&&!failed&&!partial&&!repaired&&complete_inventory?"complete":"partial");sqlite3_finalize(stmt);return 0;
}
static char *buffered_job(Index *index,const char *id){char *result=NULL;size_t length;FILE *stream=open_memstream(&result,&length);if(!stream)return NULL;int rc=job_reply(index,id,stream);int closed=fclose(stream);if(rc||closed){free(result);return NULL;}return result;}
static const char *recover(Index *index){
    if(!execsql(index,"BEGIN IMMEDIATE"))return "store_busy";
    sqlite3_stmt *stmt=NULL;const char *error=NULL;
    if(!prepare(index,&stmt,"SELECT owner,operation,job FROM index_operations WHERE status='running'"))error="store_failed";
    while(!error){int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *owner=(const char*)sqlite3_column_text(stmt,0),*operation=(const char*)sqlite3_column_text(stmt,1),*job=(const char*)sqlite3_column_text(stmt,2);sqlite3_stmt *update=NULL;
        if(!prepare(index,&update,"UPDATE index_operations SET status='interrupted',result='{\"contract_version\":1,\"error\":{\"code\":\"interrupted\"}}' WHERE owner=? AND operation=? AND job=?"))error="store_failed";
        else {text(update,1,owner);text(update,2,operation);text(update,3,job);if(sqlite3_step(update)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(update);
    }
    sqlite3_finalize(stmt);if(error)execsql(index,"ROLLBACK");else if(!execsql(index,"COMMIT")){execsql(index,"ROLLBACK");error="store_failed";}return error;
}
static int operation_reply(Index *index,const char *operation,FILE *out){
    sqlite3_stmt *stmt=NULL;if(!prepare(index,&stmt,"SELECT job,status,result FROM index_operations WHERE owner=? AND operation=?"))return fail(out,"store_failed");text(stmt,1,index->owner);text(stmt,2,operation);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW){sqlite3_finalize(stmt);return fail(out,"unknown_operation");}
    fputs("{\"contract_version\":1,\"operation_id\":",out);json_quote(out,operation,strlen(operation));fprintf(out,",\"job_id\":\"%s\",\"status\":\"%s\",\"result\":%s}\n",(const char*)sqlite3_column_text(stmt,0),(const char*)sqlite3_column_text(stmt,1),sqlite3_column_text(stmt,2)?(const char*)sqlite3_column_text(stmt,2):"null");sqlite3_finalize(stmt);return 0;
}
static char *normalized(const char *kind,const char *operation,const char *selector,size_t batch){char *result=NULL;size_t n;FILE *stream=open_memstream(&result,&n);if(!stream)return NULL;fputs("{\"op\":",stream);json_quote(stream,kind,strlen(kind));fputs(",\"operation_id\":",stream);json_quote(stream,operation,strlen(operation));fputs(",\"selector\":",stream);json_quote(stream,selector,strlen(selector));fprintf(stream,",\"batch\":%zu}",batch);if(fclose(stream)){free(result);return NULL;}return result;}
static const char *retry(Index *index,const char *operation,const char *request,int *found){*found=0;sqlite3_stmt *stmt=NULL;const char *error=NULL;if(!prepare(index,&stmt,"SELECT request,status FROM index_operations WHERE owner=? AND operation=?"))error="store_failed";else {text(stmt,1,index->owner);text(stmt,2,operation);int rc=sqlite3_step(stmt);if(rc==SQLITE_ROW){*found=!strcmp((const char*)sqlite3_column_text(stmt,1),"running")?2:1;if(strcmp((const char*)sqlite3_column_text(stmt,0),request))error="idempotency_conflict";}else if(rc!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);return error;}
static const char *new_operation(Index *index,const char *operation,const char *request,const char *job){sqlite3_stmt *stmt=NULL;const char *error=NULL;if(!prepare(index,&stmt,"INSERT INTO index_operations(owner,operation,request,job,status) VALUES(?,?,?,?,'running')"))error="store_failed";else {text(stmt,1,index->owner);text(stmt,2,operation);text(stmt,3,request);text(stmt,4,job);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);return error;}
static const char *finish_operation(Index *index,const char *operation,const char *result){sqlite3_stmt *stmt=NULL;const char *error=NULL;if(!prepare(index,&stmt,"UPDATE index_operations SET status='completed',result=? WHERE owner=? AND operation=?"))error="store_failed";else {text(stmt,1,result);text(stmt,2,index->owner);text(stmt,3,operation);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);return error;}
static int create(Index *index,const Json *request,FILE *out){
    char *path=json_string(request,json_get(request,0,"path")),*operation=json_string(request,json_get(request,0,"operation_id")),*encoded=NULL,*manifest=NULL,*result=NULL;const char *error=NULL;int found=0,transaction=0;
    if(!path||!valid(operation,128))error="invalid_request";
    if(!error){encoded=normalized("index_create",operation,path,0);if(!encoded)error="resource_limit";else error=retry(index,operation,encoded,&found);}
    if(!error&&found==1)goto done;
    if(!error){error=lock_index(index);if(error&&found==2&&!strcmp(error,"busy")){error=NULL;goto done;}if(!error)error=recover(index);}
    if(!error)error=retry(index,operation,encoded,&found);
    if(!error&&found)goto done;
    if(!error){sqlite3_int64 jobs=scalar(index,"SELECT count(*) FROM index_jobs"),operations=scalar(index,"SELECT count(*) FROM index_operations");if(jobs<0||operations<0)error="store_failed";else if(jobs>=8||operations>=512)error="index_limit";}
    char root_scope[65];
    if(!error)error=inventory_scope_identity(index->cfg,"",root_scope);
    size_t manifest_length=0;
    if(!error){FILE *stream=open_memstream(&manifest,&manifest_length);if(!stream)error="resource_limit";else {int rc=inventory_run(index->cfg,path,stream);int closed=fclose(stream);if(closed)error="resource_limit";else if(rc){fwrite(manifest,1,manifest_length,out);free(path);free(operation);free(encoded);free(manifest);return 1;}}}
    Json captured;int entries=-1;if(!error){if(manifest_length>524288||!json_parse(&captured,manifest,manifest_length)||(entries=json_get(&captured,0,"entries"))<0||captured.tokens[entries].type!=JT_ARRAY||captured.tokens[entries].count>256)error="invalid_inventory";}
    if(!error){char current_root[65];error=inventory_scope_identity(index->cfg,"",current_root);if(!error&&strcmp(root_scope,current_root))error="source_changed";}
    char job[65];if(!error){Sha256 hash;unsigned char sum[32];sha256_init(&hash);sha256_update(&hash,index->owner,strlen(index->owner)+1);sha256_update(&hash,encoded,strlen(encoded));sha256_final(&hash,sum);sha256_hex(sum,job);if(!execsql(index,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    sqlite3_stmt *stmt=NULL;
    if(!error){if(!prepare(index,&stmt,"INSERT INTO index_jobs(id,owner,path,manifest,total) VALUES(?,?,?,?,?)"))error="store_failed";else {text(stmt,1,job);text(stmt,2,index->owner);text(stmt,3,path);text(stmt,4,manifest);sqlite3_bind_int(stmt,5,captured.tokens[entries].count);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
    if(!error){
        if(!prepare(index,&stmt,"INSERT INTO index_root_scopes VALUES(?,?)"))error="store_failed";
        else {text(stmt,1,job);text(stmt,2,root_scope);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    size_t ordinal=0;
    for(int item=entries+1;!error&&item<captured.tokens[entries].next;item=captured.tokens[item].next,ordinal++){
        char *locator=json_string(&captured,json_get(&captured,item,"locator")),*identity=json_string(&captured,json_get(&captured,item,"identity"));int eligible=json_get(&captured,item,"eligible");int allowed=eligible>=0&&captured.tokens[eligible].type==JT_BOOL&&captured.text[captured.tokens[eligible].begin]=='t';
        if(!locator||!valid(identity,64)||strlen(identity)!=64)error="invalid_inventory";
        else if(!prepare(index,&stmt,"INSERT INTO index_items(job,ordinal,locator,identity,eligible) VALUES(?,?,?,?,?)"))error="store_failed";else {text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);text(stmt,3,locator);text(stmt,4,identity);sqlite3_bind_int(stmt,5,allowed);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;free(locator);free(identity);
    }
    if(!error)error=new_operation(index,operation,encoded,job);
    if(!error){result=buffered_job(index,job);if(!result)error="store_failed";else error=finish_operation(index,operation,result);}
    if(transaction){if(error)execsql(index,"ROLLBACK");else if(!execsql(index,"COMMIT")){execsql(index,"ROLLBACK");error="store_failed";}}
done:
    free(path);free(encoded);free(manifest);free(result);int rc=error?fail(out,error):operation_reply(index,operation,out);free(operation);return rc;
}
static int reader_format(const char *locator){
    const char *dot=strrchr(locator,'.');if(!dot||strlen(dot)>8)return -1;char extension[9];size_t n=strlen(dot);for(size_t i=0;i<=n;i++)extension[i]=(char)tolower((unsigned char)dot[i]);
    if(!strcmp(extension,".html")||!strcmp(extension,".htm"))return 1;
    if(!strcmp(extension,".docx"))return 2;
    if(!strcmp(extension,".pdf"))return 3;
    const char *images[]={".png",".jpg",".jpeg",".bmp",".tif",".tiff",".gif",".webp",".pgm",".ppm",".pnm"};
    for(size_t i=0;i<sizeof(images)/sizeof(images[0]);i++)if(!strcmp(extension,images[i]))return 4;
    return !strcmp(extension,".txt")||!strcmp(extension,".md")||!strcmp(extension,".csv")||!strcmp(extension,".json")||!strcmp(extension,".log")?0:-1;
}
char *source_index_literal(const char *body,size_t length){
    Json j;if(!json_parse(&j,body,length))return NULL;
    int pages=json_get(&j,0,"pages");if(pages<0)return json_string(&j,json_get(&j,0,"text"));
    int range=json_get(&j,0,"range");size_t start=0,end=0;if(j.tokens[pages].type!=JT_ARRAY||!json_uint(&j,json_get(&j,range,"start_page"),&start)||!json_uint(&j,json_get(&j,range,"end_page"),&end)||!start||end<start||end-start>=4||(size_t)j.tokens[pages].count!=end-start+1)return NULL;
    char *literal=malloc(SOURCES_READ_MAX+1);if(!literal)return NULL;size_t n=0,page=start;
    for(int t=pages+1;t<j.tokens[pages].next;t=j.tokens[t].next,page++){
        size_t number=0;char *text=json_string(&j,json_get(&j,t,"text"));if(!text||!json_uint(&j,json_get(&j,t,"page"),&number)||number!=page||strlen(text)+(page>start)>SOURCES_READ_MAX-n){free(text);free(literal);return NULL;}
        if(page>start)literal[n++]='\n';
        size_t bytes=strlen(text);memcpy(literal+n,text,bytes);n+=bytes;free(text);
    }
    literal[n]=0;return literal;
}
static char *registration_request(const char *locator,const char *identity,size_t *length){
    char *result=NULL;FILE *stream=open_memstream(&result,length);if(!stream)return NULL;
    const char *ops[]={"register_text","register_html","register_docx","register_pdf","register_image"};
    int format=reader_format(locator);if(format<0){fclose(stream);free(result);return NULL;}fprintf(stream,"{\"contract_version\":1,\"op\":\"%s\",\"path\":",ops[format]);json_quote(stream,locator,strlen(locator));
    fputs(",\"expected_file_identity\":",stream);json_quote(stream,identity,strlen(identity));fputc('}',stream);
    if(fclose(stream)){free(result);return NULL;}return result;
}
static char *live_request(const char *locator,const char *source,size_t end,size_t *length){
    char *body=NULL;FILE *out=open_memstream(&body,length);if(!out)return NULL;
    fputs("{\"contract_version\":1,\"op\":\"locator_read\",\"path\":",out);
    json_quote(out,locator,strlen(locator));
    const char *ops[]={"read_text","read_html","read_docx","read_pdf","read_image"};int format=reader_format(locator);if(format<0){fclose(out);free(body);return NULL;}
    fprintf(out,",\"read\":{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"%s\":%d,\"%s\":%zu}}",ops[format],source,source,format==3?"start_page":"start_byte",format==3?1:0,format==3?"end_page":"end_byte",end);
    if(fclose(out)){free(body);return NULL;}return body;
}
static char *source_call(Index *index,const char *request,size_t length,int *rc){char *result=NULL;size_t n;FILE *stream=open_memstream(&result,&n);if(!stream){*rc=1;return NULL;}*rc=sources_dispatch(index->cfg,request,length,stream);if(fclose(stream)){free(result);*rc=1;return NULL;}return result;}
static char *response_error(const char *response){Json j;if(response&&json_parse(&j,response,strlen(response))){char *code=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));if(valid(code,128))return code;free(code);}return strdup("source_failed");}
static char *index_pdf_ocr_page(void *context,const char *locator,const char *source,size_t page,char **capture){
    Index *index=context;char *request=NULL;size_t length=0;FILE *f=open_memstream(&request,&length);if(!f)return strdup("resource_limit");
    fputs("{\"contract_version\":1,\"op\":\"locator_read\",\"path\":",f);json_quote(f,locator,strlen(locator));fprintf(f,",\"read\":{\"contract_version\":1,\"op\":\"read_pdf_ocr\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_page\":%zu,\"end_page\":%zu}}",source,source,page,page);
    if(fclose(f)){free(request);return strdup("resource_limit");}int rc;char *reply=source_call(index,request,length,&rc);free(request);
    if(rc){char *error=response_error(reply);free(reply);return error;}
    Json j;int evidence=-1;const char *error=NULL;if(!reply||!json_parse(&j,reply,strlen(reply))||(evidence=json_get(&j,0,"evidence"))<0||j.tokens[evidence].type!=JT_OBJECT)error="invalid_source_response";
    if(!error&&!(*capture=strndup(reply+j.tokens[evidence].begin,j.tokens[evidence].end-j.tokens[evidence].begin)))error="resource_limit";
    free(reply);return error?strdup(error):NULL;
}
/* Preserve text-layer pages exactly; OCR only empty selected pages through the
 * same live-bound public reader. Retain each constituent envelope as provenance. */
static char *pdf_index_fallback(void *context,char *(*read_page)(void *,const char *,const char *,size_t,char **),const char *locator,const char *source,const char *layer,char **error){
    Json text_json;if(!json_parse(&text_json,layer,strlen(layer))){*error=strdup("invalid_source_response");return NULL;}
    int pages=json_get(&text_json,0,"pages"),range=json_get(&text_json,0,"range"),coverage=json_get(&text_json,0,"coverage");size_t start=0,end=0,total=0;
    if(pages<0||text_json.tokens[pages].type!=JT_ARRAY||!json_uint(&text_json,json_get(&text_json,range,"start_page"),&start)||!json_uint(&text_json,json_get(&text_json,range,"end_page"),&end)||!start||end<start||end-start>=4||text_json.tokens[pages].count!=(int)(end-start+1)||!json_uint(&text_json,json_get(&text_json,coverage,"total_pages"),&total)){*error=strdup("invalid_source_response");return NULL;}
    char *recognized[4]={0};int selected[4]={0};size_t count=0;Sha256 identity;sha256_init(&identity);sha256_update(&identity,"pdfium-index-fallback-v1:",25);sha256_update(&identity,layer,strlen(layer));
    size_t ordinal=0;
    for(int token=pages+1;!*error&&token<text_json.tokens[pages].next;token=text_json.tokens[token].next,ordinal++){
        selected[ordinal]=token;char *value=json_string(&text_json,json_get(&text_json,token,"text"));if(!value){*error=strdup("invalid_source_response");break;}
        int blank=1;for(const unsigned char *p=(const unsigned char *)value;*p;p++)if(!isspace(*p)){blank=0;break;}free(value);if(!blank)continue;
        *error=read_page(context,locator,source,start+ordinal,&recognized[ordinal]);if(*error)break;
        sha256_update(&identity,recognized[ordinal],strlen(recognized[ordinal]));count++;
    }
    char *result=NULL;size_t length=0;FILE *f=NULL;
    if(!*error&&!count){result=strdup(layer);if(!result)*error=strdup("resource_limit");}
    if(!*error&&count){unsigned char digest[32];char id[65];sha256_final(&identity,digest);sha256_hex(digest,id);f=open_memstream(&result,&length);if(!f)*error=strdup("resource_limit");else {
        fprintf(f,"{\"contract_version\":1,\"kind\":\"literal\",\"evidence_id\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"range\":{\"start_page\":%zu,\"end_page\":%zu},\"coverage\":{\"status\":\"partial\",\"reason\":\"text_layer_with_ocr_fallback\",\"total_pages\":%zu,\"inspected_pages\":%zu,\"ocr_pages\":%zu},\"pages\":[",id,source,source,start,end,total,end-start+1,count);
        for(size_t i=0;!*error&&i<=end-start;i++){if(i)fputc(',',f);if(recognized[i]){Json j;int p=-1;if(!json_parse(&j,recognized[i],strlen(recognized[i]))||(p=json_get(&j,0,"pages"))<0||j.tokens[p].type!=JT_ARRAY||j.tokens[p].count!=1)*error=strdup("invalid_source_response");else raw(f,&j,p+1);}else raw(f,&text_json,selected[i]);}
        fprintf(f,"],\"provenance\":{\"reader\":\"pdfium-index-fallback-v1\",\"extractor_fingerprint\":\"%s\",\"locator\":",id);json_quote(f,locator,strlen(locator));fputs(",\"text_layer_capture\":",f);fputs(layer,f);fputs(",\"ocr_captures\":[",f);size_t n=0;for(size_t i=0;i<4;i++)if(recognized[i]){if(n++)fputc(',',f);fputs(recognized[i],f);}fputs("]}}",f);if(fclose(f)&&!*error)*error=strdup("resource_limit");
    }}
    for(size_t i=0;i<4;i++){free(recognized[i]);}if(*error){free(result);result=NULL;}return result;
}
typedef struct {const SourcesConfig *cfg;const void *bytes;size_t length;} PdfRegenerate;
static char *regenerate_pdf_ocr_page(void *context,const char *locator,const char *source,size_t page,char **capture){
    PdfRegenerate *input=context;char *body=NULL;size_t length=0;FILE *f=open_memstream(&body,&length);if(!f)return strdup("resource_limit");
    const char *error=pdf_ocr_extract(input->cfg,input->bytes,input->length,source,source,page,page,locator,f);if(fclose(f))error="resource_limit";
    Json j;if(!error&&!json_parse(&j,body,length))error="invalid_source_response";
    if(!error&&!(*capture=strndup(body+j.tokens[0].begin,j.tokens[0].end-j.tokens[0].begin)))error="resource_limit";
    free(body);return error?strdup(error):NULL;
}
char *source_index_pdf_regenerate(const SourcesConfig *cfg,const void *bytes,size_t length,const char *source,size_t start,size_t end,const char *locator,char **error){
    *error=NULL;char *body=NULL,*layer=NULL,*result=NULL;size_t n=0;FILE *f=open_memstream(&body,&n);if(!f){*error=strdup("resource_limit");return NULL;}
    const char *failure=pdf_extract(cfg,cfg->pdfium_library,bytes,length,source,source,start,end,locator,f);if(fclose(f))failure="resource_limit";
    Json j;if(!failure&&!json_parse(&j,body,n))failure="invalid_source_response";if(!failure&&!(layer=strndup(body+j.tokens[0].begin,j.tokens[0].end-j.tokens[0].begin)))failure="resource_limit";
    if(failure)*error=strdup(failure);else {PdfRegenerate input={cfg,bytes,length};result=pdf_index_fallback(&input,regenerate_pdf_ocr_page,locator,source,layer,error);}free(layer);free(body);return result;
}
static const char *discovery_root(Index *index,const char *job){
    sqlite3_stmt *stmt=NULL;const char *error=NULL;char actual[65];
    error=inventory_scope_identity(index->cfg,"",actual);
    if(error)return error;
    if(!prepare(index,&stmt,"SELECT scope FROM index_root_scopes WHERE job=?"))return "store_failed";
    text(stmt,1,job);
    if(sqlite3_step(stmt)!=SQLITE_ROW)error="corrupt_index";
    else{const char *scope=(const char*)sqlite3_column_text(stmt,0);if(!scope||strlen(scope)!=64)error="corrupt_index";else if(strcmp(scope,actual))error="scope_unavailable";}
    sqlite3_finalize(stmt);return error;
}
static int text_candidate(const char *);
static int discover(Index *index,const Json *request,FILE *out,int automatic){
    char *job=json_string(request,json_get(request,0,"job_id")),*operation=json_string(request,json_get(request,0,"operation_id")),*encoded=NULL,*path=NULL,*manifest=NULL,*original=NULL,*result=NULL;const char *error=NULL;int found=0,transaction=0;size_t total=0,n=0;sqlite3_stmt *stmt=NULL;
    if(!valid(job,64)||strlen(job)!=64||(!automatic&&!valid(operation,128)))error="invalid_request";
    if(!error&&!automatic){encoded=normalized("index_discover",operation,job,0);
    if(!encoded)error="resource_limit";else error=retry(index,operation,encoded,&found);}if(!error&&found==1)goto done;
    if(!error)error=lock_index(index);
    if(!error)error=recover(index);
    if(!error&&!automatic)error=retry(index,operation,encoded,&found);
    if(!error&&found)goto done;
    if(!error&&!automatic){sqlite3_int64 count=scalar(index,"SELECT count(*) FROM index_operations");
    if(count<0)error="store_failed";else if(count>=512)error="index_limit";}
    if(!error){if(!prepare(index,&stmt,"SELECT path,manifest,total FROM index_jobs WHERE id=? AND owner=?"))error="store_failed";else{text(stmt,1,job);text(stmt,2,index->owner);int rc=sqlite3_step(stmt);
    if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index":"store_failed";else{path=column(stmt,0);original=column(stmt,1);sqlite3_int64 saved=sqlite3_column_int64(stmt,2);
    if(!path||!original||saved<0||saved>256)error="corrupt_index";else total=(size_t)saved;}}sqlite3_finalize(stmt);stmt=NULL;}
    if(!error){char *queued=NULL;error=recovery_report(index,job,&queued);free(queued);}
    if(!error)error=discovery_root(index,job);
    char before[65],after[65];
    if(!error){Json captured;char *expected=NULL;
    if(!json_parse(&captured,original,strlen(original)))error="corrupt_index";else expected=json_string(&captured,json_get(&captured,0,"scope_identity"));
    if(!error)error=inventory_scope_identity(index->cfg,path,before);
    if(!error&&(!expected||strcmp(expected,before))){error="stale_scope";}
        free(expected);
    }
    if(!error){FILE *stream=open_memstream(&manifest,&n);
    if(!stream)error="resource_limit";else{int rc=inventory_run(index->cfg,path,stream);
    if(fclose(stream))error="resource_limit";else if(rc){fputs(manifest,out);free(job);free(operation);free(encoded);free(path);free(original);free(manifest);return 1;}}}
    Json inventory;int entries=-1;
    if(!error&&(n>524288||!json_parse(&inventory,manifest,n)||(entries=json_get(&inventory,0,"entries"))<0||inventory.tokens[entries].type!=JT_ARRAY||inventory.tokens[entries].count>256))error="invalid_inventory";
    if(!error)error=inventory_scope_identity(index->cfg,path,after);
    if(!error&&strcmp(before,after))error="source_changed";
    if(!error)error=discovery_root(index,job);
    if(!error){if(!execsql(index,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    for(int item=entries+1;!error&&item<inventory.tokens[entries].next;item=inventory.tokens[item].next){char *locator=json_string(&inventory,json_get(&inventory,item,"locator")),*identity=json_string(&inventory,json_get(&inventory,item,"identity"));int eligible=json_get(&inventory,item,"eligible"),exists=0;
        if(!locator||!valid(identity,64)||strlen(identity)!=64)error="invalid_inventory";
        if(!error){if(!prepare(index,&stmt,"SELECT count(*) FROM index_items WHERE job=? AND locator=?"))error="store_failed";else{text(stmt,1,job);text(stmt,2,locator);
    if(sqlite3_step(stmt)!=SQLITE_ROW)error="store_failed";else exists=sqlite3_column_int(stmt,0);}sqlite3_finalize(stmt);stmt=NULL;}
        if(!error&&!exists){if(total>=256)error="index_limit";else if(!prepare(index,&stmt,"INSERT INTO index_items(job,ordinal,locator,identity,eligible) VALUES(?,?,?,?,?)"))error="store_failed";else{text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)total);text(stmt,3,locator);text(stmt,4,identity);sqlite3_bind_int(stmt,5,eligible>=0&&inventory.tokens[eligible].type==JT_BOOL&&inventory.text[inventory.tokens[eligible].begin]=='t');
    if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";else total++;}sqlite3_finalize(stmt);stmt=NULL;}
        if(!error&&exists&&eligible>=0&&inventory.tokens[eligible].type==JT_BOOL&&inventory.text[inventory.tokens[eligible].begin]=='t'&&text_candidate(locator)){
            if(!prepare(index,&stmt,"INSERT INTO index_recovery(job,ordinal,identity) SELECT job,ordinal,? FROM index_items WHERE job=? AND locator=? AND status IN ('failed','skipped') ON CONFLICT(job,ordinal) DO UPDATE SET identity=excluded.identity,next_ms=0,failures=0 WHERE identity!=excluded.identity"))error="store_failed";
            else {text(stmt,1,identity);text(stmt,2,job);text(stmt,3,locator);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
            sqlite3_finalize(stmt);stmt=NULL;
        }
        free(locator);free(identity);
    }
    if(!error){if(!prepare(index,&stmt,"UPDATE index_jobs SET total=? WHERE id=? AND owner=?"))error="store_failed";else{sqlite3_bind_int64(stmt,1,(sqlite3_int64)total);text(stmt,2,job);text(stmt,3,index->owner);
    if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
    if(!error)error=sources_interrupted(index->cfg);
    if(!error)error=inventory_scope_identity(index->cfg,path,after);
    if(!error&&strcmp(before,after))error="source_changed";
    if(!error)error=discovery_root(index,job);
    if(!error){
        Json captured;char *previous=NULL;
        if(!json_parse(&captured,original,strlen(original)))error="corrupt_index";
        else error=discovery_report(index,job,&captured,&previous);
        free(previous);
    }
    if(!error){
        char digest[65];hash_bytes(manifest,n,digest);
        if(!prepare(index,&stmt,"INSERT INTO index_discovery(job,cycle,manifest,digest) VALUES(?,1,?,?) ON CONFLICT(job) DO UPDATE SET cycle=cycle+1,manifest=excluded.manifest,digest=excluded.digest WHERE cycle<9223372036854775807"))error="store_failed";
        else {
            text(stmt,1,job);text(stmt,2,manifest);text(stmt,3,digest);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
            else if(sqlite3_changes(index->db)!=1)error="index_limit";
        }
        sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error&&!automatic)error=new_operation(index,operation,encoded,job);
    if(!error){result=buffered_job(index,job);
    if(!result)error="store_failed";else if(!automatic)error=finish_operation(index,operation,result);}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *marker=getenv("SAMOSA_DISCOVERY_TEST_PRECOMMIT_MARKER"),*release=getenv("SAMOSA_DISCOVERY_TEST_PRECOMMIT_RELEASE");
    if(!error&&marker&&release){
        FILE *ready=fopen(marker,"w");if(!ready)_exit(98);
        fputs("prepared",ready);if(fclose(ready))_exit(98);
        int released=0;
        for(int waiting=0;waiting<500;waiting++){
            if(!access(release,F_OK)){released=1;break;}
            struct timespec delay={0,10000000};nanosleep(&delay,NULL);
        }
        if(!released)_exit(99);
    }
#endif
    if(!error)error=sources_interrupted(index->cfg);
    if(!error)error=inventory_scope_identity(index->cfg,path,after);
    if(!error&&strcmp(before,after))error="source_changed";
    if(!error)error=discovery_root(index,job);
    if(transaction){if(error)execsql(index,"ROLLBACK");else if(!execsql(index,"COMMIT")){execsql(index,"ROLLBACK");error="store_failed";}}
done: free(job);free(encoded);free(path);free(original);free(manifest);free(result);int rc=error?fail(out,error):automatic?0:operation_reply(index,operation,out);free(operation);return rc;
}
static int text_candidate(const char *locator){return reader_format(locator)>=0;}
static int visual_candidate(const char *locator){
    const char *extension=strrchr(locator,'.');const char *formats[]={".mp4",".mov",".mkv",".webm",".avi",".m4v",".mpeg",".mpg",".ts"};
    if(!extension)return 0;
    for(size_t i=0;i<sizeof(formats)/sizeof(*formats);i++)if(!strcasecmp(extension,formats[i]))return 1;
    return 0;
}
static int audio_candidate(const char *locator){
    const char *extension=strrchr(locator,'.');
    static const char *formats[]={".wav",".mp3",".m4a",".aac",".flac",".ogg",".opus",".aif",".aiff"};
    if(!extension)return 0;
    for(size_t i=0;i<sizeof(formats)/sizeof(*formats);i++)if(!strcasecmp(extension,formats[i]))return 1;
    return 0;
}
static char *capture_media(Index *index,const char *locator,const char *identity,char source[65],size_t *duration,int visual){
    const char *extension=strrchr(locator,'.');int wave=!visual&&extension&&!strcasecmp(extension,".wav");
    for(int attempt=0;attempt<2;attempt++){
        char *request=NULL;size_t length=0;FILE *stream=open_memstream(&request,&length);if(!stream)return strdup("resource_limit");
        fprintf(stream,"{\"contract_version\":1,\"op\":\"%s\",\"path\":",wave?"register_audio":"register_media");json_quote(stream,locator,strlen(locator));
        fputs(",\"expected_file_identity\":",stream);json_quote(stream,identity,strlen(identity));fputc('}',stream);
        if(fclose(stream)){free(request);return strdup("resource_limit");}
        int rc;char *response=source_call(index,request,length,&rc);free(request);
        if(rc){char *error=response_error(response);free(response);if(wave&&error&&!strcmp(error,"unsupported_audio_format")){free(error);wave=0;continue;}return error;}
        Json j;char *id=NULL;if(response&&json_parse(&j,response,strlen(response)))id=json_string(&j,json_get(&j,0,"source_id"));free(response);
        if(!id||strlen(id)!=64||!valid(id,64)){free(id);return strdup("invalid_source_response");}
        memcpy(source,id,65);free(id);break;
    }
    char request[256];int length=snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\"}",visual?"inspect_visual":"inspect_audio",source,source);
    int rc;char *response=source_call(index,request,(size_t)length,&rc);if(rc){char *error=response_error(response);free(response);return error;}
    Json j;int ok=response&&json_parse(&j,response,strlen(response))&&json_uint(&j,json_get(&j,0,"duration_ms"),duration)&&*duration&&*duration<=14400000;
    free(response);return ok?NULL:strdup("invalid_source_response");
}
static char *capture(Index *index,const char *locator,const char *identity,char **literal,char **coverage){
    *literal=*coverage=NULL;size_t request_length=0;char *request=registration_request(locator,identity,&request_length);if(!request)return strdup("resource_limit");int rc;char *registration=source_call(index,request,request_length,&rc);free(request);if(rc){char *error=response_error(registration);free(registration);return error;}
    Json j;char *source=NULL;size_t length=0;if(!registration||!json_parse(&j,registration,strlen(registration))||(source=json_string(&j,json_get(&j,0,"source_id")))==NULL||!valid(source,64)||strlen(source)!=64||!json_uint(&j,json_get(&j,0,"bytes"),&length)||length>SOURCES_INPUT_MAX){free(registration);free(source);return strdup("invalid_source_response");}free(registration);
    int format=reader_format(locator);size_t end=format==3?1:format?0:length>SOURCES_READ_MAX?SOURCES_READ_MAX:length;char *evidence=NULL,*error=NULL;
    for(size_t phase=0;phase<(format?2u:1u);phase++){
        for(size_t attempt=0;attempt<4;attempt++){request=live_request(locator,source,end,&request_length);if(!request){error=strdup("resource_limit");break;}evidence=source_call(index,request,request_length,&rc);free(request);if(!rc)break;error=response_error(evidence);free(evidence);evidence=NULL;if(!error||strcmp(error,"utf8_boundary")||!end)break;free(error);error=NULL;end--;}
        if(!evidence)break;
        int token;if(!json_parse(&j,evidence,strlen(evidence))||(token=json_get(&j,0,"evidence"))<0||j.tokens[token].type!=JT_OBJECT){free(evidence);evidence=NULL;error=strdup("invalid_source_response");break;}
        char *qualified=strndup(evidence+j.tokens[token].begin,j.tokens[token].end-j.tokens[token].begin);free(evidence);evidence=qualified;if(!evidence){error=strdup("resource_limit");break;}
        if(format&&phase==0){size_t total=0;if(!json_parse(&j,evidence,strlen(evidence))||!json_uint(&j,json_get(&j,json_get(&j,0,"coverage"),format==3?"total_pages":"total_bytes"),&total)||(format==3&&!total)){free(evidence);evidence=NULL;error=strdup("invalid_source_response");break;}size_t target=format==3?(total>4?4:total):total;if(target==end)break;end=target;free(evidence);evidence=NULL;}
    }
    if(evidence&&format==3&&index->cfg->ocr){char *combined=pdf_index_fallback(index,index_pdf_ocr_page,locator,source,evidence,&error);free(evidence);evidence=combined;}
    free(source);if(!evidence){if(!error)error=strdup("source_failed");return error;}
    if(!json_parse(&j,evidence,strlen(evidence))){free(evidence);return strdup("invalid_source_response");}*literal=source_index_literal(evidence,strlen(evidence));*coverage=json_string(&j,json_get(&j,json_get(&j,0,"coverage"),"status"));
    if(!*literal||strlen(*literal)>SOURCES_READ_MAX||!*coverage||(strcmp(*coverage,"complete")&&strcmp(*coverage,"partial"))){free(*literal);free(*coverage);free(evidence);*literal=*coverage=NULL;return strdup("invalid_source_response");}
    /* The caller separately stores literal text and the exact qualified envelope. */
    return evidence;
}
static const char *commit_item(Index *index,const char *job,size_t ordinal,const char *status,const char *literal,const char *evidence,const char *coverage,const char *item_error,const char *audio_source,size_t duration,int visual,char admission_error[65]){
    if(!execsql(index,"BEGIN IMMEDIATE"))return "store_busy";
    const char *error=NULL;sqlite3_stmt *stmt=NULL;
    char audio_job[65];
    if(audio_source){
        error=visual?sources_visual_index_admit(index->cfg,index->db,audio_source,duration,audio_job,admission_error):sources_audio_index_admit(index->cfg,index->db,audio_source,duration,audio_job,admission_error);
        if(!error){if(!prepare(index,&stmt,visual?"INSERT INTO index_visual VALUES(?,?,?,?)":"INSERT INTO index_audio VALUES(?,?,?,?)"))error="store_failed";else {text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);text(stmt,3,audio_source);text(stmt,4,audio_job);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
    }
    if(literal&&evidence){sqlite3_int64 usage=scalar(index,"SELECT coalesce(sum(length(CAST(text AS BLOB))+length(CAST(evidence AS BLOB))),0) FROM (SELECT text,evidence FROM index_items UNION ALL SELECT text,evidence FROM index_versions)");size_t extra=strlen(literal)+strlen(evidence);if(usage<0)error="store_failed";else if((size_t)usage>INDEX_BYTES_MAX||extra>INDEX_BYTES_MAX-(size_t)usage)error="index_limit";}
    if(!error){if(!prepare(index,&stmt,"UPDATE index_items SET status=?,text=?,evidence=?,coverage=?,digest=?,error=? WHERE job=? AND ordinal=? AND status='pending'"))error="store_failed";else {text(stmt,1,status);text(stmt,2,literal);text(stmt,3,evidence);text(stmt,4,coverage);char digest[65];if(evidence){hash_bytes(evidence,strlen(evidence),digest);text(stmt,5,digest);}else sqlite3_bind_null(stmt,5);text(stmt,6,item_error);text(stmt,7,job);sqlite3_bind_int64(stmt,8,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
    if(!error){if(!prepare(index,&stmt,"UPDATE index_jobs SET cursor=cursor+1 WHERE id=? AND owner=? AND cursor=?"))error="store_failed";else {text(stmt,1,job);text(stmt,2,index->owner);sqlite3_bind_int64(stmt,3,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="store_failed";}sqlite3_finalize(stmt);}
    if(error)execsql(index,"ROLLBACK");else if(!execsql(index,"COMMIT")){execsql(index,"ROLLBACK");error="store_failed";}return error;
}
static int step(Index *index,const Json *request,FILE *out,int automatic){
    char *job=json_string(request,json_get(request,0,"job_id")),*operation=json_string(request,json_get(request,0,"operation_id")),*encoded=NULL,*manifest=NULL,*path=NULL,*result=NULL;size_t batch=0,cursor=0,total=0;const char *error=NULL;char admission_error[65];int found=0;
    if(automatic)batch=1;
    if(!valid(job,64)||strlen(job)!=64||(!automatic&&(!valid(operation,128)||!json_uint(request,json_get(request,0,"max_items"),&batch)||!batch||batch>4)))error="invalid_request";
    if(!error&&!automatic){encoded=normalized("index_step",operation,job,batch);if(!encoded)error="resource_limit";else error=retry(index,operation,encoded,&found);}
    if(!error&&found==1)goto done;
    if(!error){error=lock_index(index);if(error&&found==2&&!strcmp(error,"busy")){error=NULL;goto done;}if(!error)error=recover(index);}
    if(!error&&!automatic)error=retry(index,operation,encoded,&found);
    if(!error&&found)goto done;
    sqlite3_stmt *stmt=NULL;
    if(!error){if(!prepare(index,&stmt,"SELECT cursor,total,path,manifest FROM index_jobs WHERE id=? AND owner=?"))error="store_failed";else {text(stmt,1,job);text(stmt,2,index->owner);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index":"store_failed";else {cursor=(size_t)sqlite3_column_int64(stmt,0);total=(size_t)sqlite3_column_int64(stmt,1);path=column(stmt,2);manifest=column(stmt,3);if(!path||!manifest||total>256||cursor>total)error="corrupt_index";}}sqlite3_finalize(stmt);stmt=NULL;}
    if(!error){Json captured;char actual[65],*expected=NULL;if(!json_parse(&captured,manifest,strlen(manifest)))error="corrupt_index";else {expected=json_string(&captured,json_get(&captured,0,"scope_identity"));error=inventory_scope_identity(index->cfg,path,actual);if(!error&&(!expected||strcmp(expected,actual)))error="stale_scope";}free(expected);}
    if(!error&&!automatic){sqlite3_int64 count=scalar(index,"SELECT count(*) FROM index_operations");if(count<0)error="store_failed";else if(count>=512)error="index_limit";}
    if(!error&&!automatic)error=new_operation(index,operation,encoded,job); /* durable admission before capture */
    size_t target=cursor+batch;if(target>total)target=total;
    for(size_t ordinal=cursor;!error&&ordinal<target;ordinal++){
        char *locator=NULL,*identity=NULL;int eligible=0;
        if(!prepare(index,&stmt,"SELECT locator,identity,eligible FROM index_items WHERE job=? AND ordinal=? AND status='pending'"))error="store_failed";
        else {text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_ROW)error="corrupt_index";else {locator=column(stmt,0);identity=column(stmt,1);eligible=sqlite3_column_int(stmt,2);if(!locator||!identity)error="corrupt_index";}}sqlite3_finalize(stmt);stmt=NULL;
        char *literal=NULL,*evidence=NULL,*coverage=NULL,*item_error=NULL;char audio_source[65]={0};size_t audio_duration=0;int visual=locator&&visual_candidate(locator);
        if(!error&&eligible&&visual)item_error=capture_media(index,locator,identity,audio_source,&audio_duration,1);
        if(!error&&eligible&&audio_candidate(locator))item_error=capture_media(index,locator,identity,audio_source,&audio_duration,0);
        if(!error&&eligible&&!audio_candidate(locator)&&!visual){if(!text_candidate(locator))item_error=strdup("unsupported_reader");else {sqlite3_int64 bytes=scalar(index,"SELECT coalesce(sum(length(CAST(text AS BLOB))+length(CAST(evidence AS BLOB))),0) FROM (SELECT text,evidence FROM index_items UNION ALL SELECT text,evidence FROM index_versions)");if(bytes<0)error="store_failed";else if(bytes>INDEX_BYTES_MAX-524288)item_error=strdup("index_limit");else {char *captured=capture(index,locator,identity,&literal,&coverage);if(literal)evidence=captured;else item_error=captured;}}}
        if(!error&&eligible&&!literal&&!item_error&&!audio_duration)error="resource_limit";
        if(!error)error=commit_item(index,job,ordinal,!eligible?"skipped":item_error?"failed":audio_duration?(visual?"queued_visual":"queued_audio"):"indexed",literal,evidence,coverage,item_error,!item_error&&audio_duration?audio_source:NULL,audio_duration,visual,admission_error);
        free(locator);free(identity);free(literal);free(evidence);free(coverage);free(item_error);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
        const char *marker=getenv("SAMOSA_INDEX_TEST_COMMITTED_MARKER");
        if(!error&&marker){FILE *ready=fopen(marker,"w");if(!ready)_exit(98);fputs("committed",ready);fclose(ready);for(;;)pause();}
#endif
    }
    if(!error){result=buffered_job(index,job);if(!result)error="store_failed";else if(!automatic)error=finish_operation(index,operation,result);}
done:
    free(job);free(encoded);free(manifest);free(path);free(result);int rc=error?fail(out,error):automatic?0:operation_reply(index,operation,out);free(operation);return rc;
}
static const char *commit_version(Index *index,const char *job,size_t ordinal,sqlite3_int64 revision,const char *scope,const char *path,const char *identity,const char *hash,const char *literal,const char *evidence,const char *coverage){
    if(!execsql(index,"BEGIN IMMEDIATE"))return "store_busy";
    char checked_identity[65],checked_hash[65],actual_scope[65];const char *error=source_locator_index_target(index->cfg,index->db,scope,path,revision,checked_identity,checked_hash);
    if(!error&&(strcmp(identity,checked_identity)||strcmp(hash,checked_hash)))error="stale_observation";
    if(!error)error=inventory_scope_identity(index->cfg,"",actual_scope);
    if(!error&&strcmp(actual_scope,scope))error="stale_scope";
    sqlite3_int64 usage=scalar(index,"SELECT coalesce(sum(length(CAST(text AS BLOB))+length(CAST(evidence AS BLOB))),0) FROM (SELECT text,evidence FROM index_items UNION ALL SELECT text,evidence FROM index_versions)"),versions=scalar(index,"SELECT count(*) FROM index_versions");
    size_t extra=strlen(literal)+strlen(evidence);
    if(!error&&(usage<0||versions<0))error="store_failed";
    if(!error&&(versions>=256||(size_t)usage>INDEX_BYTES_MAX||extra>INDEX_BYTES_MAX-(size_t)usage))error="index_limit";
    sqlite3_stmt *stmt=NULL;
    if(!error&&!prepare(index,&stmt,"INSERT INTO index_versions SELECT ?,?,coalesce(max(generation),0)+1,?,?,?,?,?,? FROM index_versions WHERE job=? AND ordinal=?"))error="store_failed";
    if(!error){char digest[65];hash_bytes(evidence,strlen(evidence),digest);text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);text(stmt,3,identity);text(stmt,4,literal);text(stmt,5,evidence);text(stmt,6,coverage);text(stmt,7,digest);sqlite3_bind_int64(stmt,8,revision);text(stmt,9,job);sqlite3_bind_int64(stmt,10,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(index,&stmt,"DELETE FROM index_pending WHERE job=? AND ordinal=? AND revision=?"))error="store_failed";
    if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);sqlite3_bind_int64(stmt,3,revision);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="stale_observation";}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(index,&stmt,"DELETE FROM index_attempts WHERE job=? AND ordinal=?"))error="store_failed";
    if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(index,&stmt,"DELETE FROM index_retry WHERE job=? AND ordinal=?"))error="store_failed";
    if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
    sqlite3_finalize(stmt);
    if(!error)error=sources_interrupted(index->cfg);
    if(error)execsql(index,"ROLLBACK");else if(!execsql(index,"COMMIT")){execsql(index,"ROLLBACK");error="store_failed";}return error;
}
static const char *retry_clock(sqlite3_int64 *now){
    struct timespec time;if(clock_gettime(CLOCK_REALTIME,&time)||time.tv_sec<0||(uint64_t)time.tv_sec>(uint64_t)(INT64_MAX-61000)/1000)return "clock_unavailable";
    *now=(sqlite3_int64)time.tv_sec*1000+time.tv_nsec/1000000;return NULL;
}
static const char *defer_item(Index *index,const char *job,sqlite3_int64 ordinal,sqlite3_int64 revision,const char *identity,const char *hash){
    sqlite3_int64 now;const char *error=retry_clock(&now);sqlite3_stmt *stmt=NULL;int failures=1;
    if(!error&&!prepare(index,&stmt,"SELECT failures FROM index_retry WHERE job=? AND ordinal=?"))error="store_failed";
    if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,ordinal);int rc=sqlite3_step(stmt);if(rc==SQLITE_ROW){int saved=sqlite3_column_int(stmt,0);if(sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||saved<1||saved>7)error="corrupt_index";else failures=saved<7?saved+1:7;}else if(rc!=SQLITE_DONE)error="store_failed";}
    sqlite3_finalize(stmt);stmt=NULL;sqlite3_int64 delay=failures==7?60000:((sqlite3_int64)1000<<(failures-1));char target[65];retry_key(identity,hash,target);
    if(!error&&!prepare(index,&stmt,"INSERT INTO index_retry SELECT job,ordinal,?,?,? FROM index_pending WHERE job=? AND ordinal=? AND revision=? ON CONFLICT(job,ordinal) DO UPDATE SET target=excluded.target,next_ms=excluded.next_ms,failures=excluded.failures"))error="store_failed";
    if(!error){text(stmt,1,target);sqlite3_bind_int64(stmt,2,now+delay);sqlite3_bind_int(stmt,3,failures);text(stmt,4,job);sqlite3_bind_int64(stmt,5,ordinal);sqlite3_bind_int64(stmt,6,revision);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";else if(sqlite3_changes(index->db)!=1)error="stale_observation";}
    sqlite3_finalize(stmt);return error;
}
static const char *finish_defer(Index *index,const char *job,sqlite3_int64 ordinal,const char *identity,const char *hash){
    char target[65];retry_key(identity,hash,target);sqlite3_stmt *stmt=NULL;sqlite3_int64 now;const char *error=retry_clock(&now);int level=0;
    if(!error&&!prepare(index,&stmt,"SELECT failures FROM index_retry WHERE job=? AND ordinal=? AND target=?"))error="store_failed";
    if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,ordinal);text(stmt,3,target);int rc=sqlite3_step(stmt);if(rc==SQLITE_ROW){level=sqlite3_column_int(stmt,0);if(sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||level<1||level>7)error="corrupt_index";}else if(rc!=SQLITE_DONE)error="store_failed";}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&level){
        sqlite3_int64 delay=level==7?60000:((sqlite3_int64)1000<<(level-1));
        if(!prepare(index,&stmt,"UPDATE index_retry SET next_ms=MAX(next_ms,?) WHERE job=? AND ordinal=? AND target=?"))error="store_failed";
        else {sqlite3_bind_int64(stmt,1,now+delay);text(stmt,2,job);sqlite3_bind_int64(stmt,3,ordinal);text(stmt,4,target);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
    }
    sqlite3_finalize(stmt);return error;
}
static int reindex(Index *index,const Json *request,FILE *out,int automatic){
    char *job=json_string(request,json_get(request,0,"job_id")),*operation=json_string(request,json_get(request,0,"operation_id")),*encoded=NULL,*scope=NULL,*result=NULL;size_t batch=0,processed=0;const char *error=NULL;char admission_error[65];int found=0;sqlite3_int64 next_turn=1,now=0;
    if(automatic)batch=1;
    else if(!valid(job,64)||strlen(job)!=64||!valid(operation,128)||!json_uint(request,json_get(request,0,"max_items"),&batch)||!batch||batch>4)error="invalid_request";
    if(!error&&!automatic){encoded=normalized("index_reindex_step",operation,job,batch);if(!encoded)error="resource_limit";else error=retry(index,operation,encoded,&found);}
    if(!error&&found==1)goto done;
    if(!error){error=lock_index(index);if(error&&found==2&&!strcmp(error,"busy")){error=NULL;goto done;}if(!error)error=recover(index);}
    if(!error&&!automatic)error=retry(index,operation,encoded,&found);
    if(!error&&found)goto done;
    sqlite3_stmt *stmt=NULL;
    if(!error&&automatic){
        error=retry_clock(&now);
        if(!error&&!prepare(index,&stmt,"SELECT next_ms,failures,target FROM index_retry"))error="store_failed";
        size_t retries=0;
        while(!error){int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}const char *target=(const char*)sqlite3_column_text(stmt,2);if(++retries>2048||sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||sqlite3_column_int64(stmt,0)<0||sqlite3_column_type(stmt,1)!=SQLITE_INTEGER||sqlite3_column_int(stmt,1)<1||sqlite3_column_int(stmt,1)>7||!valid(target,64)||strlen(target)!=64)error="corrupt_index";}
        sqlite3_finalize(stmt);stmt=NULL;
        if(!error&&!prepare(index,&stmt,"SELECT turn FROM index_schedule"))error="store_failed";
        size_t scheduled=0;
        while(!error){int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}sqlite3_int64 turn=sqlite3_column_int64(stmt,0);if(++scheduled>8||sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||turn<0)error="corrupt_index";else if(turn==INT64_MAX)error="scheduler_limit";else if(turn>=next_turn)next_turn=turn+1;}
        sqlite3_finalize(stmt);stmt=NULL;
        char actual[65];if(!error)error=inventory_scope_identity(index->cfg,"",actual);
        if(!error&&!prepare(index,&stmt,"SELECT p.job FROM index_pending p JOIN index_jobs j ON j.id=p.job JOIN index_root_scopes s ON s.job=j.id LEFT JOIN index_attempts a ON a.job=p.job AND a.ordinal=p.ordinal LEFT JOIN index_schedule r ON r.job=p.job LEFT JOIN index_retry b ON b.job=p.job AND b.ordinal=p.ordinal WHERE j.owner=? AND s.scope=? AND (b.next_ms IS NULL OR b.next_ms<=? OR b.next_ms>?) ORDER BY coalesce(r.turn,0),coalesce(a.attempts,0),p.job,p.ordinal LIMIT 1"))error="store_failed";
        if(!error){text(stmt,1,index->owner);text(stmt,2,actual);sqlite3_bind_int64(stmt,3,now);sqlite3_bind_int64(stmt,4,now+60000);int rc=sqlite3_step(stmt);if(rc==SQLITE_ROW){free(job);job=column(stmt,0);if(!valid(job,64)||strlen(job)!=64)error="corrupt_index";}else if(rc!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
        if(!error&&!job){fputs("{\"contract_version\":1,\"idle\":true,\"reindexed_items\":0}\n",out);free(operation);free(encoded);return 0;}
    }
    if(!error&&!prepare(index,&stmt,"SELECT s.scope FROM index_jobs j JOIN index_root_scopes s ON s.job=j.id WHERE j.id=? AND j.owner=?"))error="store_failed";
    if(!error){text(stmt,1,job);text(stmt,2,index->owner);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index":"store_failed";else {scope=column(stmt,0);if(!scope||strlen(scope)!=64)error="corrupt_index";}}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error){char actual[65];error=inventory_scope_identity(index->cfg,"",actual);if(!error&&strcmp(scope,actual))error="stale_scope";}
    if(!error&&!automatic){sqlite3_int64 count=scalar(index,"SELECT count(*) FROM index_operations");if(count<0)error="store_failed";else if(count>=512)error="index_limit";}
    if(!error&&automatic){
        if(!prepare(index,&stmt,"INSERT INTO index_schedule VALUES(?,?) ON CONFLICT(job) DO UPDATE SET turn=excluded.turn"))error="store_failed";
        else {text(stmt,1,job);sqlite3_bind_int64(stmt,2,next_turn);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error&&!automatic)error=new_operation(index,operation,encoded,job);
    for(size_t i=0;!error&&i<batch;i++){
        char *path=NULL;sqlite3_int64 revision=0,ordinal=0;
        if(!prepare(index,&stmt,"SELECT p.ordinal,p.revision,i.locator,coalesce(a.attempts,0) FROM index_pending p JOIN index_items i ON i.job=p.job AND i.ordinal=p.ordinal LEFT JOIN index_attempts a ON a.job=p.job AND a.ordinal=p.ordinal WHERE p.job=? AND (?=0 OR NOT EXISTS(SELECT 1 FROM index_retry b WHERE b.job=p.job AND b.ordinal=p.ordinal AND b.next_ms>? AND b.next_ms<=?)) ORDER BY coalesce(a.attempts,0),p.ordinal LIMIT 1"))error="store_failed";
        if(!error){text(stmt,1,job);sqlite3_bind_int(stmt,2,automatic);sqlite3_bind_int64(stmt,3,now);sqlite3_bind_int64(stmt,4,now+60000);int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE){sqlite3_finalize(stmt);stmt=NULL;break;}if(rc!=SQLITE_ROW)error="store_failed";else {ordinal=sqlite3_column_int64(stmt,0);revision=sqlite3_column_int64(stmt,1);path=column(stmt,2);if(!path||ordinal<0||ordinal>=256||revision<1||sqlite3_column_type(stmt,3)!=SQLITE_INTEGER||sqlite3_column_int64(stmt,3)<0)error="corrupt_index";}}
        sqlite3_finalize(stmt);stmt=NULL;int deferred_admitted=0;char identity[65]={0},hash[65]={0};char *literal=NULL,*evidence=NULL,*coverage=NULL;
        /* Persist attempted admission before content work, including killed workers. */
        if(!error&&!prepare(index,&stmt,"INSERT INTO index_attempts SELECT job,ordinal,1 FROM index_pending WHERE job=? AND ordinal=? AND revision=? ON CONFLICT(job,ordinal) DO UPDATE SET attempts=MIN(attempts,9223372036854775806)+1"))error="store_failed";
        if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,ordinal);sqlite3_bind_int64(stmt,3,revision);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";else if(sqlite3_changes(index->db)!=1)error="stale_observation";}
        sqlite3_finalize(stmt);stmt=NULL;
        if(!error)error=source_locator_index_target(index->cfg,index->db,scope,path,revision,identity,hash);
        if(automatic&&(!error||!strcmp(error,"live_unavailable"))){const char *deferred=defer_item(index,job,ordinal,revision,identity,hash);if(deferred)error=deferred;else deferred_admitted=1;}

#ifdef SAMOSA_SOURCES_TEST_FAULTS
        const char *retry_marker=getenv("SAMOSA_REINDEX_TEST_RETRY_MARKER"),*retry_release=getenv("SAMOSA_REINDEX_TEST_RETRY_RELEASE");
        if(deferred_admitted&&retry_marker&&retry_release){FILE *ready=fopen(retry_marker,"w");if(!ready)_exit(98);fputs("reserved",ready);fclose(ready);int released=0;for(int waiting=0;waiting<500;waiting++){if(!access(retry_release,F_OK)){released=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}if(!released)_exit(99);}
#endif
        char *capture_error=NULL;char audio_source[65]={0};size_t audio_duration=0;int visual=path&&visual_candidate(path);int audio=path&&(audio_candidate(path)||visual);
        if(!error&&audio){capture_error=capture_media(index,path,identity,audio_source,&audio_duration,visual);if(capture_error)error=capture_error;else if(strcmp(audio_source,hash))error="stale_observation";}
        if(!error&&!audio){char *captured=capture(index,path,identity,&literal,&coverage);if(literal)evidence=captured;else {capture_error=captured;error=capture_error?capture_error:"resource_limit";}}
        if(!error&&!audio){Json j;char *version=NULL;if(!json_parse(&j,evidence,strlen(evidence))||!(version=json_string(&j,json_get(&j,0,"version")))||strcmp(version,hash))error="stale_observation";free(version);}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
        const char *before=getenv("SAMOSA_REINDEX_TEST_PRECOMMIT_MARKER"),*release=getenv("SAMOSA_REINDEX_TEST_PRECOMMIT_RELEASE");
        if(!error&&before&&release){FILE *ready=fopen(before,"w");if(!ready)_exit(98);fputs("captured",ready);fclose(ready);int released=0;for(int waiting=0;waiting<500;waiting++){if(!access(release,F_OK)){released=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}if(!released)_exit(99);}
#endif
        if(!error)error=audio?(visual?commit_visual_version(index,job,(size_t)ordinal,revision,scope,path,identity,audio_source,audio_duration,admission_error):commit_audio_version(index,job,(size_t)ordinal,revision,scope,path,identity,audio_source,audio_duration,admission_error)):commit_version(index,job,(size_t)ordinal,revision,scope,path,identity,hash,literal,evidence,coverage);
        if(!error)processed++;
        if(error&&deferred_admitted){const char *finished=finish_defer(index,job,ordinal,identity,hash);if(finished)error=finished;}
        /* Keep dynamically owned failure text alive through the final error response. */
        if(capture_error){if(error==capture_error){result=strdup(capture_error);error=result?result:"resource_limit";}free(capture_error);}
        free(path);free(literal);free(evidence);free(coverage);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
        const char *marker=getenv("SAMOSA_REINDEX_TEST_COMMITTED_MARKER");if(!error&&marker){FILE *ready=fopen(marker,"w");if(!ready)_exit(98);fputs("committed",ready);fclose(ready);for(;;)pause();}
#endif
    }
    if(!error){size_t n;FILE *stream=open_memstream(&result,&n);if(!stream)error="resource_limit";else {fprintf(stream,"{\"contract_version\":1,\"job_id\":\"%s\",\"reindexed_items\":%zu,\"semantics\":\"retained_immutable_index_versions\"}",job,processed);if(fclose(stream))error="resource_limit";else if(!automatic)error=finish_operation(index,operation,result);}}
    done: ;int rc;if(error)rc=fail(out,error);else if(automatic){fputs(result,out);fputc('\n',out);rc=0;}else rc=operation_reply(index,operation,out);free(job);free(operation);free(encoded);free(scope);free(result);return rc;
}
static int match_ascii(const char *text_value,const char *query){size_t length=strlen(query);for(const char *p=text_value;*p;p++){size_t i=0;while(i<length&&p[i]&&tolower((unsigned char)p[i])==tolower((unsigned char)query[i]))i++;if(i==length)return 1;}return 0;}
static int search(Index *index,const Json *request,FILE *out){
    char *job=json_string(request,json_get(request,0,"job_id")),*query=json_string(request,json_get(request,0,"query"));if(!valid(job,64)||strlen(job)!=64||!query||!*query||strlen(query)>128){free(job);free(query);return fail(out,"invalid_request");}
    char *summary=buffered_job(index,job);if(!summary){free(job);free(query);return fail(out,"unknown_index");}
    sqlite3_stmt *scope_stmt=NULL;char *scope=NULL;
    if(!prepare(index,&scope_stmt,"SELECT scope FROM index_root_scopes WHERE job=?")){free(summary);free(job);free(query);return fail(out,"store_failed");}
    text(scope_stmt,1,job);int scope_rc=sqlite3_step(scope_stmt);
    if(scope_rc==SQLITE_ROW)scope=column(scope_stmt,0);
    sqlite3_finalize(scope_stmt);
    if((scope_rc!=SQLITE_ROW&&scope_rc!=SQLITE_DONE)||(scope_rc==SQLITE_ROW&&(!scope||strlen(scope)!=64))){free(scope);free(summary);free(job);free(query);return fail(out,"corrupt_index");}
    fprintf(out,"{\"contract_version\":1,\"index\":%s,\"match_policy\":\"literal_ascii_case_insensitive_substring\",\"exhaustive_current_filesystem\":false,\"candidates\":[",summary);free(summary);sqlite3_stmt *stmt=NULL;const char *error=NULL;size_t count=0;int truncated=0;
    if(!prepare(index,&stmt,"SELECT locator,identity,evidence,text,digest FROM (SELECT ordinal,0 AS generation,locator,identity,evidence,text,digest FROM index_items WHERE job=? AND status='indexed' UNION ALL SELECT v.ordinal,v.generation,i.locator,v.identity,v.evidence,v.text,v.digest FROM index_versions v JOIN index_items i ON i.job=v.job AND i.ordinal=v.ordinal WHERE v.job=?) ORDER BY ordinal,generation LIMIT 512"))error="store_failed";
    else {text(stmt,1,job);text(stmt,2,job);}
    while(!error){int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *literal=(const char*)sqlite3_column_text(stmt,3);if(!literal){error="corrupt_index";break;}
        const char *evidence=(const char*)sqlite3_column_text(stmt,2),*stored_digest=(const char*)sqlite3_column_text(stmt,4);char actual_digest[65];if(evidence)hash_bytes(evidence,strlen(evidence),actual_digest);Json j;if(!evidence||!stored_digest||strcmp(actual_digest,stored_digest)||!json_parse(&j,evidence,strlen(evidence))){error="corrupt_index";break;}const char *locator=(const char*)sqlite3_column_text(stmt,0),*identity=(const char*)sqlite3_column_text(stmt,1);
        char *kind=json_string(&j,json_get(&j,0,"kind")),*captured_text=source_index_literal(evidence,strlen(evidence));size_t contract=0;
        int qualified=kind&&!strcmp(kind,"literal")&&captured_text&&!strcmp(captured_text,literal)&&json_uint(&j,json_get(&j,0,"contract_version"),&contract)&&contract==1&&locator&&identity;
        free(kind);free(captured_text);if(!qualified){error="corrupt_index";break;}
        if(!match_ascii(literal,query))continue;
        if(count>=10){truncated=1;break;}
        if(count++)fputc(',',out);
        fputs("{\"locator\":",out);json_quote(out,locator,strlen(locator));fputs(",\"file_identity\":",out);json_quote(out,identity,strlen(identity));
        const char *keys[]={"source_id","version","evidence_id","range","coverage","provenance"};for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++){int token=json_get(&j,0,keys[i]);if(token<0){error="corrupt_index";break;}fputc(',',out);json_quote(out,keys[i],strlen(keys[i]));fputc(':',out);raw(out,&j,token);}
        char *version=json_string(&j,json_get(&j,0,"version"));
        fputs(",\"live_observation\":",out);
        if(!version){error="corrupt_index";break;}
        if(scope)error=source_locator_freshness(index->cfg,index->db,scope,locator,version,out);
        else fputs("{\"status\":\"unobserved\",\"semantics\":\"committed_observation_history\"}",out);
        free(version);if(error)break;
        size_t snippet=strlen(literal);if(snippet>512){snippet=512;while(snippet&&((unsigned char)literal[snippet]&192)==128)snippet--;}fputs(",\"literal_preview\":",out);json_quote(out,literal,snippet);fputc('}',out);
    }
    sqlite3_finalize(stmt);fprintf(out,"],\"returned_candidates\":%zu,\"candidate_coverage\":\"%s\"}\n",count,truncated?"partial":"complete_for_indexed_snapshot");free(scope);free(job);free(query);return error?1:0;
}
static int pending(Index *index,const Json *request,FILE *out){
    char *job=json_string(request,json_get(request,0,"job_id"));
    if(!valid(job,64)||strlen(job)!=64){free(job);return fail(out,"invalid_request");}
    sqlite3_stmt *authorized=NULL;if(!prepare(index,&authorized,"SELECT 1 FROM index_jobs WHERE id=? AND owner=?")){free(job);return fail(out,"store_failed");}text(authorized,1,job);text(authorized,2,index->owner);int authorized_rc=sqlite3_step(authorized);sqlite3_finalize(authorized);if(authorized_rc!=SQLITE_ROW){free(job);return fail(out,authorized_rc==SQLITE_DONE?"unknown_index":"store_failed");}
    sqlite3_stmt *stmt=NULL;const char *error=NULL;size_t count=0;
    if(!prepare(index,&stmt,"SELECT i.ordinal,i.locator,coalesce(v.evidence,i.evidence),coalesce(v.digest,i.digest),p.revision,s.scope,coalesce(a.attempts,0),coalesce(b.next_ms,0),coalesce(b.failures,0),coalesce(q.source,z.source) FROM index_pending p JOIN index_items i ON i.job=p.job AND i.ordinal=p.ordinal LEFT JOIN index_audio q ON q.job=i.job AND q.ordinal=i.ordinal LEFT JOIN index_visual z ON z.job=i.job AND z.ordinal=i.ordinal LEFT JOIN index_versions v ON v.job=i.job AND v.ordinal=i.ordinal AND v.generation=(SELECT max(generation) FROM index_versions WHERE job=i.job AND ordinal=i.ordinal) JOIN index_root_scopes s ON s.job=p.job LEFT JOIN index_attempts a ON a.job=p.job AND a.ordinal=p.ordinal LEFT JOIN index_retry b ON b.job=p.job AND b.ordinal=p.ordinal WHERE p.job=? ORDER BY i.ordinal LIMIT 257"))error="store_failed";
    else text(stmt,1,job);
    fprintf(out,"{\"contract_version\":1,\"job_id\":\"%s\",\"work_semantics\":\"committed_observation_reindex_pending\",\"pending\":[",job);
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        if(count>=256){error="corrupt_index";break;}
        const char *path=(const char*)sqlite3_column_text(stmt,1),*evidence=(const char*)sqlite3_column_text(stmt,2),*digest=(const char*)sqlite3_column_text(stmt,3),*scope=(const char*)sqlite3_column_text(stmt,5);Json j;char *version=indexed_version(evidence,digest,(const char*)sqlite3_column_text(stmt,9));
        sqlite3_int64 ordinal=sqlite3_column_int64(stmt,0),revision=sqlite3_column_int64(stmt,4);
        if(!path||!scope||!version||ordinal<0||ordinal>=256||revision<1||sqlite3_column_type(stmt,6)!=SQLITE_INTEGER||sqlite3_column_int64(stmt,6)<0||sqlite3_column_type(stmt,7)!=SQLITE_INTEGER||sqlite3_column_int64(stmt,7)<0||sqlite3_column_type(stmt,8)!=SQLITE_INTEGER||sqlite3_column_int(stmt,8)<0||sqlite3_column_int(stmt,8)>7)error="corrupt_index";
        char *observed=NULL;size_t n;FILE *stream=NULL;
        if(!error){stream=open_memstream(&observed,&n);if(!stream)error="resource_limit";else {error=source_locator_freshness(index->cfg,index->db,scope,path,version,stream);if(fclose(stream)&&!error)error="resource_limit";}}
        size_t recorded=0;char *status=NULL;
        if(!error&&(!json_parse(&j,observed,n)||!json_uint(&j,json_get(&j,0,"locator_revision"),&recorded)||recorded!=(size_t)revision||!(status=json_string(&j,json_get(&j,0,"status")))||!strcmp(status,"available")))error="corrupt_index";
        if(!error){if(count++)fputc(',',out);fprintf(out,"{\"ordinal\":%lld,\"locator\":",(long long)ordinal);json_quote(out,path,strlen(path));fprintf(out,",\"indexed_version\":\"%s\",\"attempts\":%lld,\"next_attempt_at_ms\":%lld,\"backoff_level\":%d,\"live_observation\":%s}",version,(long long)sqlite3_column_int64(stmt,6),(long long)sqlite3_column_int64(stmt,7),sqlite3_column_int(stmt,8),observed);}
        free(status);free(observed);free(version);
    }
    sqlite3_finalize(stmt);fprintf(out,"],\"pending_items\":%zu}\n",count);free(job);return error?fail(out,error):0;
}
/* The writer lease spans selection, reservation and work. Checkpoints are per job,
 * not operation receipts, so repeated background scans consume no receipt quota. */
static const char *refresh_schedule(Index *index,int initial,sqlite3_int64 *turn){
    sqlite3_stmt *stmt=NULL;*turn=1;
    if(!prepare(index,&stmt,"SELECT job,scan_next,scan_failures,scan_turn,initial_next,initial_failures,initial_turn FROM index_refresh_schedule"))return "store_failed";
    const char *error=NULL;size_t rows=0;
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;
        if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *job=(const char*)sqlite3_column_text(stmt,0);
        if(++rows>8||!valid(job,64)||strlen(job)!=64){error="corrupt_index";break;}
        for(int c=1;c<7;c++){
            sqlite3_int64 value=sqlite3_column_int64(stmt,c);
            if(sqlite3_column_type(stmt,c)!=SQLITE_INTEGER||value<0||((c==2||c==5)&&value>7)){error="corrupt_index";break;}
        }
        sqlite3_int64 saved=sqlite3_column_int64(stmt,initial?6:3);
        if(saved==INT64_MAX)error="scheduler_limit";
        else if(saved>=*turn)*turn=saved+1;
    }
    sqlite3_finalize(stmt);return error;
}
static const char *refresh_reserve(Index *index,const char *job,int initial,sqlite3_int64 turn,sqlite3_int64 next){
    sqlite3_stmt *stmt=NULL;
    const char *sql=initial?
        "INSERT INTO index_refresh_schedule(job,initial_next,initial_turn) VALUES(?,?,?) ON CONFLICT(job) DO UPDATE SET initial_next=excluded.initial_next,initial_turn=excluded.initial_turn":
        "INSERT INTO index_refresh_schedule(job,scan_next,scan_turn) VALUES(?,?,?) ON CONFLICT(job) DO UPDATE SET scan_next=excluded.scan_next,scan_turn=excluded.scan_turn";
    if(!prepare(index,&stmt,sql))return "store_failed";
    text(stmt,1,job);sqlite3_bind_int64(stmt,2,next);sqlite3_bind_int64(stmt,3,turn);
    int rc=sqlite3_step(stmt);sqlite3_finalize(stmt);return rc==SQLITE_DONE?NULL:"store_failed";
}
static const char *refresh_finish(Index *index,const char *job,int initial,int failures,sqlite3_int64 next){
    sqlite3_stmt *stmt=NULL;
    if(!prepare(index,&stmt,initial?"UPDATE index_refresh_schedule SET initial_next=?,initial_failures=? WHERE job=?":"UPDATE index_refresh_schedule SET scan_next=?,scan_failures=? WHERE job=?"))return "store_failed";
    sqlite3_bind_int64(stmt,1,next);sqlite3_bind_int(stmt,2,failures);text(stmt,3,job);
    int rc=sqlite3_step(stmt),changed=sqlite3_changes(index->db);sqlite3_finalize(stmt);
    return rc!=SQLITE_DONE?"store_failed":changed!=1?"corrupt_index":NULL;
}
static int refresh_recoverable(const char *error){
    return error&&(!strcmp(error,"stale_scope")||!strcmp(error,"source_changed")||!strcmp(error,"unauthorized_path")||!strcmp(error,"index_limit")||!strcmp(error,"scope_unavailable")||!strcmp(error,"live_unavailable")||!strcmp(error,"read_failed")||!strcmp(error,"stale_source"));
}
static const char *refresh_phase(Index *index,int initial,size_t *processed,char **failure){
    *processed=0;*failure=NULL;
    sqlite3_int64 now=0,turn=1;const char *error=retry_clock(&now);
    if(!error)error=sources_interrupted(index->cfg);
    if(!error)error=refresh_schedule(index,initial,&turn);
    char scope[65];if(!error)error=inventory_scope_identity(index->cfg,"",scope);
    sqlite3_stmt *stmt=NULL;char *job=NULL;int failures=0;
    const char *sql=initial?
        "SELECT j.id,coalesce(r.initial_failures,0) FROM index_jobs j JOIN index_root_scopes s ON s.job=j.id LEFT JOIN index_refresh_schedule r ON r.job=j.id WHERE j.owner=? AND s.scope=? AND j.cursor<j.total AND (r.initial_next IS NULL OR r.initial_next<=? OR r.initial_next>?) ORDER BY coalesce(r.initial_turn,0),j.id LIMIT 1":
        "SELECT j.id,coalesce(r.scan_failures,0) FROM index_jobs j JOIN index_root_scopes s ON s.job=j.id LEFT JOIN index_refresh_schedule r ON r.job=j.id WHERE j.owner=? AND s.scope=? AND (r.scan_next IS NULL OR r.scan_next<=? OR r.scan_next>?) ORDER BY coalesce(r.scan_turn,0),j.id LIMIT 1";
    if(!error&&!prepare(index,&stmt,sql))error="store_failed";
    if(!error){
        text(stmt,1,index->owner);text(stmt,2,scope);sqlite3_bind_int64(stmt,3,now);sqlite3_bind_int64(stmt,4,now+60000);
        int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){job=column(stmt,0);failures=sqlite3_column_int(stmt,1);if(!valid(job,64)||strlen(job)!=64)error="corrupt_index";}
        else if(rc!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);
    if(!error&&job)error=refresh_reserve(index,job,initial,turn,now+(initial?1000:(sqlite3_int64)index->cfg->discovery_interval_ms));
    char *body=NULL;size_t length=0;
    if(!error&&job){
        char input[128];int n=snprintf(input,sizeof(input),"{\"job_id\":\"%s\"}",job);Json request;
        if(n<0||(size_t)n>=sizeof(input)||!json_parse(&request,input,(size_t)n))error="corrupt_index";
        else {
            FILE *out=open_memstream(&body,&length);
            if(!out)error="resource_limit";
            else {
                int rc=initial?step(index,&request,out,1):discover(index,&request,out,1);
                if(fclose(out))error="resource_limit";
                else if(rc){
                    Json response;
                    if(!body||!json_parse(&response,body,length)||!(*failure=json_string(&response,json_get(&response,json_get(&response,0,"error"),"code"))))error="corrupt_index";
                    else if(!refresh_recoverable(*failure))error=*failure;
                }else *processed=1;
            }
        }
        sqlite3_int64 finished=0;
        if(!error)error=retry_clock(&finished);
        if(!error){
            int level=*failure?(failures<7?failures+1:7):0;
            sqlite3_int64 delay=level?(level==7?60000:((sqlite3_int64)1000<<(level-1))):(initial?0:(sqlite3_int64)index->cfg->discovery_interval_ms);
            error=refresh_finish(index,job,initial,level,finished+delay);
        }
    }
    free(body);free(job);return error;
}
static const char *recovery_scope(Index *index,const char *job){
    const char *error=discovery_root(index,job);sqlite3_stmt *stmt=NULL;
    if(!error&&!prepare(index,&stmt,"SELECT path,manifest FROM index_jobs WHERE id=? AND owner=?"))error="store_failed";
    if(!error){
        text(stmt,1,job);text(stmt,2,index->owner);
        if(sqlite3_step(stmt)!=SQLITE_ROW)error="corrupt_index";
        else {
            const char *path=(const char*)sqlite3_column_text(stmt,0),*manifest=(const char*)sqlite3_column_text(stmt,1);Json j;char *expected=NULL,actual[65];
            if(!path||!manifest||!json_parse(&j,manifest,strlen(manifest))||!(expected=json_string(&j,json_get(&j,0,"scope_identity"))))error="corrupt_index";
            if(!error)error=inventory_scope_identity(index->cfg,path,actual);
            if(!error&&strcmp(expected,actual))error="stale_scope";
            free(expected);
        }
    }
    sqlite3_finalize(stmt);return error;
}
static const char *recovery_step(Index *index,size_t *recovered,char **failure){
    *recovered=0;*failure=NULL;
    sqlite3_int64 now=0,turn=1;const char *error=retry_clock(&now);sqlite3_stmt *stmt=NULL;
    if(!error)error=sources_interrupted(index->cfg);
    if(!error&&!prepare(index,&stmt,"SELECT ordinal,identity,next_ms,failures,turn FROM index_recovery"))error="store_failed";
    size_t rows=0;
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *identity=(const char*)sqlite3_column_text(stmt,1);sqlite3_int64 ordinal=sqlite3_column_int64(stmt,0),next=sqlite3_column_int64(stmt,2),level=sqlite3_column_int64(stmt,3),saved=sqlite3_column_int64(stmt,4);
        if(++rows>2048||sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||ordinal<0||ordinal>=256||!valid(identity,64)||strlen(identity)!=64||sqlite3_column_type(stmt,2)!=SQLITE_INTEGER||next<0||sqlite3_column_type(stmt,3)!=SQLITE_INTEGER||level<0||level>7||sqlite3_column_type(stmt,4)!=SQLITE_INTEGER||saved<0)error="corrupt_index";
        else if(saved==INT64_MAX)error="scheduler_limit";else if(saved>=turn)turn=saved+1;
    }
    sqlite3_finalize(stmt);stmt=NULL;char scope[65];
    if(!error)error=inventory_scope_identity(index->cfg,"",scope);
    char *job=NULL,*path=NULL,*identity=NULL;sqlite3_int64 ordinal=0;int level=0;
    if(!error&&!prepare(index,&stmt,"SELECT r.job,r.ordinal,r.identity,i.locator,r.failures FROM index_recovery r JOIN index_items i ON i.job=r.job AND i.ordinal=r.ordinal JOIN index_jobs j ON j.id=r.job JOIN index_root_scopes s ON s.job=j.id WHERE j.owner=? AND s.scope=? AND i.status IN ('failed','skipped') AND (r.next_ms<=? OR r.next_ms>?) ORDER BY r.turn,r.job,r.ordinal LIMIT 1"))error="store_failed";
    if(!error){
        text(stmt,1,index->owner);text(stmt,2,scope);sqlite3_bind_int64(stmt,3,now);sqlite3_bind_int64(stmt,4,now+60000);
        int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){job=column(stmt,0);ordinal=sqlite3_column_int64(stmt,1);identity=column(stmt,2);path=column(stmt,3);level=sqlite3_column_int(stmt,4);if(!valid(job,64)||strlen(job)!=64||!path||!identity)error="corrupt_index";}
        else if(rc!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&job){
        level=level<7?level+1:7;sqlite3_int64 delay=level==7?60000:((sqlite3_int64)1000<<(level-1));
        if(!prepare(index,&stmt,"UPDATE index_recovery SET next_ms=?,failures=?,turn=? WHERE job=? AND ordinal=? AND identity=?"))error="store_failed";
        else {sqlite3_bind_int64(stmt,1,now+delay);sqlite3_bind_int(stmt,2,level);sqlite3_bind_int64(stmt,3,turn);text(stmt,4,job);sqlite3_bind_int64(stmt,5,ordinal);text(stmt,6,identity);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    char *literal=NULL,*evidence=NULL,*coverage=NULL;
    if(!error&&job){
        const char *binding=recovery_scope(index,job);
        if(binding)*failure=strdup(binding);
        else {char *captured=capture(index,path,identity,&literal,&coverage);if(literal)evidence=captured;else *failure=captured;}
        if(!literal&&!*failure)error="resource_limit";
        if(*failure&&(!strcmp(*failure,"cancelled")||!strcmp(*failure,"deadline_exceeded")))error=*failure;
        if(!error&&literal){
            if(!execsql(index,"BEGIN IMMEDIATE"))error="store_busy";
            else {
                error=recovery_scope(index,job);
                sqlite3_int64 usage=scalar(index,"SELECT coalesce(sum(length(CAST(text AS BLOB))+length(CAST(evidence AS BLOB))),0) FROM (SELECT text,evidence FROM index_items UNION ALL SELECT text,evidence FROM index_versions)");size_t extra=strlen(literal)+strlen(evidence);
                if(!error&&usage<0)error="store_failed";
                if(!error&&((size_t)usage>INDEX_BYTES_MAX||extra>INDEX_BYTES_MAX-(size_t)usage))error="index_limit";
                if(!error&&!prepare(index,&stmt,"UPDATE index_items SET status='indexed',identity=?,eligible=1,text=?,evidence=?,coverage=?,digest=?,error=NULL WHERE job=? AND ordinal=? AND status IN ('failed','skipped') AND EXISTS(SELECT 1 FROM index_recovery WHERE job=? AND ordinal=? AND identity=?)"))error="store_failed";
                if(!error){char digest[65];hash_bytes(evidence,strlen(evidence),digest);text(stmt,1,identity);text(stmt,2,literal);text(stmt,3,evidence);text(stmt,4,coverage);text(stmt,5,digest);text(stmt,6,job);sqlite3_bind_int64(stmt,7,ordinal);text(stmt,8,job);sqlite3_bind_int64(stmt,9,ordinal);text(stmt,10,identity);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="stale_observation";}
                sqlite3_finalize(stmt);stmt=NULL;
                if(!error&&!prepare(index,&stmt,"DELETE FROM index_recovery WHERE job=? AND ordinal=? AND identity=?"))error="store_failed";
                if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,ordinal);text(stmt,3,identity);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="stale_observation";}
                sqlite3_finalize(stmt);stmt=NULL;
                if(!error&&!prepare(index,&stmt,"INSERT INTO index_repaired VALUES(?,?)"))error="store_failed";
                if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
                sqlite3_finalize(stmt);stmt=NULL;
#ifdef SAMOSA_SOURCES_TEST_FAULTS
                const char *marker=getenv("SAMOSA_RECOVERY_TEST_PRECOMMIT_MARKER"),*release=getenv("SAMOSA_RECOVERY_TEST_PRECOMMIT_RELEASE");
                if(!error&&marker&&release){
                    FILE *ready=fopen(marker,"w");if(!ready)_exit(98);
                    fputs("prepared",ready);if(fclose(ready))_exit(98);
                    int released=0;
                    for(int waiting=0;waiting<500;waiting++){
                        if(!access(release,F_OK)){released=1;break;}
                        struct timespec delay={0,10000000};nanosleep(&delay,NULL);
                    }
                    if(!released)_exit(99);
                }
#endif
                if(!error)error=sources_interrupted(index->cfg);
                if(!error)error=recovery_scope(index,job);
                if(error)execsql(index,"ROLLBACK");else if(!execsql(index,"COMMIT")){execsql(index,"ROLLBACK");error="store_failed";}else *recovered=1;
            }
        }
    }
    if(error&&refresh_recoverable(error)){
        free(*failure);*failure=strdup(error);error=*failure?NULL:"resource_limit";
    }
    free(job);free(path);free(identity);free(literal);free(evidence);free(coverage);return error;
}
static const char *freshness_step(Index *index,size_t *observed,char **failure){
    *observed=0;*failure=NULL;sqlite3_int64 now=0,turn=1;const char *error=retry_clock(&now);sqlite3_stmt *stmt=NULL;
    if(!error)error=sources_interrupted(index->cfg);
    if(!error&&!prepare(index,&stmt,"SELECT job,ordinal,next_ms,turn FROM index_freshness_schedule"))error="store_failed";
    size_t rows=0;
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        const char *job=(const char*)sqlite3_column_text(stmt,0);sqlite3_int64 ordinal=sqlite3_column_int64(stmt,1),next=sqlite3_column_int64(stmt,2),saved=sqlite3_column_int64(stmt,3);
        if(++rows>8||!valid(job,64)||strlen(job)!=64||sqlite3_column_type(stmt,1)!=SQLITE_INTEGER||ordinal<0||ordinal>=256||sqlite3_column_type(stmt,2)!=SQLITE_INTEGER||next<0||sqlite3_column_type(stmt,3)!=SQLITE_INTEGER||saved<0)error="corrupt_index";
        else if(saved==INT64_MAX)error="scheduler_limit";else if(saved>=turn)turn=saved+1;
    }
    sqlite3_finalize(stmt);stmt=NULL;char scope[65];
    if(!error)error=inventory_scope_identity(index->cfg,"",scope);
    char *job=NULL;sqlite3_int64 last=-1,ordinal=0;
    if(!error&&!prepare(index,&stmt,"SELECT j.id,coalesce(f.ordinal,-1) FROM index_jobs j JOIN index_root_scopes s ON s.job=j.id LEFT JOIN index_freshness_schedule f ON f.job=j.id WHERE j.owner=? AND s.scope=? AND EXISTS(SELECT 1 FROM index_items WHERE job=j.id AND status IN ('indexed','queued_audio','queued_visual')) AND (f.next_ms IS NULL OR f.next_ms<=? OR f.next_ms>?) ORDER BY coalesce(f.turn,0),j.id LIMIT 1"))error="store_failed";
    if(!error){text(stmt,1,index->owner);text(stmt,2,scope);sqlite3_bind_int64(stmt,3,now);sqlite3_bind_int64(stmt,4,now+60000);int rc=sqlite3_step(stmt);if(rc==SQLITE_ROW){job=column(stmt,0);last=sqlite3_column_int64(stmt,1);if(!valid(job,64)||strlen(job)!=64)error="corrupt_index";}else if(rc!=SQLITE_DONE)error="store_failed";}
    sqlite3_finalize(stmt);stmt=NULL;char *path=NULL,*version=NULL;
    if(!error&&job&&!prepare(index,&stmt,"SELECT i.ordinal,i.locator,coalesce(v.evidence,i.evidence),coalesce(v.digest,i.digest),coalesce(q.source,z.source) FROM index_items i LEFT JOIN index_audio q ON q.job=i.job AND q.ordinal=i.ordinal LEFT JOIN index_visual z ON z.job=i.job AND z.ordinal=i.ordinal LEFT JOIN index_versions v ON v.job=i.job AND v.ordinal=i.ordinal AND v.generation=(SELECT max(generation) FROM index_versions WHERE job=i.job AND ordinal=i.ordinal) WHERE i.job=? AND i.status IN ('indexed','queued_audio','queued_visual') ORDER BY CASE WHEN i.ordinal>? THEN 0 ELSE 1 END,i.ordinal LIMIT 1"))error="store_failed";
    if(!error&&job){
        text(stmt,1,job);sqlite3_bind_int64(stmt,2,last);
        if(sqlite3_step(stmt)!=SQLITE_ROW)error="corrupt_index";
        else {
            ordinal=sqlite3_column_int64(stmt,0);path=column(stmt,1);const char *evidence=(const char*)sqlite3_column_text(stmt,2),*digest=(const char*)sqlite3_column_text(stmt,3);
            version=indexed_version(evidence,digest,(const char*)sqlite3_column_text(stmt,4));
            if(!path||ordinal<0||ordinal>=256||!version)error="corrupt_index";
        }
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&job){
        if(!prepare(index,&stmt,"INSERT INTO index_freshness_schedule VALUES(?,?,?,?) ON CONFLICT(job) DO UPDATE SET ordinal=excluded.ordinal,next_ms=excluded.next_ms,turn=excluded.turn"))error="store_failed";
        else {text(stmt,1,job);sqlite3_bind_int64(stmt,2,ordinal);sqlite3_bind_int64(stmt,3,now+(sqlite3_int64)index->cfg->discovery_interval_ms);sqlite3_bind_int64(stmt,4,turn);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
        if(!error){
            error=recovery_scope(index,job);
            if(!error)error=source_locator_observe_index(index->cfg,index->db,index->directory,scope,path,version);
            if(!error)*observed=1;
            else if(refresh_recoverable(error)||!strcmp(error,"busy")||!strcmp(error,"locator_limit")){
                *failure=strdup(error);error=*failure?NULL:"resource_limit";
            }
        }
    }
    free(job);free(path);free(version);return error;
}
static int refresh(Index *index,FILE *out){
    size_t interval=index->cfg->discovery_interval_ms;
    if(!interval)return fail(out,"discovery_disabled");
    if(interval<100||interval>60000)return fail(out,"invalid_configuration");
    const char *error=lock_index(index);
    if(!error)error=recover(index);
    size_t scanned=0,processed=0,recovered=0,observed=0;char *scan_error=NULL,*initial_error=NULL,*recovery_error=NULL,*freshness_error=NULL;
    if(!error)error=refresh_phase(index,0,&scanned,&scan_error);
    if(!error)error=refresh_phase(index,1,&processed,&initial_error);
    if(!error)error=recovery_step(index,&recovered,&recovery_error);
    if(!error)error=freshness_step(index,&observed,&freshness_error);
    int rc=0;
    if(error)rc=fail(out,error);
    else {
        fprintf(out,"{\"contract_version\":1,\"scanned_jobs\":%zu,\"processed_items\":%zu,\"discovery_error\":",scanned,processed);
        if(scan_error)json_quote(out,scan_error,strlen(scan_error));else fputs("null",out);
        fputs(",\"initial_error\":",out);
        if(initial_error)json_quote(out,initial_error,strlen(initial_error));else fputs("null",out);
        fprintf(out,",\"recovered_items\":%zu,\"recovery_error\":",recovered);
        if(recovery_error)json_quote(out,recovery_error,strlen(recovery_error));else fputs("null",out);
        fprintf(out,",\"observed_items\":%zu,\"freshness_error\":",observed);
        if(freshness_error)json_quote(out,freshness_error,strlen(freshness_error));else fputs("null",out);
        fputs("}\n",out);
    }
    free(scan_error);free(initial_error);free(recovery_error);free(freshness_error);return rc;
}
int source_index_dispatch(const SourcesConfig *cfg,const char *input,size_t length,sqlite3 *db,int directory,FILE *out){
    Json request;if(!json_parse(&request,input,length))return fail(out,"invalid_json");char *op=json_string(&request,json_get(&request,0,"op"));if(!op)return fail(out,"invalid_request");Index index={.cfg=cfg,.db=db,.owner=cfg->owner?cfg->owner:"local-owner",.directory=directory,.lock=-1};const char *error=valid(index.owner,128)?schema(&index):"invalid_configuration";
    char *result=NULL;size_t n=0;FILE *stream=NULL;int rc=1;if(!error){stream=open_memstream(&result,&n);if(!stream)error="resource_limit";}
    if(!error){if(!strcmp(op,"index_embed_step")||!strcmp(op,"index_embed_queue_step")||!strcmp(op,"index_embedding_status"))rc=sources_index_embed(cfg,&request,db,directory,stream);else if(!strcmp(op,"index_enrich_step")||!strcmp(op,"index_enrich_queue_step")||!strcmp(op,"index_enrichment_status"))rc=sources_index_enrich(cfg,&request,db,directory,stream);else if(!strcmp(op,"index_publish_step")||!strcmp(op,"index_publish_queue_step")||!strcmp(op,"index_publication_status")){error=lock_index(&index);if(error&&!strcmp(error,"busy")&&!strcmp(op,"index_publication_status"))error=NULL;if(!error)rc=source_index_publish(cfg,input,length,db,stream);}else if(!strcmp(op,"index_refresh_step"))rc=refresh(&index,stream);else if(!strcmp(op,"index_create"))rc=create(&index,&request,stream);else if(!strcmp(op,"index_discover"))rc=discover(&index,&request,stream,0);else if(!strcmp(op,"index_step"))rc=step(&index,&request,stream,0);else if(!strcmp(op,"index_reindex_step"))rc=reindex(&index,&request,stream,0);else if(!strcmp(op,"index_queue_step"))rc=reindex(&index,&request,stream,1);else {
            const char *locked=lock_index(&index);if(!locked)error=recover(&index);else if(strcmp(locked,"busy"))error=locked;
            if(!error){if(!execsql(&index,"BEGIN"))error="store_busy";else {if(!strcmp(op,"index_visual_search"))rc=search_visual(&index,&request,stream);else if(!strcmp(op,"index_visual_read"))rc=retained_visual_read(&index,&request,stream);else if(!strcmp(op,"index_visual_history"))rc=retained_visual_history(&index,&request,stream);else if(!strcmp(op,"index_audio_search"))rc=search_audio(&index,&request,stream);else if(!strcmp(op,"index_search"))rc=search(&index,&request,stream);else if(!strcmp(op,"index_pending"))rc=pending(&index,&request,stream);else if(!strcmp(op,"index_audio_read"))rc=retained_audio_read(&index,&request,stream);else if(!strcmp(op,"index_audio_history"))rc=retained_audio_history(&index,&request,stream);else {char *job=json_string(&request,json_get(&request,0,"job_id"));rc=valid(job,64)&&strlen(job)==64?job_reply(&index,job,stream):fail(stream,"invalid_request");free(job);}if(!execsql(&index,rc?"ROLLBACK":"COMMIT")){execsql(&index,"ROLLBACK");error="store_failed";}}}
        }
        if(fclose(stream))error="resource_limit";
        stream=NULL;
    }
    if(index.lock>=0)close(index.lock);
    free(op);if(error){free(result);return fail(out,error);}if(n>524288){free(result);return fail(out,"response_too_large");}
    if(rc){Json response;if(!result||!json_parse(&response,result,n)||json_get(&response,0,"error")<0){free(result);return fail(out,"corrupt_index");}}
    fwrite(result,1,n,out);free(result);return rc;
}
