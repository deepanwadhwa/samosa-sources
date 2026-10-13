#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "chutni.h"
#include "index.h"
#include "native.h"
#include "inventory.h"
#include "control.h"
#include "json.h"
#include "sha256.h"
#include "transcript.h"
#include "summary.h"
#include "visual.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static int fail(FILE *out,const char *error){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",error);return 1;}
static int hex(const char *s){if(!s||strlen(s)!=64)return 0;for(size_t i=0;i<64;i++)if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static int uuid(const char *s){if(!s||strlen(s)!=36)return 0;for(size_t i=0;i<36;i++)if(i==8||i==13||i==18||i==23){if(s[i]!='-')return 0;}else if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static void text(sqlite3_stmt *s,int i,const char *v){sqlite3_bind_text(s,i,v,-1,SQLITE_TRANSIENT);}
static char *column(sqlite3_stmt *s,int i){const char *v=(const char *)sqlite3_column_text(s,i);return v?strdup(v):NULL;}
static int prepare(sqlite3 *db,sqlite3_stmt **s,const char *sql){return sqlite3_prepare_v2(db,sql,-1,s,NULL)==SQLITE_OK;}
static int sql(sqlite3 *db,const char *command){return sqlite3_exec(db,command,NULL,NULL,NULL)==SQLITE_OK;}
static sqlite3_int64 now_ms(void){struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t)||t.tv_sec<0||t.tv_sec>INT64_MAX/1000-1)return -1;return (sqlite3_int64)t.tv_sec*1000+t.tv_nsec/1000000;}
static int field(const Json *j,int object,const char *key,const char *expected){char *s=json_string(j,json_get(j,object,key));int good=s&&expected&&!strcmp(s,expected);free(s);return good;}
static const char *schema(sqlite3 *db){return sql(db,"CREATE TABLE IF NOT EXISTS index_publications(job TEXT NOT NULL,ordinal INTEGER NOT NULL,generation INTEGER NOT NULL,store_id TEXT NOT NULL,version TEXT NOT NULL,evidence_id TEXT NOT NULL,digest TEXT NOT NULL,publication_id TEXT NOT NULL,status TEXT NOT NULL,response TEXT,response_digest TEXT,next_ms INTEGER NOT NULL DEFAULT 0,failures INTEGER NOT NULL DEFAULT 0,turn INTEGER NOT NULL,last_error TEXT,PRIMARY KEY(job,ordinal,generation,store_id));CREATE TABLE IF NOT EXISTS transcript_publications(key TEXT PRIMARY KEY,owner TEXT NOT NULL,response TEXT NOT NULL,digest TEXT NOT NULL)")?NULL:"store_failed";}
static const char *integrity(sqlite3 *db,sqlite3_int64 *turn){
    sqlite3_stmt *s=NULL;*turn=1;size_t rows=0;
    if(!prepare(db,&s,"SELECT job,ordinal,generation,store_id,version,evidence_id,digest,publication_id,status,response,response_digest,next_ms,failures,turn,last_error FROM index_publications"))return "store_failed";
    const char *error=NULL;
    while(!error){int rc=sqlite3_step(s);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}if(++rows>4096){error="corrupt_publication";break;}
        const char *job=(const char *)sqlite3_column_text(s,0),*store=(const char *)sqlite3_column_text(s,3),*version=(const char *)sqlite3_column_text(s,4),*evidence=(const char *)sqlite3_column_text(s,5),*digest=(const char *)sqlite3_column_text(s,6),*key=(const char *)sqlite3_column_text(s,7),*status=(const char *)sqlite3_column_text(s,8),*response=(const char *)sqlite3_column_text(s,9),*proof=(const char *)sqlite3_column_text(s,10);sqlite3_int64 ordinal=sqlite3_column_int64(s,1),generation=sqlite3_column_int64(s,2),next=sqlite3_column_int64(s,11),failures=sqlite3_column_int64(s,12),saved_turn=sqlite3_column_int64(s,13);
        if(!hex(job)||!uuid(store)||!hex(version)||!hex(evidence)||!hex(digest)||!hex(key)||!status||ordinal<0||ordinal>255||generation<0||generation>256||next<0||failures<0||failures>7||saved_turn<1||sqlite3_column_type(s,1)!=SQLITE_INTEGER||sqlite3_column_type(s,2)!=SQLITE_INTEGER||sqlite3_column_type(s,11)!=SQLITE_INTEGER||sqlite3_column_type(s,12)!=SQLITE_INTEGER||sqlite3_column_type(s,13)!=SQLITE_INTEGER){error="corrupt_publication";break;}
        if(saved_turn==INT64_MAX){error="scheduler_limit";break;}if(saved_turn>=*turn)*turn=saved_turn+1;
        int columns[]={0,3,4,5,6,7,8,9,10,14};for(size_t i=0;i<10;i++){int column=columns[i];if(sqlite3_column_type(s,column)==SQLITE_NULL){if(column!=9&&column!=10&&column!=14)error="corrupt_publication";continue;}const char *value=(const char *)sqlite3_column_text(s,column);if(sqlite3_column_type(s,column)!=SQLITE_TEXT||!value||strlen(value)!=(size_t)sqlite3_column_bytes(s,column))error="corrupt_publication";}if(error)break;
        const char *last_error=(const char *)sqlite3_column_text(s,14);if(last_error){if(!*last_error||strlen(last_error)>64)error="corrupt_publication";for(size_t i=0;last_error[i];i++)if(!strchr("abcdefghijklmnopqrstuvwxyz0123456789_",last_error[i]))error="corrupt_publication";}if(error)break;
        if(!strcmp(status,"committed")){
            Json j;char actual[65];if(response)hash_bytes(response,strlen(response),actual);size_t a=0,b=0;
            if(!response||strlen(response)>8192||!hex(proof)||strcmp(actual,proof)||!json_parse(&j,response,strlen(response))||!field(&j,0,"job_id",job)||!json_uint(&j,json_get(&j,0,"ordinal"),&a)||a!=(size_t)ordinal||!json_uint(&j,json_get(&j,0,"generation"),&b)||b!=(size_t)generation){error="corrupt_publication";break;}
            int item=json_get(&j,0,"publication");if(item<0){error="corrupt_publication";break;}
            char metadata[512];snprintf(metadata,sizeof(metadata),"{\"store_id\":\"%s\",\"publication_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"}",store,key,version,evidence);char *body=strndup(j.text+j.tokens[item].begin,j.tokens[item].end-j.tokens[item].begin);if(!body)error="resource_limit";else if(sources_chutni_publish_validate(metadata,body,strlen(body)))error="corrupt_publication";free(body);
        }else if(strcmp(status,"pending")||response||proof)error="corrupt_publication";
    }
    sqlite3_finalize(s);return error;
}
static const char *reserve(sqlite3 *db,const char *job,sqlite3_int64 ordinal,sqlite3_int64 generation,const char *store,const char *version,const char *evidence,const char *digest,const char *key,sqlite3_int64 time,sqlite3_int64 turn){
    if(!sql(db,"BEGIN IMMEDIATE"))return "store_busy";
    sqlite3_stmt *s=NULL;const char *error=NULL;
    if(!prepare(db,&s,"SELECT (SELECT count(*) FROM index_publications)+(SELECT count(*) FROM transcript_publications),EXISTS(SELECT 1 FROM index_publications WHERE job=? AND ordinal=? AND generation=? AND store_id=?)"))error="store_failed";
    else{text(s,1,job);sqlite3_bind_int64(s,2,ordinal);sqlite3_bind_int64(s,3,generation);text(s,4,store);if(sqlite3_step(s)!=SQLITE_ROW)error="store_failed";else if(sqlite3_column_int64(s,0)>=4096&&!sqlite3_column_int(s,1))error="publication_limit";}
    sqlite3_finalize(s);s=NULL;
    if(!error&&!prepare(db,&s,"INSERT INTO index_publications(job,ordinal,generation,store_id,version,evidence_id,digest,publication_id,status,next_ms,turn) VALUES(?,?,?,?,?,?,?,?,'pending',?,?) ON CONFLICT(job,ordinal,generation,store_id) DO UPDATE SET next_ms=excluded.next_ms,turn=excluded.turn WHERE status='pending' AND version=excluded.version AND evidence_id=excluded.evidence_id AND digest=excluded.digest AND publication_id=excluded.publication_id"))error="store_failed";
    if(!error){text(s,1,job);sqlite3_bind_int64(s,2,ordinal);sqlite3_bind_int64(s,3,generation);text(s,4,store);text(s,5,version);text(s,6,evidence);text(s,7,digest);text(s,8,key);sqlite3_bind_int64(s,9,time+1000);sqlite3_bind_int64(s,10,turn);if(sqlite3_step(s)!=SQLITE_DONE||sqlite3_changes(db)!=1)error="corrupt_publication";}
    sqlite3_finalize(s);if(error)sql(db,"ROLLBACK");else if(!sql(db,"COMMIT")){sql(db,"ROLLBACK");error="store_failed";}return error;
}
static const char *finish(sqlite3 *db,const char *job,sqlite3_int64 ordinal,sqlite3_int64 generation,const char *store,const char *key,const char *response,sqlite3_int64 time,int failures,const char *last_error){
    if(!sql(db,"BEGIN IMMEDIATE"))return "store_busy";
    sqlite3_stmt *s=NULL;const char *error=NULL;
    if(!prepare(db,&s,response?"UPDATE index_publications SET status='committed',response=?,response_digest=?,next_ms=0,failures=0,last_error=NULL WHERE job=? AND ordinal=? AND generation=? AND store_id=? AND publication_id=? AND status='pending'":"UPDATE index_publications SET next_ms=?,failures=?,last_error=? WHERE job=? AND ordinal=? AND generation=? AND store_id=? AND publication_id=? AND status='pending'"))error="store_failed";
    if(!error){int offset=3;if(response){char digest[65];hash_bytes(response,strlen(response),digest);text(s,1,response);text(s,2,digest);}else{sqlite3_int64 delay=1000ll<<failures;if(delay>60000)delay=60000;sqlite3_bind_int64(s,1,time+delay);sqlite3_bind_int(s,2,failures);text(s,3,last_error?last_error:"interrupted");offset=4;}text(s,offset,job);sqlite3_bind_int64(s,offset+1,ordinal);sqlite3_bind_int64(s,offset+2,generation);text(s,offset+3,store);text(s,offset+4,key);if(sqlite3_step(s)!=SQLITE_DONE||sqlite3_changes(db)!=1)error="corrupt_publication";}
    sqlite3_finalize(s);if(error)sql(db,"ROLLBACK");else if(!sql(db,"COMMIT")){sql(db,"ROLLBACK");error="store_failed";}return error;
}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
static const char *post_sdk(const SourcesConfig *cfg){const char *path=getenv("SAMOSA_CHUTNI_PUBLICATION_POST_SDK_MARKER");if(!path||!*path)return NULL;int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return "test_marker_failed";close(fd);while(!access(path,F_OK)){const char *error=sources_interrupted(cfg);if(error)return error;struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);}return NULL;}
#endif
static int status(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out){
    char *job=json_string(request,json_get(request,0,"job_id"));size_t after=0;int token=json_get(request,0,"after");if(!hex(job)||(token>=0&&(!json_uint(request,token,&after)||after>INT64_MAX))){free(job);return fail(out,"invalid_request");}
    const char *owner=cfg->owner?cfg->owner:"local-owner",*error=sources_interrupted(cfg);sqlite3_stmt *s=NULL;sqlite3_int64 turn;int transaction=0;
    if(!error)error=schema(db);
    if(!error){if(!sql(db,"BEGIN"))error="store_busy";else transaction=1;}
    if(!error)error=integrity(db,&turn);
    if(!error&&!prepare(db,&s,"SELECT 1 FROM index_jobs WHERE id=? AND owner=?"))error="store_failed";
    if(!error){text(s,1,job);text(s,2,owner);int rc=sqlite3_step(s);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index":"store_failed";}sqlite3_finalize(s);s=NULL;
    char *body=NULL;size_t n=0;FILE *f=NULL;size_t count=0,next=after;int more=0;
    if(!error){f=open_memstream(&body,&n);if(!f)error="resource_limit";else fprintf(f,"{\"contract_version\":1,\"job_id\":\"%s\",\"coverage\":\"publication_checkpoint_history\",\"publications\":[",job);}
    if(!error&&!prepare(db,&s,"SELECT rowid,ordinal,generation,store_id,version,evidence_id,publication_id,status,next_ms,failures,last_error,response FROM index_publications WHERE job=? AND rowid>? ORDER BY rowid LIMIT 17"))error="store_failed";
    if(!error){text(s,1,job);sqlite3_bind_int64(s,2,(sqlite3_int64)after);}
    while(!error){int rc=sqlite3_step(s);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}if(count==16){more=1;break;}if(count++)fputc(',',f);next=(size_t)sqlite3_column_int64(s,0);fprintf(f,"{\"ordinal\":%lld,\"generation\":%lld",(long long)sqlite3_column_int64(s,1),(long long)sqlite3_column_int64(s,2));const char *keys[]={"store_id","version","evidence_id","publication_id","state"};for(size_t i=0;i<5;i++){const char *value=(const char *)sqlite3_column_text(s,(int)i+3);fputc(',',f);json_quote(f,keys[i],strlen(keys[i]));fputc(':',f);json_quote(f,value,strlen(value));}fprintf(f,",\"retry_at_unix_ms\":%lld,\"failures\":%lld,\"last_error\":",(long long)sqlite3_column_int64(s,8),(long long)sqlite3_column_int64(s,9));const char *last=(const char *)sqlite3_column_text(s,10);if(last)json_quote(f,last,strlen(last));else fputs("null",f);fputs(",\"receipt\":",f);const char *receipt=(const char *)sqlite3_column_text(s,11);fputs(receipt?receipt:"null",f);fputc('}',f);error=sources_interrupted(cfg);}
    sqlite3_finalize(s);if(f){fprintf(f,"],\"next_after\":%zu,\"more\":%s}\n",next,more?"true":"false");if(fclose(f))error="resource_limit";}
    if(transaction){if(error)sql(db,"ROLLBACK");else if(!sql(db,"COMMIT")){sql(db,"ROLLBACK");error="store_failed";}}
    if(!error)fwrite(body,1,n,out);
    free(body);free(job);return error?fail(out,error):0;
}
int source_index_publish(const SourcesConfig *cfg,const char *input,size_t length,sqlite3 *db,FILE *out){
    Json check;if(json_parse(&check,input,length)&&field(&check,0,"op","index_publication_status"))return status(cfg,&check,db,out);
    if(!cfg->chutni_publish)return fail(out,"chutni_publish_disabled");
    if(!cfg->chutni_library||cfg->chutni_library[0]!='/'||!cfg->chutni_store||cfg->chutni_store[0]!='/'||!cfg->chutni_helper||!uuid(cfg->chutni_store_id)||!cfg->root)return fail(out,"invalid_chutni_configuration");
    Json request;char *requested=NULL,*op=NULL;if(!json_parse(&request,input,length)||!(op=json_string(&request,json_get(&request,0,"op"))))return fail(out,"invalid_request");
    int automatic=!strcmp(op,"index_publish_queue_step");free(op);if(!automatic){requested=json_string(&request,json_get(&request,0,"job_id"));if(!hex(requested)){free(requested);return fail(out,"invalid_request");}}
    const char *owner=cfg->owner?cfg->owner:"local-owner",*error=sources_interrupted(cfg);char scope[65],root[4096];sqlite3_int64 time=now_ms(),turn=0;
    if(!error&&(time<0||time>INT64_MAX-60000))error="state_unavailable";
    if(!error)error=inventory_scope_identity(cfg,"",scope);
    if(!error&&!realpath(cfg->root,root))error="scope_unavailable";
    if(!error)error=schema(db);
    if(!error)error=integrity(db,&turn);
    sqlite3_stmt *s=NULL;if(!error&&requested){if(!prepare(db,&s,"SELECT scope FROM index_jobs j JOIN index_root_scopes s ON s.job=j.id WHERE j.id=? AND j.owner=?"))error="store_failed";else{text(s,1,requested);text(s,2,owner);int rc=sqlite3_step(s);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index":"store_failed";else{const char *stored=(const char *)sqlite3_column_text(s,0);if(!stored||strcmp(stored,scope))error="scope_unavailable";}}sqlite3_finalize(s);s=NULL;}
    const char *selection="WITH versions AS (SELECT i.job,i.ordinal,0 AS generation,i.locator,i.text,i.evidence,i.digest FROM index_items i WHERE i.status='indexed' UNION ALL SELECT v.job,v.ordinal,v.generation,i.locator,v.text,v.evidence,v.digest FROM index_versions v JOIN index_items i ON i.job=v.job AND i.ordinal=v.ordinal) SELECT v.job,v.ordinal,v.generation,v.locator,v.text,v.evidence,v.digest,p.publication_id,p.failures FROM versions v JOIN index_jobs j ON j.id=v.job JOIN index_root_scopes r ON r.job=v.job LEFT JOIN index_publications p ON p.job=v.job AND p.ordinal=v.ordinal AND p.generation=v.generation AND p.store_id=? WHERE j.owner=? AND r.scope=? AND (?='' OR v.job=?) AND ((p.status='pending' AND p.next_ms<=?) OR (p.job IS NULL AND v.generation=coalesce((SELECT max(generation) FROM index_versions x WHERE x.job=v.job AND x.ordinal=v.ordinal),0))) ORDER BY CASE WHEN p.status='pending' THEN 0 ELSE 1 END,coalesce(p.turn,0),v.job,v.ordinal LIMIT 1";
    char *job=NULL,*locator=NULL,*literal=NULL,*evidence=NULL,*digest=NULL,*saved_key=NULL,*version=NULL,*evidence_id=NULL,*metadata=NULL,*body=NULL,*response=NULL;unsigned char *snapshot=NULL;size_t bytes=0,metadata_length=0,body_length=0,response_length=0;sqlite3_int64 ordinal=0,generation=0;int failures=0,reserved=0,found=0;
    if(!error&&!prepare(db,&s,selection))error="store_failed";
    if(!error){text(s,1,cfg->chutni_store_id);text(s,2,owner);text(s,3,scope);text(s,4,requested?requested:"");text(s,5,requested?requested:"");sqlite3_bind_int64(s,6,time);int rc=sqlite3_step(s);if(rc==SQLITE_ROW){found=1;job=column(s,0);ordinal=sqlite3_column_int64(s,1);generation=sqlite3_column_int64(s,2);locator=column(s,3);literal=column(s,4);evidence=column(s,5);digest=column(s,6);saved_key=column(s,7);failures=sqlite3_column_int(s,8);if(!hex(job)||ordinal<0||ordinal>255||generation<0||generation>256||!locator||!literal||!evidence||!hex(digest)||failures<0||failures>7)error="corrupt_index";}else if(rc!=SQLITE_DONE)error="store_failed";}
    sqlite3_finalize(s);s=NULL;
    char key[65],path[4096];Json captured;
    if(!error&&found){char actual[65];hash_bytes(evidence,strlen(evidence),actual);if(strcmp(actual,digest)||strlen(literal)>SOURCES_READ_MAX||!json_parse(&captured,evidence,strlen(evidence))||!field(&captured,0,"kind","literal")||(version=json_string(&captured,json_get(&captured,0,"version")))==NULL||!hex(version)||(evidence_id=json_string(&captured,json_get(&captured,0,"evidence_id")))==NULL||!hex(evidence_id))error="corrupt_index";
        if(!error){char *decoded=source_index_literal(evidence,strlen(evidence));if(!decoded||strcmp(decoded,literal))error="corrupt_index";free(decoded);}
        if(!error){size_t offset=0;for(const char *p=locator;*p;){const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);if(!n||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.')){error="corrupt_index";break;}offset+=n;if(!slash)break;p=slash+1;}if(locator[0]=='/'||!offset||snprintf(path,sizeof(path),"%s/%s",root,locator)>=(int)sizeof(path))error="corrupt_index";}
        if(!error){char material[512];int n=snprintf(material,sizeof(material),"chutni-publication-v1:%s:%s:%s:%s:%s",owner,scope,cfg->chutni_store_id,version,digest);if(n<0||n>=(int)sizeof(material))error="invalid_configuration";else{hash_bytes(material,(size_t)n,key);if(saved_key&&strcmp(saved_key,key))error="corrupt_publication";}}
        if(!error&&!prepare(db,&s,"SELECT bytes FROM snapshots JOIN source_claims c ON c.id=snapshots.id WHERE snapshots.id=? AND c.owner=? AND c.media=?"))error="store_failed";
        if(!error){text(s,1,version);text(s,2,owner);int provenance=json_get(&captured,0,"provenance");const char *media=field(&captured,provenance,"reader","html-readable-v1")?"text/html":field(&captured,provenance,"reader","docx-readable-v1")?"application/vnd.openxmlformats-officedocument.wordprocessingml.document":(field(&captured,provenance,"reader","pdfium-text-v1")||field(&captured,provenance,"reader","pdfium-index-fallback-v1"))?"application/pdf":field(&captured,provenance,"reader","image-ocr-v1")?"image/raster":"text/plain";text(s,3,media);int rc=sqlite3_step(s);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";else{int n=sqlite3_column_bytes(s,0);const void *data=sqlite3_column_blob(s,0);if(n<0||(size_t)n>SOURCES_INPUT_MAX||sqlite3_column_type(s,0)!=SQLITE_BLOB||(n&&!data))error="corrupt_snapshot";else{bytes=(size_t)n;snapshot=malloc(bytes?bytes:1);if(!snapshot)error="resource_limit";else{if(bytes)memcpy(snapshot,data,bytes);hash_bytes(snapshot,bytes,actual);if(strcmp(actual,version))error="corrupt_snapshot";}}}}sqlite3_finalize(s);s=NULL;
        FILE *f=NULL;if(!error){f=open_memstream(&metadata,&metadata_length);if(!f)error="resource_limit";else{const char *names[]={"library","store","store_id","root","path","publication_id","version","evidence_id","evidence_digest","text"};const char *values[]={cfg->chutni_library,cfg->chutni_store,cfg->chutni_store_id,root,path,key,version,evidence_id,digest,literal};fputc('{',f);for(size_t i=0;i<10;i++){if(i)fputc(',',f);json_quote(f,names[i],strlen(names[i]));fputc(':',f);json_quote(f,values[i],strlen(values[i]));}fputs(",\"evidence\":",f);fputs(evidence,f);fputc('}',f);if(fclose(f))error="resource_limit";}}
        if(!error)error=sources_interrupted(cfg);
        if(!error){error=reserve(db,job,ordinal,generation,cfg->chutni_store_id,version,evidence_id,digest,key,time,turn);if(!error)reserved=1;}
        if(!error)error=sources_native_chutni_publish(cfg,snapshot,bytes,metadata,&body,&body_length);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
        if(!error)error=post_sdk(cfg);
#endif
        if(!error){char actual_scope[65];error=inventory_scope_identity(cfg,"",actual_scope);if(!error&&strcmp(scope,actual_scope))error="scope_unavailable";}
        if(!error){f=open_memstream(&response,&response_length);if(!f)error="resource_limit";else{fprintf(f,"{\"contract_version\":1,\"published_items\":1,\"receipt_state\":\"committed\",\"job_id\":\"%s\",\"ordinal\":%lld,\"generation\":%lld,\"publication\":",job,(long long)ordinal,(long long)generation);fwrite(body,1,body_length,f);fputs("}\n",f);if(fclose(f))error="resource_limit";}}
        if(!error)error=sources_interrupted(cfg);
        if(reserved){sqlite3_int64 finished=now_ms();if(finished<0||finished>INT64_MAX-60000){if(!error)error="state_unavailable";}else{const char *saved=finish(db,job,ordinal,generation,cfg->chutni_store_id,key,error?NULL:response,finished,failures<7?failures+1:7,error);if(!error)error=saved;}}
    }
    if(!error){if(found)fwrite(response,1,response_length,out);else fputs("{\"contract_version\":1,\"published_items\":0,\"coverage\":\"no_due_publications\"}\n",out);}
    free(requested);free(job);free(locator);free(literal);free(evidence);free(digest);free(saved_key);free(version);free(evidence_id);free(metadata);free(body);free(response);free(snapshot);return error?fail(out,error):0;
}

/* Publish only an owner-authorized cached capture. The SDK's stable operation
 * key recovers a commit if the parent dies before recording its local receipt. */
static int derived_publish_capture(const SourcesConfig *cfg,const char *capture,size_t capture_bytes,sqlite3 *db,FILE *out,int visual){
    int summary=visual==2;visual=visual==1;
    FILE *stream=NULL;

    if(!cfg->chutni_publish)return fail(out,"chutni_publish_disabled");
    if(!cfg->chutni_library||cfg->chutni_library[0]!='/'||!cfg->chutni_store||cfg->chutni_store[0]!='/'||!cfg->chutni_helper||!uuid(cfg->chutni_store_id)||!cfg->root)return fail(out,"invalid_chutni_configuration");
    if(!(summary?cfg->summary_inference:visual?cfg->visual_inference:cfg->inference))return fail(out,"inference_unconfigured");
    const char *error=summary?sources_summary_capture_validate(cfg,capture,capture_bytes):visual?sources_visual_capture_validate(cfg,capture,capture_bytes):sources_transcript_capture_validate(cfg,capture,capture_bytes);Json j;
    char *source=NULL,*artifact=NULL,*literal=NULL,*locator=NULL,*metadata=NULL,*receipt=NULL;unsigned char *snapshot=NULL;size_t bytes=0,metadata_bytes=0,receipt_bytes=0;
    char scope[65],root[4096],path[4096],digest[65],key[65];
    if(!error&&!json_parse(&j,capture,capture_bytes))error="corrupt_cache";
    if(!error){source=json_string(&j,json_get(&j,0,"source_id"));artifact=json_string(&j,json_get(&j,0,"artifact_id"));literal=json_string(&j,json_get(&j,0,"text"));if(!hex(source)||!hex(artifact)||!literal)error="corrupt_cache";else hash_bytes(capture+j.tokens[0].begin,j.tokens[0].end-j.tokens[0].begin,digest);}
    if(!error)error=inventory_scope_identity(cfg,"",scope);
    if(!error&&!realpath(cfg->root,root))error="scope_unavailable";
    sqlite3_stmt *s=NULL;const char *owner=cfg->owner?cfg->owner:"local-owner";
    if(!error&&!prepare(db,&s,summary?"SELECT snapshots.bytes,c.locator FROM snapshots JOIN source_claims c ON c.id=snapshots.id WHERE c.owner=? AND c.id=? AND c.media IN ('text/plain','text/html','application/pdf','image/raster','application/vnd.openxmlformats-officedocument.wordprocessingml.document') LIMIT 1":"SELECT snapshots.bytes,c.locator FROM snapshots JOIN source_claims c ON c.id=snapshots.id WHERE c.owner=? AND c.id=? AND c.media IN ('audio/wav','application/x-samosa-encoded-media') ORDER BY c.media='audio/wav' DESC LIMIT 1"))error="store_failed";
    if(!error){text(s,1,owner);text(s,2,source);int step=sqlite3_step(s);if(step!=SQLITE_ROW)error=step==SQLITE_DONE?"unknown_source":"store_failed";else{int n=sqlite3_column_bytes(s,0);const void *data=sqlite3_column_blob(s,0);locator=column(s,1);if(n<0||(size_t)n>SOURCES_INPUT_MAX||sqlite3_column_type(s,0)!=SQLITE_BLOB||(n&&!data))error="corrupt_snapshot";else{bytes=(size_t)n;snapshot=malloc(bytes?bytes:1);if(!snapshot)error="resource_limit";else{if(bytes)memcpy(snapshot,data,bytes);char actual[65];hash_bytes(snapshot,bytes,actual);if(strcmp(actual,source))error="corrupt_snapshot";}}}}sqlite3_finalize(s);s=NULL;
    if(!error){if(!locator||!*locator||!strcmp(locator,"upload:")||strstr(locator,"://")||locator[0]=='/'||strlen(locator)>1024)error="publication_locator_unavailable";else for(const char *p=locator;*p;){const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);if(!n||(n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.')||(slash&&!slash[1])){error="publication_locator_unavailable";break;}if(!slash)break;p=slash+1;}}
    if(!error&&snprintf(path,sizeof(path),"%s/%s",root,locator)>=(int)sizeof(path))error="publication_locator_unavailable";
    if(!error){char material[512];int n=snprintf(material,sizeof(material),"%s:%s:%s:%s:%s:%s",summary?"chutni-summary-publication-v1":visual?"chutni-visual-publication-v1":"chutni-transcript-publication-v1",owner,scope,cfg->chutni_store_id,source,digest);if(n<0||n>=(int)sizeof(material))error="invalid_configuration";else hash_bytes(material,(size_t)n,key);}
    if(!error){stream=open_memstream(&metadata,&metadata_bytes);if(!stream)error="resource_limit";else{const char *names[]={"library","store","store_id","root","path","publication_id","version","evidence_id","evidence_digest","text"},*values[]={cfg->chutni_library,cfg->chutni_store,cfg->chutni_store_id,root,path,key,source,artifact,digest,literal};fputc('{',stream);for(size_t i=0;i<10;i++){if(i)fputc(',',stream);json_quote(stream,names[i],strlen(names[i]));fputc(':',stream);json_quote(stream,values[i],strlen(values[i]));}fputs(",\"evidence\":",stream);fwrite(capture+j.tokens[0].begin,1,j.tokens[0].end-j.tokens[0].begin,stream);fputc('}',stream);if(fclose(stream))error="resource_limit";}}
    if(!error)error=schema(db);
    if(!error&&!prepare(db,&s,"SELECT response,digest FROM transcript_publications WHERE key=? AND owner=?"))error="store_failed";
    if(!error){text(s,1,key);text(s,2,owner);int step=sqlite3_step(s);if(step==SQLITE_ROW){receipt=column(s,0);char *expected=column(s,1),actual[65];if(!receipt||strlen(receipt)>8192||sqlite3_column_type(s,0)!=SQLITE_TEXT||strlen(receipt)!=(size_t)sqlite3_column_bytes(s,0)||sqlite3_column_type(s,1)!=SQLITE_TEXT||sqlite3_column_bytes(s,1)!=64||!hex(expected))error="corrupt_publication";else{receipt_bytes=strlen(receipt);hash_bytes(receipt,receipt_bytes,actual);if(strcmp(actual,expected)||sources_chutni_publish_validate(metadata,receipt,receipt_bytes))error="corrupt_publication";}free(expected);}else if(step!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(s);s=NULL;
    int saved=receipt!=NULL;
    if(!error&&!saved){if(!prepare(db,&s,"SELECT (SELECT count(*) FROM transcript_publications)+(SELECT count(*) FROM index_publications)"))error="store_failed";else if(sqlite3_step(s)!=SQLITE_ROW)error="store_failed";else if(sqlite3_column_int64(s,0)>=4096)error="publication_limit";sqlite3_finalize(s);s=NULL;}
    if(!error)error=sources_interrupted(cfg);
    if(!error&&!saved)error=sources_native_chutni_publish(cfg,snapshot,bytes,metadata,&receipt,&receipt_bytes);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if(!error&&!saved)error=post_sdk(cfg);
#endif
    if(!error){char actual[65];error=inventory_scope_identity(cfg,"",actual);if(!error&&strcmp(actual,scope))error="scope_unavailable";}
    if(!error&&!saved){if(!sql(db,"BEGIN IMMEDIATE"))error="store_busy";else{
        if(!prepare(db,&s,"INSERT INTO transcript_publications(key,owner,response,digest) SELECT ?,?,?,? WHERE (SELECT count(*) FROM transcript_publications)+(SELECT count(*) FROM index_publications)<4096 ON CONFLICT(key) DO NOTHING"))error="store_failed";
        if(!error){char actual[65];hash_bytes(receipt,receipt_bytes,actual);text(s,1,key);text(s,2,owner);text(s,3,receipt);text(s,4,actual);if(sqlite3_step(s)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(s);s=NULL;
        /* Concurrent recoveries may differ only in the recovered flag. Keep the
         * first committed receipt and validate its immutable publication identity. */
        if(!error&&!prepare(db,&s,"SELECT response,digest FROM transcript_publications WHERE key=? AND owner=?"))error="store_failed";
        if(!error){text(s,1,key);text(s,2,owner);int step=sqlite3_step(s);if(step!=SQLITE_ROW)error=step==SQLITE_DONE?"publication_limit":"store_failed";else{char *first=column(s,0),*expected=column(s,1),actual[65];if(first)hash_bytes(first,strlen(first),actual);if(!first||strlen(first)>8192||sqlite3_column_type(s,0)!=SQLITE_TEXT||strlen(first)!=(size_t)sqlite3_column_bytes(s,0)||sqlite3_column_type(s,1)!=SQLITE_TEXT||sqlite3_column_bytes(s,1)!=64||!hex(expected)||strcmp(actual,expected)||sources_chutni_publish_validate(metadata,first,strlen(first)))error="corrupt_publication";else{free(receipt);receipt=first;first=NULL;receipt_bytes=strlen(receipt);}free(first);free(expected);}}sqlite3_finalize(s);
        if(!error)error=sources_interrupted(cfg);
        if(error)sql(db,"ROLLBACK");else if(!sql(db,"COMMIT")){sql(db,"ROLLBACK");error="store_failed";}
    }}
    if(!error){fputs("{\"contract_version\":1,\"published_items\":1,\"receipt_state\":\"committed\",\"publication\":",out);fwrite(receipt,1,receipt_bytes,out);fputs("}\n",out);}
    free(source);free(artifact);free(literal);free(locator);free(metadata);free(receipt);free(snapshot);return error?fail(out,error):0;
}

int source_transcript_publish_capture(const SourcesConfig *cfg,const char *capture,size_t n,sqlite3 *db,FILE *out) {
    return derived_publish_capture(cfg,capture,n,db,out,0);
}
int source_visual_publish_capture(const SourcesConfig *cfg,const char *capture,size_t n,sqlite3 *db,FILE *out) {
    return derived_publish_capture(cfg,capture,n,db,out,1);
}
int source_visual_publish(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out) {
    if(!cfg->chutni_publish)return fail(out,"chutni_publish_disabled");
    if(!cfg->chutni_library||cfg->chutni_library[0]!='/'||!cfg->chutni_store||cfg->chutni_store[0]!='/'||!cfg->chutni_helper||!uuid(cfg->chutni_store_id)||!cfg->root)return fail(out,"invalid_chutni_configuration");
    if(!cfg->visual_inference)return fail(out,"inference_unconfigured");
    SourcesInferenceConfig identity=*cfg->visual_inference;identity.call=NULL;
    SourcesConfig cached=*cfg;cached.visual_inference=&identity;
    char *capture=NULL;size_t n=0;FILE *stream=open_memstream(&capture,&n);
    if(!stream)return fail(out,"resource_limit");
    int rc=sources_visual_dispatch(&cached,request,db,stream);
    if(fclose(stream)){free(capture);return fail(out,"resource_limit");}
    if(rc){fwrite(capture,1,n,out);free(capture);return 1;}
    rc=source_visual_publish_capture(cfg,capture,n,db,out);free(capture);return rc;
}

int source_transcript_publish(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out){
    if(!cfg->chutni_publish)return fail(out,"chutni_publish_disabled");
    if(!cfg->chutni_library||cfg->chutni_library[0]!='/'||!cfg->chutni_store||cfg->chutni_store[0]!='/'||!cfg->chutni_helper||!uuid(cfg->chutni_store_id)||!cfg->root)return fail(out,"invalid_chutni_configuration");
    if(!cfg->inference)return fail(out,"inference_unconfigured");
    SourcesInferenceConfig identity=*cfg->inference;identity.call=NULL;SourcesConfig cached=*cfg;cached.inference=&identity;
    char *capture=NULL;size_t capture_bytes=0;FILE *stream=open_memstream(&capture,&capture_bytes);if(!stream)return fail(out,"resource_limit");
    int rc=sources_transcript_dispatch(&cached,request,db,stream);if(fclose(stream)){free(capture);return fail(out,"resource_limit");}
    if(rc){fwrite(capture,1,capture_bytes,out);free(capture);return 1;}
    rc=source_transcript_publish_capture(cfg,capture,capture_bytes,db,out);free(capture);return rc;
}

int source_summary_publish_capture(const SourcesConfig *cfg,const char *capture,size_t bytes,sqlite3 *db,FILE *out){return derived_publish_capture(cfg,capture,bytes,db,out,2);}
int source_summary_publish(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out){
    if(!cfg->chutni_publish)return fail(out,"chutni_publish_disabled");
    if(!cfg->summary_inference)return fail(out,"inference_unconfigured");
    SourcesInferenceConfig identity=*cfg->summary_inference;identity.call=NULL;SourcesConfig cached=*cfg;cached.summary_inference=&identity;
    char *capture=NULL;size_t bytes=0;FILE *stream=open_memstream(&capture,&bytes);if(!stream)return fail(out,"resource_limit");
    int rc=sources_summary_opening_dispatch(&cached,request,db,stream);if(fclose(stream)){free(capture);return fail(out,"resource_limit");}
    if(rc){fwrite(capture,1,bytes,out);free(capture);return 1;}
    rc=source_summary_publish_capture(cfg,capture,bytes,db,out);free(capture);return rc;
}
