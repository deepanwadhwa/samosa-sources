/* Shared durable queue implementation. Instantiate only with fixed modality
 * callbacks/namespaces; SQLite admission, lease, receipts, backoff and recovery
 * remain one implementation for audio transcripts and visual frame samples. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "visual.h"
#include "audio.h"
#include "transcript.h"
#include "chutni.h"
#include "index.h"
#include "inventory.h"
#include "native.h"
#include "control.h"
#include "sha256.h"
#include <time.h>
#include <unistd.h>
#define QUEUE_BYTES_MAX (8u*1048576u)
static int fail(FILE *out,const char *error){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",error);return 1;}
static int hex(const char *s){if(!s||strlen(s)!=64)return 0;for(size_t i=0;i<64;i++)if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static int uuid(const char *s){if(!s||strlen(s)!=36)return 0;for(size_t i=0;i<36;i++)if(i==8||i==13||i==18||i==23){if(s[i]!='-')return 0;}else if(!strchr("0123456789abcdef",s[i]))return 0;return 1;}
static int sql(sqlite3 *db,const char *s){return sqlite3_exec(db,s,NULL,NULL,NULL)==SQLITE_OK;}
static int prepare(sqlite3 *db,sqlite3_stmt **s,const char *query){return sqlite3_prepare_v2(db,query,-1,s,NULL)==SQLITE_OK;}
static void text(sqlite3_stmt *s,int i,const char *v){sqlite3_bind_text(s,i,v,-1,SQLITE_TRANSIENT);}
static char *column(sqlite3_stmt *s,int i){const char *v=(const char*)sqlite3_column_text(s,i);return sqlite3_column_type(s,i)==SQLITE_TEXT&&v&&strlen(v)==(size_t)sqlite3_column_bytes(s,i)?strdup(v):NULL;}
static sqlite3_int64 now_ms(void){struct timespec t;if(clock_gettime(CLOCK_REALTIME,&t)||t.tv_sec<0||t.tv_sec>INT64_MAX/1000-61)return -1;return (sqlite3_int64)t.tv_sec*1000+t.tv_nsec/1000000;}
static const char *schema(sqlite3 *db){return sql(db,"CREATE TABLE IF NOT EXISTS audio_index_jobs(id TEXT PRIMARY KEY,owner TEXT NOT NULL,scope TEXT NOT NULL,source TEXT NOT NULL,start_ms INTEGER NOT NULL,end_ms INTEGER NOT NULL,total INTEGER NOT NULL,cursor INTEGER NOT NULL DEFAULT 0,recipe TEXT NOT NULL,store TEXT NOT NULL,next_ms INTEGER NOT NULL DEFAULT 0,failures INTEGER NOT NULL DEFAULT 0,turn INTEGER NOT NULL DEFAULT 0,last_error TEXT);CREATE TABLE IF NOT EXISTS audio_index_windows(job TEXT NOT NULL,ordinal INTEGER NOT NULL,capture TEXT NOT NULL,digest TEXT NOT NULL,receipt TEXT,receipt_digest TEXT,PRIMARY KEY(job,ordinal));CREATE TABLE IF NOT EXISTS visual_index_jobs(id TEXT PRIMARY KEY,owner TEXT NOT NULL,scope TEXT NOT NULL,source TEXT NOT NULL,start_ms INTEGER NOT NULL,end_ms INTEGER NOT NULL,total INTEGER NOT NULL,cursor INTEGER NOT NULL DEFAULT 0,recipe TEXT NOT NULL,store TEXT NOT NULL,next_ms INTEGER NOT NULL DEFAULT 0,failures INTEGER NOT NULL DEFAULT 0,turn INTEGER NOT NULL DEFAULT 0,last_error TEXT);CREATE TABLE IF NOT EXISTS visual_index_windows(job TEXT NOT NULL,ordinal INTEGER NOT NULL,capture TEXT NOT NULL,digest TEXT NOT NULL,receipt TEXT,receipt_digest TEXT,PRIMARY KEY(job,ordinal))")?NULL:((sqlite3_errcode(db)==SQLITE_BUSY||sqlite3_errcode(db)==SQLITE_LOCKED)?"store_busy":"store_failed");}
#if QUEUE_VISUAL
static const char *snapshot(const SourcesConfig *cfg,sqlite3 *db,const char *source,size_t end,char recipe[65]) {
    char model[65],decoder[65];const char *error=sources_visual_recipe(cfg,model);
    sqlite3_stmt *stmt=NULL;
    if(!error&&!prepare(db,&stmt,"SELECT b.bytes,c.locator FROM snapshots b JOIN source_claims c ON c.id=b.id WHERE c.owner=? AND c.id=? AND c.media='application/x-samosa-encoded-media' LIMIT 1"))error="store_failed";
    if(!error) {
        text(stmt,1,cfg->owner?cfg->owner:"local-owner");text(stmt,2,source);
        int rc=sqlite3_step(stmt);
        if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";
        else {
            const void *bytes=sqlite3_column_blob(stmt,0);int n=sqlite3_column_bytes(stmt,0);
            char *locator=column(stmt,1),actual[65];
            if(!locator||!bytes||n<=0||(size_t)n>SOURCES_INPUT_MAX||sqlite3_column_type(stmt,0)!=SQLITE_BLOB)error="corrupt_snapshot";
            else {
                hash_bytes(bytes,(size_t)n,actual);
                if(strcmp(actual,source))error="corrupt_snapshot";
                if(!error)error=sources_native_media_fingerprint(cfg,decoder);
                char *body=NULL;size_t length=0,duration=0;
                if(!error)error=sources_native_video_probe(cfg,bytes,(size_t)n,decoder,&body,&length);
                if(!error){Json j;if(!json_parse(&j,body,length)||!json_uint(&j,json_get(&j,0,"duration_ms"),&duration))error="native_helper_protocol";else if(end>duration)error="invalid_range";}
                free(body);
                if(!error&&cfg->chutni_publish&&(!strcmp(locator,"upload:")||strstr(locator,"://")))error="publication_locator_unavailable";
            }
            free(locator);
        }
    }
    sqlite3_finalize(stmt);
    if(!error){char material[160];int n=snprintf(material,sizeof(material),"visual-index-recipe-v1:%s:%s",model,decoder);hash_bytes(material,(size_t)n,recipe);}
    return error;
}
#else
static const char *snapshot(const SourcesConfig *cfg,sqlite3 *db,const char *source,size_t end,char recipe[65]){
    char model[65],decoder[65];const char *error=sources_transcript_recipe(cfg,model);sqlite3_stmt *s=NULL;
    if(!error&&!prepare(db,&s,"SELECT b.bytes,c.media,c.locator FROM snapshots b JOIN source_claims c ON c.id=b.id WHERE c.owner=? AND c.id=? AND c.media IN ('audio/wav','application/x-samosa-encoded-media') ORDER BY c.media='audio/wav' DESC LIMIT 1"))error="store_failed";
    if(!error){text(s,1,cfg->owner?cfg->owner:"local-owner");text(s,2,source);int rc=sqlite3_step(s);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";else{const void *bytes=sqlite3_column_blob(s,0);int n=sqlite3_column_bytes(s,0);char *media=column(s,1),*locator=column(s,2),actual[65];if(!media||!locator||!bytes||n<=0||(size_t)n>SOURCES_INPUT_MAX||sqlite3_column_type(s,0)!=SQLITE_BLOB)error="corrupt_snapshot";else{hash_bytes(bytes,(size_t)n,actual);if(strcmp(actual,source))error="corrupt_snapshot";else if(!strcmp(media,"audio/wav")){SourcesWave wave;error=sources_wave_inspect(cfg,bytes,(size_t)n,&wave);if(!error&&end>wave.frames/16)error="invalid_range";if(!error)hash_bytes("audio-pcm-window-v1:RIFF:PCM16:mono:16000:exclusive-ms:lossless",strlen("audio-pcm-window-v1:RIFF:PCM16:mono:16000:exclusive-ms:lossless"),decoder);}else error=sources_native_media_fingerprint(cfg,decoder);if(!error&&cfg->chutni_publish&&(!strcmp(locator,"upload:")||strstr(locator,"://")))error="publication_locator_unavailable";}free(media);free(locator);}}
    sqlite3_finalize(s);if(!error){char material[160];int n=snprintf(material,sizeof(material),"" QUEUE_PREFIX "-index-recipe-v1:%s:%s",model,decoder);hash_bytes(material,(size_t)n,recipe);}return error;
}
#endif
typedef struct {char *id,*scope,*source,*recipe,*store,*last_error;size_t start,end,total,cursor,published;sqlite3_int64 next,turn;int failures;} Job;
static void release(Job *j){free(j->id);free(j->scope);free(j->source);free(j->recipe);free(j->store);free(j->last_error);memset(j,0,sizeof(*j));}
static void job_id(const SourcesConfig *cfg,const Job *j,char id[65]){Sha256 hash;sha256_init(&hash);const char *owner=cfg->owner?cfg->owner:"local-owner";char selector[64];snprintf(selector,sizeof(selector),":%zu:%zu:",j->start,j->end);sha256_update(&hash,QUEUE_PREFIX "-index-v1:",strlen(QUEUE_PREFIX "-index-v1:"));sha256_update(&hash,owner,strlen(owner));sha256_update(&hash,j->scope,64);sha256_update(&hash,j->source,64);sha256_update(&hash,j->recipe,64);sha256_update(&hash,selector,strlen(selector));sha256_update(&hash,j->store,strlen(j->store));unsigned char digest[32];sha256_final(&hash,digest);sha256_hex(digest,id);}
static int equal(const Json *j,int token,const char *key,const char *expected){char *s=json_string(j,json_get(j,token,key));int same=s&&expected&&!strcmp(s,expected);free(s);return same;}
static const char *response_error(const char *body,size_t bytes,char code[65]){Json j;char *s=NULL;if(json_parse(&j,body,bytes))s=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));int good=s&&*s&&strlen(s)<=64;for(size_t i=0;good&&s[i];i++)if(!strchr("abcdefghijklmnopqrstuvwxyz0123456789_",s[i]))good=0;if(good)memcpy(code,s,strlen(s)+1);free(s);return good?code:"invalid_response";}
static const char *receipt_valid(const SourcesConfig *cfg,const Job *job,const char *capture,const char *receipt){
    Json c,r;char digest[65],key[65],material[512],metadata[600];if(!json_parse(&c,capture,strlen(capture))||!json_parse(&r,receipt,strlen(receipt))||r.tokens[0].count!=4||!equal(&r,0,"receipt_state","committed"))return "corrupt_" QUEUE_PREFIX "_index";
    size_t version=0,items=0;if(!json_uint(&r,json_get(&r,0,"contract_version"),&version)||version!=1||!json_uint(&r,json_get(&r,0,"published_items"),&items)||items!=1)return "corrupt_" QUEUE_PREFIX "_index";
    hash_bytes(capture+c.tokens[0].begin,c.tokens[0].end-c.tokens[0].begin,digest);int n=snprintf(material,sizeof(material),"" QUEUE_PUBLICATION ":%s:%s:%s:%s:%s",cfg->owner?cfg->owner:"local-owner",job->scope,job->store,job->source,digest);if(n<0||n>=(int)sizeof(material))return "invalid_configuration";hash_bytes(material,(size_t)n,key);char *artifact=json_string(&c,json_get(&c,0,"artifact_id"));int item=json_get(&r,0,"publication");if(!hex(artifact)||item<0){free(artifact);return "corrupt_" QUEUE_PREFIX "_index";}
    snprintf(metadata,sizeof(metadata),"{\"store_id\":\"%s\",\"publication_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\",\"evidence\":{\"kind\":\"derived\"}}",job->store,key,job->source,artifact);free(artifact);char *body=strndup(r.text+r.tokens[item].begin,r.tokens[item].end-r.tokens[item].begin);if(!body)return "resource_limit";const char *error=sources_chutni_publish_validate(metadata,body,strlen(body));free(body);return error?"corrupt_" QUEUE_PREFIX "_index":NULL;
}
static const char *window(sqlite3 *db,const SourcesConfig *cfg,Job *job,size_t ordinal,char **capture,char **receipt){
    *capture=*receipt=NULL;sqlite3_stmt *s=NULL;const char *error=NULL;if(!prepare(db,&s,"SELECT capture,digest,receipt,receipt_digest FROM " QUEUE_PREFIX "_index_windows WHERE job=? AND ordinal=?"))return "store_failed";text(s,1,job->id);sqlite3_bind_int64(s,2,(sqlite3_int64)ordinal);int rc=sqlite3_step(s);
    if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"corrupt_" QUEUE_PREFIX "_index":"store_failed";else{*capture=column(s,0);char *proof=column(s,1),actual[65];if(!*capture||strlen(*capture)>262144||!hex(proof))error="corrupt_" QUEUE_PREFIX "_index";else{hash_bytes(*capture,strlen(*capture),actual);if(strcmp(actual,proof)||QUEUE_VALIDATE(cfg,*capture,strlen(*capture)))error="corrupt_" QUEUE_PREFIX "_index";else{Json c;size_t count,start,end,a,b;json_parse(&c,*capture,strlen(*capture));int range=json_get(&c,0,"range");error=QUEUE_PLAN(cfg,job->end-job->start,ordinal,&count,&start,&end);if(!error&&(!equal(&c,0,"source_id",job->source)||!json_uint(&c,json_get(&c,range,"start_ms"),&a)||!json_uint(&c,json_get(&c,range,"end_ms"),&b)||a!=job->start+start||b!=job->start+end))error="corrupt_" QUEUE_PREFIX "_index";}}free(proof);
        if(!error&&sqlite3_column_type(s,2)!=SQLITE_NULL){*receipt=column(s,2);proof=column(s,3);if(!*receipt||strlen(*receipt)>8192||!hex(proof)||!*job->store)error="corrupt_" QUEUE_PREFIX "_index";else{hash_bytes(*receipt,strlen(*receipt),actual);if(strcmp(actual,proof))error="corrupt_" QUEUE_PREFIX "_index";else error=receipt_valid(cfg,job,*capture,*receipt);}free(proof);}else if(sqlite3_column_type(s,3)!=SQLITE_NULL)error="corrupt_" QUEUE_PREFIX "_index";}
    sqlite3_finalize(s);if(error){free(*capture);free(*receipt);*capture=*receipt=NULL;}return error;
}
static const char *load(sqlite3 *db,const SourcesConfig *cfg,const char *id,Job *j){
    memset(j,0,sizeof(*j));sqlite3_stmt *s=NULL;const char *error=NULL;
    if(!prepare(db,&s,"SELECT id,scope,source,start_ms,end_ms,total,cursor,recipe,store,next_ms,failures,turn,last_error FROM " QUEUE_PREFIX "_index_jobs WHERE id=? AND owner=?"))return "store_failed";
    text(s,1,id);text(s,2,cfg->owner?cfg->owner:"local-owner");int rc=sqlite3_step(s);
    if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_" QUEUE_PREFIX "_index":"store_failed";else{j->id=column(s,0);j->scope=column(s,1);j->source=column(s,2);j->recipe=column(s,7);j->store=column(s,8);sqlite3_int64 a=sqlite3_column_int64(s,3),b=sqlite3_column_int64(s,4),total=sqlite3_column_int64(s,5),cursor=sqlite3_column_int64(s,6),failures=sqlite3_column_int64(s,10);j->next=sqlite3_column_int64(s,9);j->turn=sqlite3_column_int64(s,11);if(sqlite3_column_type(s,12)!=SQLITE_NULL){j->last_error=column(s,12);if(!j->last_error||!*j->last_error||strlen(j->last_error)>64)error="corrupt_" QUEUE_PREFIX "_index";else for(size_t i=0;j->last_error[i];i++)if(!strchr("abcdefghijklmnopqrstuvwxyz0123456789_",j->last_error[i]))error="corrupt_" QUEUE_PREFIX "_index";}if((failures==0&&j->last_error)||(failures>0&&!j->last_error))error="corrupt_" QUEUE_PREFIX "_index";
        if(!hex(j->id)||!hex(j->scope)||!hex(j->source)||!hex(j->recipe)||!j->store||(*j->store&&!uuid(j->store))||a<0||b<=a||b>14400000||total<1||total>497||cursor<0||cursor>total||j->next<0||j->turn<0||failures<0||failures>7)error="corrupt_" QUEUE_PREFIX "_index";
        const int columns[]={3,4,5,6,9,10,11};for(size_t i=0;!error&&i<7;i++)if(sqlite3_column_type(s,columns[i])!=SQLITE_INTEGER)error="corrupt_" QUEUE_PREFIX "_index";
        if(!error){j->start=(size_t)a;j->end=(size_t)b;j->total=(size_t)total;j->cursor=(size_t)cursor;j->failures=(int)failures;size_t count,start,end;error=QUEUE_PLAN(cfg,j->end-j->start,0,&count,&start,&end);if(!error&&count!=j->total)error="corrupt_" QUEUE_PREFIX "_index";char actual[65];job_id(cfg,j,actual);if(!error&&strcmp(actual,j->id))error="corrupt_" QUEUE_PREFIX "_index";}}
    sqlite3_finalize(s);s=NULL;if(!error){char scope[65],recipe[65];error=inventory_scope_identity(cfg,"",scope);if(!error&&strcmp(scope,j->scope))error="scope_unavailable";if(!error)error=snapshot(cfg,db,j->source,j->end,recipe);if(!error&&strcmp(recipe,j->recipe))error="" QUEUE_PREFIX "_index_configuration_changed";}
    if(!error&&!prepare(db,&s,"SELECT ordinal,receipt IS NOT NULL FROM " QUEUE_PREFIX "_index_windows WHERE job=? ORDER BY ordinal"))error="store_failed";
    if(!error){text(s,1,id);size_t count=0;while(!error){int step=sqlite3_step(s);if(step==SQLITE_DONE)break;if(step!=SQLITE_ROW){error="store_failed";break;}if(sqlite3_column_type(s,0)!=SQLITE_INTEGER||sqlite3_column_int64(s,0)!=(sqlite3_int64)count||count>=j->cursor){error="corrupt_" QUEUE_PREFIX "_index";break;}char *capture=NULL,*receipt=NULL;error=window(db,cfg,j,count,&capture,&receipt);if(!error){if(receipt){if(j->published!=count)error="corrupt_" QUEUE_PREFIX "_index";else j->published++;}else if(*j->store&&count+1!=j->cursor)error="corrupt_" QUEUE_PREFIX "_index";count++;}free(capture);free(receipt);}if(!error&&(count!=j->cursor||(!*j->store&&j->published)||(j->published<j->cursor&&*j->store&&j->published+1!=j->cursor)))error="corrupt_" QUEUE_PREFIX "_index";}
    sqlite3_finalize(s);return error;
}
static void status(FILE *out,const Job *j){fprintf(out,"{\"contract_version\":1,\"job_id\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"status\":\"%s\",\"total_windows\":%zu,\"captured_windows\":%zu,\"published_windows\":%zu,\"publication_required\":%s,\"range\":{\"start_ms\":%zu,\"end_ms\":%zu},\"coverage\":{\"status\":\"partial\",\"reason\":\"" QUEUE_COVERAGE "\"},\"retry_at_unix_ms\":%lld,\"failures\":%d,\"last_error\":",j->id,j->source,j->source,j->cursor==j->total&&(!*j->store||j->published==j->total)?"completed":"ready",j->total,j->cursor,j->published,*j->store?"true":"false",j->start,j->end,(long long)j->next,j->failures);if(j->last_error)json_quote(out,j->last_error,strlen(j->last_error));else fputs("null",out);fputs("}\n",out);}
const char *QUEUE_REPORT(const SourcesConfig *cfg,sqlite3 *db,const char *id,const char *source,FILE *out,int *completed){
    *completed=0;Job j={0};const char *error=hex(id)&&hex(source)?load(db,cfg,id,&j):"corrupt_index";
    if(!error&&strcmp(j.source,source))error="corrupt_index";
    if(!error){*completed=j.cursor==j.total&&(!*j.store||j.published==j.total);status(out,&j);}
    release(&j);return error;
}
const char *QUEUE_READ(const SourcesConfig *cfg,sqlite3 *db,const char *id,const char *source,size_t ordinal,FILE *out){
    Job j={0};char *capture=NULL,*receipt=NULL;const char *error=hex(id)&&hex(source)?load(db,cfg,id,&j):"corrupt_index";
    if(!error&&strcmp(j.source,source))error="corrupt_index";
    if(!error&&ordinal>=j.cursor)error="invalid_range";
    if(!error)error=window(db,cfg,&j,ordinal,&capture,&receipt);
    if(!error)fputs(capture,out);
    free(capture);free(receipt);release(&j);return error;
}
const char *QUEUE_SCAN(const SourcesConfig *cfg,sqlite3 *db,const char *id,const char *source,const char *(*visit)(void *,const char *,size_t,const char *),void *context,size_t *uncaptured){
    Job j={0};const char *error=hex(id)&&hex(source)&&visit?load(db,cfg,id,&j):"corrupt_index";
    if(!error&&strcmp(j.source,source))error="corrupt_index";
    if(!error)*uncaptured+=j.total-j.cursor;
    for(size_t ordinal=0;!error&&ordinal<j.cursor;ordinal++){
        char *capture=NULL,*receipt=NULL;error=window(db,cfg,&j,ordinal,&capture,&receipt);
        if(!error)error=visit(context,id,ordinal,capture);
        free(capture);free(receipt);
    }
    release(&j);return error;
}
static int create(sqlite3 *db,const SourcesConfig *cfg,const Json *request,FILE *out){
    Job j={0};char *version=json_string(request,json_get(request,0,"version"));j.source=json_string(request,json_get(request,0,"source_id"));const char *error=NULL;char scope[65],recipe[65],id[65];size_t start,end;
    if(!hex(j.source)||!hex(version)||!json_uint(request,json_get(request,0,"start_ms"),&j.start)||!json_uint(request,json_get(request,0,"end_ms"),&j.end)||j.start>=j.end||j.end>14400000)error="invalid_request";else if(strcmp(version,j.source))error="stale_source";free(version);
    if(!error)error=inventory_scope_identity(cfg,"",scope);
    if(!error)error=snapshot(cfg,db,j.source,j.end,recipe);
    if(!error)error=QUEUE_PLAN(cfg,j.end-j.start,0,&j.total,&start,&end);
    if(!error){j.scope=strdup(scope);j.recipe=strdup(recipe);j.store=strdup(cfg->chutni_publish&&cfg->chutni_store_id?cfg->chutni_store_id:"");if(!j.scope||!j.recipe||!j.store)error="resource_limit";else if(cfg->chutni_publish&&!uuid(j.store))error="invalid_chutni_configuration";else{job_id(cfg,&j,id);j.id=strdup(id);if(!j.id)error="resource_limit";}}
    sqlite3_stmt *s=NULL;int transaction=0;if(!error&&sqlite3_get_autocommit(db)){if(!sql(db,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    if(!error&&!prepare(db,&s,"INSERT INTO " QUEUE_PREFIX "_index_jobs(id,owner,scope,source,start_ms,end_ms,total,recipe,store) SELECT ?,?,?,?,?,?,?,?,? WHERE (SELECT (SELECT count(*) FROM audio_index_jobs)+(SELECT count(*) FROM visual_index_jobs))<8 OR EXISTS(SELECT 1 FROM " QUEUE_PREFIX "_index_jobs WHERE id=?) ON CONFLICT(id) DO NOTHING"))error="store_failed";
    if(!error){text(s,1,j.id);text(s,2,cfg->owner?cfg->owner:"local-owner");text(s,3,j.scope);text(s,4,j.source);sqlite3_bind_int64(s,5,(sqlite3_int64)j.start);sqlite3_bind_int64(s,6,(sqlite3_int64)j.end);sqlite3_bind_int64(s,7,(sqlite3_int64)j.total);text(s,8,j.recipe);text(s,9,j.store);text(s,10,j.id);if(sqlite3_step(s)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(s);s=NULL;
    if(!error&&!prepare(db,&s,"SELECT 1 FROM " QUEUE_PREFIX "_index_jobs WHERE id=?"))error="store_failed";
    if(!error){text(s,1,j.id);if(sqlite3_step(s)!=SQLITE_ROW)error="" QUEUE_PREFIX "_index_limit";}sqlite3_finalize(s);if(!error)error=sources_interrupted(cfg);
    if(transaction){if(error)sql(db,"ROLLBACK");else if(!sql(db,"COMMIT")){sql(db,"ROLLBACK");error="store_failed";}}
    if(!error){char *saved=strdup(j.id);if(!saved)error="resource_limit";else{release(&j);error=load(db,cfg,saved,&j);free(saved);}}if(!error)status(out,&j);release(&j);return error?fail(out,error):0;
}
const char *QUEUE_ADMIT(const SourcesConfig *cfg,sqlite3 *db,const char *source,size_t duration,char id[65],char failure[65]){
    id[0]=0;
    if(!db||sqlite3_get_autocommit(db)||!hex(source)||!duration||duration>14400000)return "invalid_configuration";
    const char *error=schema(db);if(error)return error;
    char request[256];int count=snprintf(request,sizeof(request),"{\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":%zu}",source,source,duration);
    Json j;if(count<0||(size_t)count>=sizeof(request)||!json_parse(&j,request,(size_t)count))return "invalid_configuration";
    char *response=NULL;size_t length=0;FILE *stream=open_memstream(&response,&length);if(!stream)return "resource_limit";
    int rc=create(db,cfg,&j,stream);if(fclose(stream)){free(response);return "resource_limit";}
    if(rc)error=response_error(response,length,failure);
    else if(!json_parse(&j,response,length))error="invalid_response";
    else{char *saved=json_string(&j,json_get(&j,0,"job_id"));if(!hex(saved))error="invalid_response";else memcpy(id,saved,65);free(saved);}
    free(response);return error;
}
static const char *advance(sqlite3 *db,const SourcesConfig *cfg,Job *j,const char *capture,const char *receipt){
    if(!sql(db,"BEGIN IMMEDIATE"))return "store_busy";
    sqlite3_stmt *s=NULL;const char *error=NULL;
    if(capture){if(!prepare(db,&s,"INSERT INTO " QUEUE_PREFIX "_index_windows(job,ordinal,capture,digest) SELECT ?,?,?,? WHERE coalesce((SELECT sum(length(CAST(capture AS BLOB))) FROM (SELECT capture FROM audio_index_windows UNION ALL SELECT capture FROM visual_index_windows)),0)+?<=8388608"))error="store_failed";else{char digest[65];hash_bytes(capture,strlen(capture),digest);text(s,1,j->id);sqlite3_bind_int64(s,2,(sqlite3_int64)j->cursor);text(s,3,capture);text(s,4,digest);sqlite3_bind_int64(s,5,(sqlite3_int64)strlen(capture));if(sqlite3_step(s)!=SQLITE_DONE)error="store_failed";else if(sqlite3_changes(db)!=1)error="" QUEUE_PREFIX "_index_limit";}}
    else if(receipt){if(!prepare(db,&s,"UPDATE " QUEUE_PREFIX "_index_windows SET receipt=?,receipt_digest=? WHERE job=? AND ordinal=? AND receipt IS NULL"))error="store_failed";else{char digest[65];hash_bytes(receipt,strlen(receipt),digest);text(s,1,receipt);text(s,2,digest);text(s,3,j->id);sqlite3_bind_int64(s,4,(sqlite3_int64)j->published);if(sqlite3_step(s)!=SQLITE_DONE||sqlite3_changes(db)!=1)error="corrupt_" QUEUE_PREFIX "_index";}}
    sqlite3_finalize(s);s=NULL;if(!error&&!prepare(db,&s,"UPDATE " QUEUE_PREFIX "_index_jobs SET cursor=cursor+?,next_ms=0,failures=0,last_error=NULL,turn=(SELECT coalesce(max(turn),0)+1 FROM " QUEUE_PREFIX "_index_jobs) WHERE id=? AND cursor=? AND (SELECT coalesce(max(turn),0) FROM " QUEUE_PREFIX "_index_jobs)<9223372036854775807"))error="store_failed";
    if(!error){sqlite3_bind_int(s,1,capture?1:0);text(s,2,j->id);sqlite3_bind_int64(s,3,(sqlite3_int64)j->cursor);if(sqlite3_step(s)!=SQLITE_DONE||sqlite3_changes(db)!=1)error="scheduler_limit";}sqlite3_finalize(s);if(!error)error=sources_interrupted(cfg);
    if(error)sql(db,"ROLLBACK");else if(!sql(db,"COMMIT")){sql(db,"ROLLBACK");error="store_failed";}return error;
}
static int step(sqlite3 *db,const SourcesConfig *cfg,const Json *request,int automatic,FILE *out){
    char *id=automatic?NULL:json_string(request,json_get(request,0,"job_id"));if(!automatic&&!hex(id)){free(id);return fail(out,"invalid_request");}sqlite3_int64 time=now_ms();if(time<0){free(id);return fail(out,"state_unavailable");}const char *error=NULL;sqlite3_stmt *s=NULL;Job j={0};
    if(automatic){char scope[65];error=inventory_scope_identity(cfg,"",scope);if(!error&&!prepare(db,&s,"SELECT id FROM " QUEUE_PREFIX "_index_jobs WHERE owner=? AND scope=? AND next_ms<=? AND (cursor<total OR (store<>'' AND (SELECT count(*) FROM " QUEUE_PREFIX "_index_windows w WHERE w.job=" QUEUE_PREFIX "_index_jobs.id AND receipt IS NOT NULL)<total)) ORDER BY turn,id LIMIT 1"))error="store_failed";if(!error){text(s,1,cfg->owner?cfg->owner:"local-owner");text(s,2,scope);sqlite3_bind_int64(s,3,time);int rc=sqlite3_step(s);if(rc==SQLITE_ROW)id=column(s,0);else if(rc!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(s);s=NULL;}
    if(!error&&!id){fputs("{\"contract_version\":1,\"processed_windows\":0,\"coverage\":\"no_due_" QUEUE_PREFIX "_windows\"}\n",out);return 0;}
    if(!error)error=load(db,cfg,id,&j);
    int pending_publication=!error&&*j.store&&j.published<j.cursor;int done=!error&&j.cursor==j.total&&!pending_publication;
    if(!error&&(done||j.next>time)){fprintf(out,"{\"contract_version\":1,\"processed_windows\":0,\"job_id\":\"%s\",\"coverage\":\"%s\"}\n",j.id,done?"completed_selected_windows":"retry_backoff");release(&j);free(id);return 0;}
    char cause[65];char *capture=NULL,*receipt=NULL,*response=NULL;size_t bytes=0;FILE *stream=NULL;
    if(!error&&pending_publication){if(!cfg->chutni_store_id||strcmp(j.store,cfg->chutni_store_id))error="" QUEUE_PREFIX "_index_configuration_changed";else error=window(db,cfg,&j,j.published,&capture,&receipt);if(!error&&receipt)error="corrupt_" QUEUE_PREFIX "_index";if(!error){stream=open_memstream(&response,&bytes);if(!stream)error="resource_limit";else{int rc=QUEUE_PUBLISH(cfg,capture,strlen(capture),db,stream);if(fclose(stream))error="resource_limit";else if(rc){error=response_error(response,bytes,cause);}else error=receipt_valid(cfg,&j,capture,response);}}if(!error)error=advance(db,cfg,&j,NULL,response);}
    else if(!error){if(!prepare(db,&s,"SELECT coalesce(sum(length(CAST(capture AS BLOB))),0) FROM (SELECT capture FROM audio_index_windows UNION ALL SELECT capture FROM visual_index_windows)"))error="store_failed";else if(sqlite3_step(s)!=SQLITE_ROW)error="store_failed";else if(sqlite3_column_int64(s,0)>QUEUE_BYTES_MAX-262144)error="" QUEUE_PREFIX "_index_limit";sqlite3_finalize(s);s=NULL;
        size_t count,start,end;if(!error)error=QUEUE_PLAN(cfg,j.end-j.start,j.cursor,&count,&start,&end);if(!error){char body[512];snprintf(body,sizeof(body),"{\"contract_version\":1,\"op\":\"" QUEUE_OP "\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu}",j.source,j.source,j.start+start,j.start+end);Json req;json_parse(&req,body,strlen(body));stream=open_memstream(&response,&bytes);if(!stream)error="resource_limit";else{int rc=QUEUE_CAPTURE(cfg,&req,db,stream);if(fclose(stream))error="resource_limit";else if(rc){error=response_error(response,bytes,cause);}else error=QUEUE_VALIDATE(cfg,response,bytes);}}if(!error)error=advance(db,cfg,&j,response,NULL);
    }
    if(error&&j.id){sqlite3_int64 finished=now_ms();if(finished>=0&&prepare(db,&s,"UPDATE " QUEUE_PREFIX "_index_jobs SET next_ms=?,failures=?,last_error=? WHERE id=?")){int level=j.failures<7?j.failures+1:7;sqlite3_int64 delay=1000ll<<level;if(delay>60000)delay=60000;sqlite3_bind_int64(s,1,finished+delay);sqlite3_bind_int(s,2,level);text(s,3,error);text(s,4,j.id);sqlite3_step(s);}sqlite3_finalize(s);}
    if(!error)fprintf(out,"{\"contract_version\":1,\"processed_windows\":1,\"job_id\":\"%s\",\"ordinal\":%zu,\"phase\":\"%s\"}\n",j.id,pending_publication?j.published:j.cursor,pending_publication?"published":"captured");
    free(capture);free(receipt);free(response);release(&j);free(id);return error?fail(out,error):0;
}
int QUEUE_DISPATCH(const SourcesConfig *cfg,const Json *request,sqlite3 *db,int directory,FILE *out){
    char *op=json_string(request,json_get(request,0,"op"));if(!op)return fail(out,"invalid_request");int lease=-1;const char *error=source_index_lease(directory,&lease);if(!error)error=schema(db);int rc=1;
    if(!error){if(!strcmp(op,"" QUEUE_PREFIX "_index_create"))rc=create(db,cfg,request,out);else if(!strcmp(op,"" QUEUE_PREFIX "_index_step")||!strcmp(op,"" QUEUE_PREFIX "_index_queue_step"))rc=step(db,cfg,request,!strcmp(op,"" QUEUE_PREFIX "_index_queue_step"),out);else{char *id=json_string(request,json_get(request,0,"job_id"));Job j={0};if(!hex(id))error="invalid_request";else error=load(db,cfg,id,&j);if(!error&&!strcmp(op,"" QUEUE_PREFIX "_index_get")){status(out,&j);rc=0;}else if(!error){size_t ordinal=0;if(!json_uint(request,json_get(request,0,"ordinal"),&ordinal)||ordinal>=j.cursor)error="invalid_range";else{char *capture=NULL,*receipt=NULL;error=window(db,cfg,&j,ordinal,&capture,&receipt);if(!error){fputs(capture,out);rc=0;}free(capture);free(receipt);}}release(&j);free(id);}}
    if(lease>=0)close(lease);
    free(op);return error?fail(out,error):rc;
}
