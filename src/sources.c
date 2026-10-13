#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "temporal.h"
#include "bound_db.h"
#include "uploads.h"
#include "control.h"
#include "json.h"
#include "sha256.h"
#include "pdf.h"
#include "ocr.h"
#include "audio.h"
#include "audio_index.h"
#include "visual_index.h"
#include "transcript.h"
#include "summary.h"
#include "visual.h"
#include "cache.h"
#include "inventory.h"
#include "index.h"
#include "locator.h"
#include "watch.h"
#include "native.h"
#include "fetch.h"
#include "search.h"
#include "chutni.h"
#include "samosa_html.h"
#include "samosa_docx.h"
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#define SNAPSHOTS_MAX 128u
#define BYTES_MAX (128u*1024u*1024u)
static const char *source_owner(const SourcesConfig *cfg){
    return cfg&&cfg->owner?cfg->owner:"local-owner";
}
static int owner_valid(const char *owner){
    if(!owner||!*owner||strlen(owner)>128)return 0;
    for(const char *p=owner;*p;p++)if(!isalnum((unsigned char)*p)&&!strchr("-_:.",*p))return 0;
    return 1;
}
static int failure(FILE *out, const char *code) {
    fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",code);
    return 1;
}
static int id_valid(const char *s) {
    if(!s || strlen(s)!=64) return 0;
    for(size_t i=0;i<64;i++) if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return 0;
    return 1;
}
static int same_file(const struct stat *a, const struct stat *b) {
#ifdef __APPLE__
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size && a->st_mtimespec.tv_sec==b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec==b->st_mtimespec.tv_nsec && a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;
#else
    return a->st_dev==b->st_dev && a->st_ino==b->st_ino && a->st_size==b->st_size && a->st_mtim.tv_sec==b->st_mtim.tv_sec && a->st_mtim.tv_nsec==b->st_mtim.tv_nsec && a->st_ctim.tv_sec==b->st_ctim.tv_sec && a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
#endif
}
/* Traverse each path component through an authorized directory descriptor.
 * O_NONBLOCK prevents a FIFO/device from blocking before regular-file checks. */ static int authorized_open(const char *root, const char *path) {
    if(!root || !path || !*path || *path=='/' || strlen(path)>=4096) return -1;
    int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) return -1;
    char *copy=strdup(path);
    if(!copy) {
        close(dir);
        return -1;
    }
    char *part=copy;
    int result=-1;
    for(;;) {
        char *slash=strchr(part,'/');
        if(slash) *slash=0;
        if(!*part || !strcmp(part,".") || !strcmp(part,"..")) break;
        int next=openat(dir,part,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK|(slash?O_DIRECTORY:0));
        if(next<0) break;
        close(dir);
        dir=next;
        if(!slash) {
            result=dir;
            dir=-1;
            break;
        }
        part=slash+1;
    }
    if(dir>=0) close(dir);
    free(copy);
    return result;
}
static void store_close(sqlite3 *db,SourceBoundDb *binding){if(binding)source_bound_close(db,binding);else sqlite3_close(db);}
static int store_open(const SourcesConfig *cfg, sqlite3 **db, int *directory,SourceBoundDb **binding) {
    *binding=NULL;
    *db=NULL;
    *directory=-1;
    if(!cfg->state || !*cfg->state) return 0;
    if(!cfg->state_pinned&&mkdir(cfg->state,0700)<0 && errno!=EEXIST) return 0;
    int dir=open(cfg->state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0) return 0;
    struct stat st;
    if(fstat(dir,&st)<0 || st.st_uid!=geteuid() || (st.st_mode&077)!=0 || (cfg->state_pinned&&((size_t)st.st_dev!=cfg->state_device||(size_t)st.st_ino!=cfg->state_inode))) {
        close(dir);
        return 0;
    }
    int rc=source_bound_open(dir,"sources.sqlite3",db,binding);

    if(rc!=SQLITE_OK) {
        if(*db)store_close(*db,*binding);
        *binding=NULL;
        *db=NULL;
        close(dir);
        return 0;
    }
    sqlite3_busy_timeout(*db,2000);
    sqlite3_limit(*db,SQLITE_LIMIT_LENGTH,2*SOURCES_INPUT_MAX);
    rc=sqlite3_exec(*db,"PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA max_page_count=40960;" "CREATE TABLE IF NOT EXISTS fetch_receipts(owner TEXT NOT NULL,id TEXT NOT NULL,url TEXT NOT NULL,format TEXT NOT NULL,response TEXT NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(owner,id));CREATE TABLE IF NOT EXISTS snapshots(id TEXT PRIMARY KEY, bytes BLOB NOT NULL, locator TEXT NOT NULL);" "CREATE TABLE IF NOT EXISTS formats(id TEXT NOT NULL,media TEXT NOT NULL,PRIMARY KEY(id,media));" "CREATE TABLE IF NOT EXISTS source_claims(owner TEXT NOT NULL,id TEXT NOT NULL,media TEXT NOT NULL,locator TEXT NOT NULL,PRIMARY KEY(owner,id,media));" "CREATE TABLE IF NOT EXISTS evidence_cache(key TEXT PRIMARY KEY,response TEXT NOT NULL,digest TEXT NOT NULL);CREATE TABLE IF NOT EXISTS prepared_artifacts(key TEXT PRIMARY KEY,response TEXT NOT NULL,digest TEXT NOT NULL);CREATE TABLE IF NOT EXISTS upload_receipts(owner TEXT NOT NULL,id TEXT PRIMARY KEY,response TEXT NOT NULL,digest TEXT NOT NULL);CREATE TABLE IF NOT EXISTS upload_chunks(owner TEXT NOT NULL,id TEXT NOT NULL,offset INTEGER NOT NULL,bytes BLOB NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(id,offset));CREATE INDEX IF NOT EXISTS upload_chunks_layout ON upload_chunks(owner,id,offset,typeof(bytes),length(bytes),typeof(digest),length(digest));CREATE TABLE IF NOT EXISTS upload_sessions(owner TEXT NOT NULL,id TEXT PRIMARY KEY,operation TEXT NOT NULL,format TEXT NOT NULL,bytes INTEGER NOT NULL,received INTEGER NOT NULL,status TEXT NOT NULL);",NULL,NULL,NULL);
    if(rc!=SQLITE_OK) {
        store_close(*db,*binding);*binding=NULL;
        *db=NULL;
        close(dir);
        return 0;
    }
    *directory=dir;
    return 1;
}
/* Shared immutable byte admission for live registrations and upload ingestion.
 * The caller retains buffer/provenance ownership throughout this call. */
int source_snapshot_accept(const SourcesConfig *cfg,sqlite3 *db,FILE *out,const unsigned char *bytes,size_t length,const char *path,int is_pdf){
    const char *err=sources_interrupted(cfg);
    if(length>SOURCES_INPUT_MAX)return failure(out,"source_too_large");
    if(!err && is_pdf==1 && (length<5||memcmp(bytes,"%PDF-",5)))err="unsupported_source";
    if(!err && is_pdf==3 && (length<4||memcmp(bytes,"PK\003\004",4)))err="unsupported_source";
    if(!err && is_pdf==6) {SourcesWave wave;err=sources_wave_inspect(cfg,bytes,length,&wave);}
    if(!err && is_pdf==7) {
        /* Admission identifies container candidates; decoding validates actual
         * codec/timeline coverage later, never while holding the store lock. */
        int candidate=(length>=3&&!memcmp(bytes,"ID3",3)) ||
            (length>=2&&bytes[0]==255&&(bytes[1]&0xe0)==0xe0) ||
            (length>=4&&(!memcmp(bytes,"fLaC",4)||!memcmp(bytes,"OggS",4))) ||
            (length>=12&&((!memcmp(bytes,"RIFF",4)&&!memcmp(bytes+8,"WAVE",4))||
                         (!memcmp(bytes,"FORM",4)&&(!memcmp(bytes+8,"AIFF",4)||!memcmp(bytes+8,"AIFC",4))))) ||
            (length>=12&&(!memcmp(bytes+4,"ftyp",4)||!memcmp(bytes+4,"moov",4)||
                         !memcmp(bytes+4,"mdat",4)||!memcmp(bytes+4,"wide",4)));
        if(!candidate)err="unsupported_source";
    }
    if(!err && is_pdf==4) {
        int raster=(length>=8&&!memcmp(bytes,"\211PNG\r\n\032\n",8)) ||
            (length>=3&&bytes[0]==255&&bytes[1]==216&&bytes[2]==255) ||
            (length>=4&&(!memcmp(bytes,"II\052\000",4)||!memcmp(bytes,"MM\000\052",4))) ||
            (length>=2&&!memcmp(bytes,"BM",2)) ||
            (length>=6&&(!memcmp(bytes,"GIF87a",6)||!memcmp(bytes,"GIF89a",6))) ||
            (length>=12&&!memcmp(bytes,"RIFF",4)&&!memcmp(bytes+8,"WEBP",4)) ||
            (length>=3&&bytes[0]=='P'&&bytes[1]>='1'&&bytes[1]<='6'&&isspace(bytes[2]));
        if(!raster)err="unsupported_source";
    }
    if(!err && is_pdf==2) {
        if(!samosa_html_sniff(bytes,length))err="unsupported_source";
        else if(!samosa_html_text_valid(bytes,length,&err)&&!err)err="invalid_utf8";
    }
    if(!err && !is_pdf && !utf8_valid(bytes,length)) err="invalid_utf8";
    char id[65];
    if(!err){
        Sha256 hash;sha256_init(&hash);
        for(size_t offset=0;offset<length&&!err;){
            err=sources_interrupted(cfg);if(err)break;
            size_t chunk=length-offset;if(chunk>65536)chunk=65536;
            sha256_update(&hash,bytes+offset,chunk);offset+=chunk;
        }
        if(!err){unsigned char digest[32];sha256_final(&hash,digest);sha256_hex(digest,id);}
    }
    int transaction=0, exists=0;
    sqlite3_stmt *stmt=NULL;
    if(!err && sqlite3_get_autocommit(db)) {
        if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK) err="store_busy";
        else transaction=1;
    }
    if(!err) {
        if(sqlite3_prepare_v2(db,"SELECT bytes FROM snapshots WHERE id=?",-1,&stmt,NULL)!=SQLITE_OK) err="store_failed";
        else {
            sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);
            int rc=sqlite3_step(stmt);
            if(rc==SQLITE_ROW) {
                exists=1;
                int saved=sqlite3_column_bytes(stmt,0);
                const void *data=sqlite3_column_blob(stmt,0);
                if(saved!=(int)length || (length && memcmp(data,bytes,length))) err="corrupt_snapshot";
            }
            else if(rc!=SQLITE_DONE) err="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    size_t count_limit=cfg->snapshot_limit?cfg->snapshot_limit:SNAPSHOTS_MAX;
    size_t byte_limit=cfg->byte_limit?cfg->byte_limit:BYTES_MAX;
    if(count_limit>SNAPSHOTS_MAX) count_limit=SNAPSHOTS_MAX;
    if(byte_limit>BYTES_MAX) byte_limit=BYTES_MAX;
    size_t reserved_count=0,reserved_bytes=0;
    if(!err&&!exists)err=source_upload_reserved(db,&reserved_count,&reserved_bytes);
    if(!err && !exists) {
        if(sqlite3_prepare_v2(db,"SELECT count(*),coalesce(sum(length(bytes)),0) FROM snapshots",-1,&stmt,NULL)!=SQLITE_OK || sqlite3_step(stmt)!=SQLITE_ROW) err="store_failed";
        else if((sqlite3_uint64)sqlite3_column_int64(stmt,0)+reserved_count>=count_limit || (sqlite3_uint64)sqlite3_column_int64(stmt,1)+reserved_bytes>byte_limit || length>byte_limit-reserved_bytes-(size_t)sqlite3_column_int64(stmt,1)) err="storage_limit";
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(!err && !exists) {
        if(sqlite3_prepare_v2(db,"INSERT INTO snapshots VALUES(?,?,?)",-1,&stmt,NULL)!=SQLITE_OK) err="store_failed";
        else {
            sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);
            sqlite3_bind_blob(stmt,2,bytes,(int)length,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,3,path,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)!=SQLITE_DONE) err="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    const char *media=is_pdf==7?"application/x-samosa-encoded-media":is_pdf==6?"audio/wav":is_pdf==4?"image/raster":is_pdf==3?"application/vnd.openxmlformats-officedocument.wordprocessingml.document":is_pdf==2?"text/html":is_pdf?"application/pdf":"text/plain";
    if(!err){
        if(sqlite3_prepare_v2(db,"SELECT count(*) FROM source_claims WHERE owner=? AND id=? AND media=?",-1,&stmt,NULL)!=SQLITE_OK)err="store_failed";
        else {
            sqlite3_bind_text(stmt,1,source_owner(cfg),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,3,media,-1,SQLITE_STATIC);
            if(sqlite3_step(stmt)!=SQLITE_ROW)err="store_failed";
            else if(!sqlite3_column_int(stmt,0)){
                sqlite3_stmt *capacity=NULL;
                if(sqlite3_prepare_v2(db,"SELECT count(*) FROM source_claims",-1,&capacity,NULL)!=SQLITE_OK||sqlite3_step(capacity)!=SQLITE_ROW)err="store_failed";
                else if(sqlite3_column_int64(capacity,0)>=256)err="claim_limit";
                sqlite3_finalize(capacity);
            }
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(!err){
        if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO source_claims(owner,id,media,locator) VALUES(?,?,?,?)",-1,&stmt,NULL)!=SQLITE_OK)err="store_failed";
        else {
            sqlite3_bind_text(stmt,1,source_owner(cfg),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,3,media,-1,SQLITE_STATIC);
            sqlite3_bind_text(stmt,4,path,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)!=SQLITE_DONE)err="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(!err) {
        if(sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO formats VALUES(?,?)",-1,&stmt,NULL)!=SQLITE_OK)err="store_failed";
        else {
            sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,media,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)!=SQLITE_DONE)err="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
    }
    if(!err)err=sources_interrupted(cfg);
    if(transaction) {
        if(err) sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
        else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK) {
            sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
            err="store_failed";
        }
    }
    if(err) return failure(out,err);
    fprintf(out,"{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\",\"bytes\":%zu,\"media_type\":\"%s\",\"identity\":\"immutable_sha256\"}\n",id,id,length,media);
    return 0;
}
int sources_upload_stream(const SourcesConfig *cfg,SourcesUploadFormat format,size_t length,SourcesUploadRead read,void *context,FILE *out){
    static const int readers[]={0,1,2,3,4,6,7};
    if(!cfg||!owner_valid(source_owner(cfg)))return failure(out,"invalid_configuration");
    if((unsigned)format>=sizeof(readers)/sizeof(readers[0])||!read)return failure(out,"invalid_request");
    if(length>SOURCES_INPUT_MAX)return failure(out,"source_too_large");
    const char *error=sources_interrupted(cfg);if(error)return failure(out,error);
    unsigned char *bytes=malloc(length+1);if(!bytes)return failure(out,"resource_limit");
    size_t offset=0;
    while(!error){
        error=sources_interrupted(cfg);if(error)break;
        size_t capacity=length+1-offset;if(capacity>65536)capacity=65536;
        size_t got=0;int failed=read(context,bytes+offset,capacity,&got);
        error=sources_interrupted(cfg);if(error)break;
        if(failed){error="upload_read_failed";break;}
        if(got>capacity){error="invalid_upload_chunk";break;}
        if(!got){if(offset!=length)error="upload_length_mismatch";break;}
        offset+=got;if(offset>length){error="upload_length_mismatch";break;}
    }
    sqlite3 *db=NULL;SourceBoundDb *binding=NULL;int directory=-1,result;
    if(error)result=failure(out,error);
    else if(!store_open(cfg,&db,&directory,&binding))result=failure(out,"state_unavailable");
    else{result=source_snapshot_accept(cfg,db,out,bytes,length,"upload:",readers[format]);store_close(db,binding);close(directory);}
    free(bytes);return result;
}
typedef struct {const unsigned char *bytes;size_t length,offset;} UploadBuffer;
static int upload_buffer_read(void *context,void *output,size_t capacity,size_t *got){
    UploadBuffer *buffer=context;*got=buffer->length-buffer->offset;if(*got>capacity)*got=capacity;
    if(*got)memcpy(output,buffer->bytes+buffer->offset,*got);
    buffer->offset+=*got;return 0;
}
int sources_upload(const SourcesConfig *cfg,SourcesUploadFormat format,const void *data,size_t length,FILE *out){
    if(!cfg||!owner_valid(source_owner(cfg)))return failure(out,"invalid_configuration");
    if(!data&&length)return failure(out,"invalid_request");
    UploadBuffer buffer={.bytes=data,.length=length};
    return sources_upload_stream(cfg,format,length,upload_buffer_read,&buffer,out);
}
int sources_fetch(const SourcesConfig *cfg,SourcesUploadFormat format,const char *url,FILE *out){
    static const int readers[]={0,1,2,3,4,6,7};
    if(!cfg||!owner_valid(source_owner(cfg)))return failure(out,"invalid_configuration");
    if((unsigned)format>=sizeof(readers)/sizeof(readers[0]))return failure(out,"invalid_request");
    unsigned char *bytes=NULL;size_t length=0;
    const char *error=sources_native_fetch(cfg,url,&bytes,&length);
    if(error)return failure(out,error);
    sqlite3 *db=NULL;SourceBoundDb *binding=NULL;int directory=-1,result;
    error=sources_interrupted(cfg);
    if(error)result=failure(out,error);
    else if(!store_open(cfg,&db,&directory,&binding))result=failure(out,"state_unavailable");
    else{result=source_snapshot_accept(cfg,db,out,bytes,length,url,readers[format]);store_close(db,binding);close(directory);}
    free(bytes);return result;
}
static int registration(const SourcesConfig *cfg, const Json *request, sqlite3 *db, FILE *out,int is_pdf) {
    char *path=json_string(request,json_get(request,0,"path"));
    if(!path) return failure(out,"invalid_request");
    int fd=authorized_open(cfg->root,path);
    if(fd<0) {
        free(path);
        return failure(out,"unauthorized_path");
    }
    struct stat before,after;
    unsigned char *bytes=NULL;
    const char *err=NULL;
    size_t length=0;
    char *expected=json_string(request,json_get(request,0,"expected_file_identity"));
    if(json_get(request,0,"expected_file_identity")>=0&&!id_valid(expected))err="invalid_request";
    if(fstat(fd,&before)<0 || !S_ISREG(before.st_mode)) err="unsupported_source";
    else if(before.st_size<0 || before.st_size>SOURCES_INPUT_MAX) err="source_too_large";
    if(!err&&expected){
        char actual[65];
        inventory_file_identity(&before,actual);
        if(strcmp(actual,expected))err="stale_source";
    }
    if(!err) {
        bytes=malloc((size_t)before.st_size+1);
        if(!bytes) err="resource_limit";
    }
    while(!err && length<=(size_t)before.st_size) {
        err=sources_interrupted(cfg);if(err)break;
        size_t wanted=(size_t)before.st_size+1-length;
        if(wanted>65536)wanted=65536;
        ssize_t got=read(fd,bytes+length,wanted);
        if(got<0) {
            if(errno==EINTR)continue;
            err="read_failed";
            break;
        }
        if(!got) break;
        length+=(size_t)got;
        if(length>(size_t)before.st_size) err="source_changed";
    }
    if(!err && (fstat(fd,&after)<0 || !same_file(&before,&after) || (off_t)length!=before.st_size)) err="source_changed";
    close(fd);
    int result=err?failure(out,err):source_snapshot_accept(cfg,db,out,bytes,length,path,is_pdf);
    free(bytes);free(path);free(expected);return result;
}
static int text_read(const SourcesConfig *cfg,const Json *request, sqlite3 *db, FILE *out) {
    char *source=json_string(request,json_get(request,0,"source_id"));
    char *version=json_string(request,json_get(request,0,"version"));
    size_t start,end;
    int valid=id_valid(source)&&id_valid(version) && json_uint(request,json_get(request,0,"start_byte"),&start) && json_uint(request,json_get(request,0,"end_byte"),&end);
    const char *err=NULL;
    sqlite3_stmt *stmt=NULL;
    if(!valid) err="invalid_request";
    else if(strcmp(source,version)) err="stale_source";
    else if(end<start || end-start>SOURCES_READ_MAX) err="invalid_range";
    if(!err) {
        if(sqlite3_prepare_v2(db,"SELECT snapshots.bytes,source_claims.locator FROM snapshots JOIN source_claims ON source_claims.id=snapshots.id WHERE snapshots.id=? AND source_claims.owner=? AND source_claims.media='text/plain'",-1,&stmt,NULL)!=SQLITE_OK) err="store_failed";
        else {
            sqlite3_bind_text(stmt,1,source,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,source_owner(cfg),-1,SQLITE_TRANSIENT);
            int rc=sqlite3_step(stmt);
            if(rc==SQLITE_DONE) err="unknown_source";
            else if(rc!=SQLITE_ROW)err="store_failed";
        }
    }
    if(!err) {
        const unsigned char *bytes=sqlite3_column_blob(stmt,0);
        size_t length=(size_t)sqlite3_column_bytes(stmt,0);
        char digest[65];
        hash_bytes(bytes,length,digest);
        if(strcmp(digest,version)||!utf8_valid(bytes,length)) err="corrupt_snapshot";
        else if(start>length||end>length) err="invalid_range";
        else if((start<length && (bytes[start]&192)==128)||(end<length && (bytes[end]&192)==128)) err="utf8_boundary";
        else {
            char selector[256],evidence[65],fingerprint[65];
            hash_bytes("text-utf8-v1:C11:strict-utf8:exclusive-byte-range",sizeof("text-utf8-v1:C11:strict-utf8:exclusive-byte-range")-1,fingerprint);
            int n=snprintf(selector,sizeof(selector),"text-utf8-v1:%s:%s:%zu:%zu",fingerprint,version,start,end);
            hash_bytes(selector,(size_t)n,evidence);
            fprintf(out,"{\"contract_version\":1,\"evidence_id\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"kind\":\"literal\",\"text\":",evidence,source,version);
            json_quote(out,bytes?bytes+start:(const unsigned char*)"",end-start);
            fprintf(out,",\"range\":{\"start_byte\":%zu,\"end_byte\":%zu},\"coverage\":{\"status\":\"%s\",\"total_bytes\":%zu,\"inspected_bytes\":%zu},\"provenance\":{\"reader\":\"text-utf8-v1\",\"locator\":",start,end,start==0&&end==length?"complete":"partial",length,end-start);
            const char *locator=(const char*)sqlite3_column_text(stmt,1);
            json_quote(out,locator,strlen(locator));
            fprintf(out,",\"extractor_fingerprint\":\"%s\"",fingerprint);
            fprintf(out,"}}\n");
        }
    }
    sqlite3_finalize(stmt);
    free(source);
    free(version);
    return err?failure(out,err):0;
}
void sources_html_fingerprint(char out[65]) {
    static const char contract[]="html-readable-v1:C11:strict-utf8:skip-active-content:decoded-exclusive-byte-range:65536";
    hash_bytes(contract,sizeof(contract)-1,out);
}
void sources_docx_fingerprint(char out[65]) {
    static const char contract[]="docx-readable-v1:C11:miniz-read:strict-xml:literal-part-join:8MiB-expanded:1MiB-entry:2048-entries:200-ratio:65536-output:decoded-byte-range";
    hash_bytes(contract,sizeof(contract)-1,out);
}
static int document_read(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out,int format) {
    int docx=format==3,image=format==4;
    const char *reader=image?"image-ocr-v1":docx?"docx-readable-v1":"html-readable-v1";
    const char *media=image?"image/raster":docx?"application/vnd.openxmlformats-officedocument.wordprocessingml.document":"text/html";
    char *source=json_string(request,json_get(request,0,"source_id")),*version=json_string(request,json_get(request,0,"version")),*locator=NULL;
    unsigned char *snapshot=NULL;size_t length=0,start=0,end=0;const char *error=NULL;sqlite3_stmt *stmt=NULL;
    if(!id_valid(source)||!id_valid(version)||!json_uint(request,json_get(request,0,"start_byte"),&start)||!json_uint(request,json_get(request,0,"end_byte"),&end))error="invalid_request";
    else if(strcmp(source,version))error="stale_source";
    else if(end<start||end-start>SOURCES_READ_MAX)error="invalid_range";
    if(!error) {
        if(sqlite3_prepare_v2(db,"SELECT snapshots.bytes,source_claims.locator FROM snapshots JOIN source_claims ON source_claims.id=snapshots.id WHERE snapshots.id=? AND source_claims.owner=? AND source_claims.media=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
        else {
            sqlite3_bind_text(stmt,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,source_owner(cfg),-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,media,-1,SQLITE_STATIC);int rc=sqlite3_step(stmt);
            if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";
            else {
                int size=sqlite3_column_bytes(stmt,0);const void *bytes=sqlite3_column_blob(stmt,0);const char *path=(const char*)sqlite3_column_text(stmt,1);
                if(size<=0||size>(int)SOURCES_INPUT_MAX||!bytes||!path)error="corrupt_snapshot";
                else {length=(size_t)size;snapshot=malloc(length);locator=strdup(path);if(!snapshot||!locator)error="resource_limit";else memcpy(snapshot,bytes,length);}
            }
        }
        sqlite3_finalize(stmt);
    }
    SourcesOcrResult raster={0};SamosaHtmlResult result={0};SamosaDocxResult word={0};char fingerprint[65],evidence[65];
    if(!error){char digest[65];hash_bytes(snapshot,length,digest);if(strcmp(digest,version))error="corrupt_snapshot";}
    if(!error)error=sources_interrupted(cfg);
    if(!error) {
        int ok=image?!(error=sources_ocr_extract(cfg,snapshot,length,&raster)):docx?samosa_docx_extract(snapshot,length,SOURCES_READ_MAX,&word,&error):samosa_html_extract(snapshot,length,SOURCES_READ_MAX,&result,&error);
        if(!ok&&!error)error="document_extraction_failed";
    }
    if(!error)error=sources_interrupted(cfg);
    if(!error) {
        const unsigned char *text=(const unsigned char*)(image?raster.text:docx?word.text:result.text);size_t readable=image?raster.text_bytes:docx?word.text_bytes:result.text_bytes;
        if(start>readable||end>readable)error="invalid_range";
        else if((start<readable&&(text[start]&192)==128)||(end<readable&&(text[end]&192)==128))error="utf8_boundary";
        else {
            char selector[256];if(image)memcpy(fingerprint,raster.fingerprint,65);else if(docx)sources_docx_fingerprint(fingerprint);else sources_html_fingerprint(fingerprint);int n=snprintf(selector,sizeof(selector),"%s:%s:%s:%zu:%zu",reader,fingerprint,version,start,end);hash_bytes(selector,(size_t)n,evidence);
            fprintf(out,"{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\",\"kind\":\"literal\",\"text\":",source,version,evidence);json_quote(out,text+start,end-start);
            fprintf(out,",\"range\":{\"start_byte\":%zu,\"end_byte\":%zu,\"coordinate_space\":\"extracted_utf8\"},\"coverage\":{\"status\":\"partial\",\"reason\":\"%s\",\"total_bytes\":%zu,\"inspected_bytes\":%zu},\"provenance\":{\"reader\":\"%s\",\"extractor_fingerprint\":\"%s\",\"locator\":",start,end,image?"ocr_recognition_uncertain":docx?"ooxml_readable_text_only":"static_readable_text_only",readable,end-start,reader,fingerprint);json_quote(out,locator,strlen(locator));if(image)fprintf(out,",\"recognition\":{\"method\":\"tesseract\",\"confidence\":%d,\"raster\":{\"width\":%u,\"height\":%u,\"coordinate_space\":\"original_pixels\"}}",raster.confidence,raster.width,raster.height);fputs("}}\n",out);
        }
    }
    sources_ocr_free(&raster);samosa_html_result_free(&result);samosa_docx_result_free(&word);free(snapshot);free(locator);free(source);free(version);return error?failure(out,error):0;
}
static int pdf_read(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out,int ocr) {
    char *source=json_string(request,json_get(request,0,"source_id"));
    char *version=json_string(request,json_get(request,0,"version"));
    size_t start=0,end=0;
    const char *err=NULL;
    sqlite3_stmt *stmt=NULL;
    if(!id_valid(source)||!id_valid(version)||!json_uint(request,json_get(request,0,"start_page"),&start)||!json_uint(request,json_get(request,0,"end_page"),&end))err="invalid_request";
    else if(strcmp(source,version))err="stale_source";
    else if(start<1||end<start||end-start>=4)err="invalid_range";
    if(!err) {
        if(sqlite3_prepare_v2(db,"SELECT snapshots.bytes,source_claims.locator FROM snapshots JOIN source_claims ON source_claims.id=snapshots.id WHERE snapshots.id=? AND source_claims.owner=? AND source_claims.media='application/pdf'",-1,&stmt,NULL)!=SQLITE_OK)err="store_failed";
        else {
            sqlite3_bind_text(stmt,1,source,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,source_owner(cfg),-1,SQLITE_TRANSIENT);
            int rc=sqlite3_step(stmt);
            if(rc==SQLITE_DONE)err="unknown_source";
            else if(rc!=SQLITE_ROW)err="store_failed";
        }
    }
    /* A native reader may block between cancellation checks. Release SQLite's
     * read transaction before extraction so unrelated readers can commit their
     * caches. Retain a bounded immutable copy, never SQLite-owned pointers. */
    void *snapshot=NULL;char *locator=NULL;size_t n=0;
    if(!err) {
        const void *bytes=sqlite3_column_blob(stmt,0);
        int stored=sqlite3_column_bytes(stmt,0);
        const char *origin=(const char*)sqlite3_column_text(stmt,1);
        if(stored<=0||(size_t)stored>SOURCES_INPUT_MAX||!bytes||!origin)err="corrupt_snapshot";
        else {
            n=(size_t)stored;snapshot=malloc(n);locator=strdup(origin);
            if(!snapshot||!locator)err="resource_limit";
            else memcpy(snapshot,bytes,n);
        }
    }
    sqlite3_finalize(stmt);
    if(!err) {
        char hash[65];hash_bytes(snapshot,n,hash);
        if(strcmp(hash,version))err="corrupt_snapshot";
        else err=ocr?pdf_ocr_extract(cfg,snapshot,n,source,version,start,end,locator,out):pdf_extract(cfg,cfg->pdfium_library,snapshot,n,source,version,start,end,locator,out);
    }
    free(snapshot);free(locator);
    free(source);
    free(version);
    return err?failure(out,err):0;
}
#define CACHE_COUNT_MAX 128u
#define CACHE_BYTES_MAX (8u*1048576u)
#define CACHE_RESPONSE_MAX (2u*1048576u)
const char *sources_cache_capacity(sqlite3 *db,const SourcesConfig *cfg,size_t extra) {
    size_t max_count=cfg->cache_limit?cfg->cache_limit:CACHE_COUNT_MAX;
    size_t max_bytes=cfg->cache_byte_limit?cfg->cache_byte_limit:CACHE_BYTES_MAX;
    if(max_count>CACHE_COUNT_MAX||max_bytes>CACHE_BYTES_MAX)return "invalid_configuration";
    sqlite3_stmt *stmt=NULL;
    const char *error=NULL;
    if(sqlite3_prepare_v2(db,"SELECT count(*),coalesce(sum(length(CAST(response AS BLOB))),0) FROM (SELECT response FROM evidence_cache UNION ALL SELECT response FROM prepared_artifacts)",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
    else if(sqlite3_step(stmt)!=SQLITE_ROW)error="store_failed";
    else {
        sqlite3_int64 count=sqlite3_column_int64(stmt,0),bytes=sqlite3_column_int64(stmt,1);
        if(count<0||bytes<0)error="corrupt_cache";
        else if((size_t)count>=max_count||(size_t)bytes>max_bytes||extra>max_bytes-(size_t)bytes)error="cache_limit";
    }
    sqlite3_finalize(stmt);
    return error;
}
static const char *cache_lookup(sqlite3 *db,const char *key,const char *evidence,char **response,size_t *length) {
    *response=NULL;
    *length=0;
    sqlite3_stmt *stmt=NULL;
    const char *error=NULL;
    if(sqlite3_prepare_v2(db,"SELECT response,digest FROM evidence_cache WHERE key=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
    else {
        sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);
        int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            const char *value=(const char*)sqlite3_column_text(stmt,0),*digest=(const char*)sqlite3_column_text(stmt,1);
            size_t n=(size_t)sqlite3_column_bytes(stmt,0);
            char actual[65];
            hash_bytes(value,n,actual);
            Json j;
            if(!value||!digest||n>CACHE_RESPONSE_MAX||strlen(value)!=n||strcmp(actual,digest)||!json_parse(&j,value,n)||j.tokens[0].type!=JT_OBJECT||json_get(&j,0,"error")>=0)error="corrupt_cache";
            else {
                char *identity=json_string(&j,json_get(&j,0,"evidence_id")),*kind=json_string(&j,json_get(&j,0,"kind"));
                size_t contract=0;
                if(!identity||strcmp(identity,evidence)||!kind||strcmp(kind,"literal")||!json_uint(&j,json_get(&j,0,"contract_version"),&contract)||contract!=1)error="corrupt_cache";
                else {
                    *response=malloc(n+1);
                    if(!*response)error="resource_limit";
                    else {
                        memcpy(*response,value,n+1);
                        *length=n;
                    }
                }
                free(identity);
                free(kind);
            }
        }
        else if(rc!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);
    return error;
}
static const char *cache_put(sqlite3 *db,const SourcesConfig *cfg,const char *key,const char *evidence,const char *response,size_t length) {
    if(length>CACHE_RESPONSE_MAX)return "cache_limit";
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return "store_busy";
    char *existing=NULL;
    size_t old_length=0;
    const char *error=cache_lookup(db,key,evidence,&existing,&old_length);
    if(!error&&existing){
        if(old_length!=length||memcmp(existing,response,length))error="cache_conflict";
    }
    else if(!error){
        error=sources_cache_capacity(db,cfg,length);
        sqlite3_stmt *stmt=NULL;
        if(!error){
            char digest[65];
            hash_bytes(response,length,digest);
            if(sqlite3_prepare_v2(db,"INSERT INTO evidence_cache(key,response,digest) VALUES(?,?,?)",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
            else {
                sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,2,response,(int)length,SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt,3,digest,-1,SQLITE_TRANSIENT);
                if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
            }
            sqlite3_finalize(stmt);
        }
    }
    free(existing);
    if(!error)error=sources_interrupted(cfg);
    if(error)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){
        sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
        error="store_failed";
    }
    return error;
}
/* Validate source bytes and selector before trusting any cached result. */ static const char *cache_key(sqlite3 *db,const SourcesConfig *cfg,const Json *request,int format,char key[65],char fingerprint[65],char evidence[65]) {
    int pdf=format==1||format==5,html=format==2,docx=format==3,image=format==4,pdf_ocr=format==5;
    char *source=json_string(request,json_get(request,0,"source_id")),*version=json_string(request,json_get(request,0,"version"));
    size_t start=0,end=0;
    const char *error=NULL;
    sqlite3_stmt *stmt=NULL;
    if(!id_valid(source)||!id_valid(version)||!json_uint(request,json_get(request,0,pdf?"start_page":"start_byte"),&start)||!json_uint(request,json_get(request,0,pdf?"end_page":"end_byte"),&end))error="invalid_request";
    else if(strcmp(source,version))error="stale_source";
    else if(end<start||(pdf?(start<1||end-start>=4):end-start>SOURCES_READ_MAX))error="invalid_range";
    if(!error){
        if(sqlite3_prepare_v2(db,"SELECT snapshots.bytes FROM snapshots JOIN source_claims ON source_claims.id=snapshots.id WHERE snapshots.id=? AND source_claims.media=? AND source_claims.owner=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
        else {
            sqlite3_bind_text(stmt,1,source,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,image?"image/raster":docx?"application/vnd.openxmlformats-officedocument.wordprocessingml.document":html?"text/html":pdf?"application/pdf":"text/plain",-1,SQLITE_STATIC);
            sqlite3_bind_text(stmt,3,source_owner(cfg),-1,SQLITE_TRANSIENT);
            int rc=sqlite3_step(stmt);
            if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";
            else {
                const unsigned char *bytes=sqlite3_column_blob(stmt,0);
                size_t length=(size_t)sqlite3_column_bytes(stmt,0);
                char digest[65];
                hash_bytes(bytes,length,digest);
                if(strcmp(digest,version)||(!pdf&&!docx&&!image&&!utf8_valid(bytes,length)))error="corrupt_snapshot";
                else if(!pdf&&!html&&!docx&&!image&&(start>length||end>length))error="invalid_range";
                else if(!pdf&&!html&&!docx&&!image&&((start<length&&(bytes[start]&192)==128)||(end<length&&(bytes[end]&192)==128)))error="utf8_boundary";
            }
        }
        sqlite3_finalize(stmt);
    }
    if(!error){
        if(pdf_ocr)error=pdf_ocr_fingerprint(cfg,fingerprint);
        else if(image)error=sources_ocr_fingerprint(cfg,fingerprint);
        else if(pdf)error=pdf_fingerprint(cfg->pdfium_library,fingerprint);
        else if(html)sources_html_fingerprint(fingerprint);
        else if(docx)sources_docx_fingerprint(fingerprint);
        else hash_bytes("text-utf8-v1:C11:strict-utf8:exclusive-byte-range",sizeof("text-utf8-v1:C11:strict-utf8:exclusive-byte-range")-1,fingerprint);
    }
    if(!error){
        char selector[256];
        int n=snprintf(selector,sizeof(selector),"%s:%s:%s:%zu:%zu",pdf_ocr?"pdfium-ocr-v1":image?"image-ocr-v1":docx?"docx-readable-v1":html?"html-readable-v1":pdf?"pdfium-text-v1":"text-utf8-v1",fingerprint,version,start,end);
        if(n<0||(size_t)n>=sizeof(selector))error="resource_limit";
        else {
            hash_bytes(selector,(size_t)n,evidence);
            Sha256 hash;
            unsigned char sum[32];
            sha256_init(&hash);
            sha256_update(&hash,"source-owner-cache-v1",sizeof("source-owner-cache-v1"));
            const char *owner=source_owner(cfg);
            sha256_update(&hash,owner,strlen(owner)+1);
            sha256_update(&hash,evidence,64);
            sha256_final(&hash,sum);
            sha256_hex(sum,key);
        }
    }
    free(source);
    free(version);
    return error;
}
static int cached_read(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out,int pdf) {
    if(cfg->cache_limit>CACHE_COUNT_MAX||cfg->cache_byte_limit>CACHE_BYTES_MAX)return failure(out,"invalid_configuration");
    char key[65],fingerprint[65],evidence[65],*response=NULL;
    size_t length=0;
    const char *error=cache_key(db,cfg,request,pdf,key,fingerprint,evidence);
    if(!error)error=sources_interrupted(cfg);
    if(!error)error=cache_lookup(db,key,evidence,&response,&length);
    if(!error&&!response){
        if((pdf==1||pdf==4||pdf==5)&&!cfg->native_helper)error="native_helper_unavailable";
        if(!error)
        error=sources_cache_capacity(db,cfg,0);
        if(!error){
            FILE *stream=open_memstream(&response,&length);
            if(!stream)error="resource_limit";
            else {
                int rc=pdf==5?pdf_read(cfg,request,db,stream,1):pdf>=2?document_read(cfg,request,db,stream,pdf):pdf?pdf_read(cfg,request,db,stream,0):text_read(cfg,request,db,stream);
                int closed=fclose(stream);
                if(closed)error="resource_limit";
                else if(rc){
                    fwrite(response,1,length,out);
                    free(response);
                    return 1;
                }
                else {
                    Json j;
                    if(!json_parse(&j,response,length))error="invalid_evidence";
                    else {
                        char *actual=json_string(&j,json_get(&j,json_get(&j,0,"provenance"),"extractor_fingerprint"));
                        if(!actual||strcmp(actual,fingerprint))error="pdf_adapter_changed";
                        free(actual);
                    }
                }
            }
        }
        if(!error)error=cache_put(db,cfg,key,evidence,response,length);
    }
    if(!error)error=sources_interrupted(cfg);
    if(!error){
        fwrite(response,1,length,out);
    }
    free(response);
    return error?failure(out,error):0;
}
const char *sources_literal_retained_validate(sqlite3 *db,const SourcesConfig *cfg,const char *body,size_t bytes){
    Json j;size_t contract=0,start=0,end=0;
    if(!body||bytes>524288||!json_parse(&j,body,bytes)||j.tokens[0].type!=JT_OBJECT||!json_uint(&j,json_get(&j,0,"contract_version"),&contract)||contract!=1)return "corrupt_index";
    int p=json_get(&j,0,"provenance"),range=json_get(&j,0,"range");
    char *source=json_string(&j,json_get(&j,0,"source_id")),*version=json_string(&j,json_get(&j,0,"version")),*eid=json_string(&j,json_get(&j,0,"evidence_id")),*kind=json_string(&j,json_get(&j,0,"kind")),*reader=json_string(&j,json_get(&j,p,"reader")),*fingerprint=json_string(&j,json_get(&j,p,"extractor_fingerprint"));
    int fallback=reader&&!strcmp(reader,"pdfium-index-fallback-v1"),pdf=reader&&(!strcmp(reader,"pdfium-text-v1")||!strcmp(reader,"pdfium-ocr-v1")||fallback);
    const char *media=pdf?"application/pdf":reader&&!strcmp(reader,"text-utf8-v1")?"text/plain":reader&&!strcmp(reader,"html-readable-v1")?"text/html":reader&&!strcmp(reader,"docx-readable-v1")?"application/vnd.openxmlformats-officedocument.wordprocessingml.document":reader&&!strcmp(reader,"image-ocr-v1")?"image/raster":NULL;
    const char *error=sources_interrupted(cfg);
    if(!error&&(!id_valid(source)||!version||strcmp(version,source)||!id_valid(eid)||!kind||strcmp(kind,"literal")||!media||!id_valid(fingerprint)||range<0||!json_uint(&j,json_get(&j,range,pdf?"start_page":"start_byte"),&start)||!json_uint(&j,json_get(&j,range,pdf?"end_page":"end_byte"),&end)||end<start||(pdf?(start<1||end-start>=4):end-start>SOURCES_READ_MAX)))error="corrupt_index";
    if(!error){char selector[256],expected[65];int n=snprintf(selector,sizeof(selector),"%s:%s:%s:%zu:%zu",reader,fingerprint,source,start,end);if(n<0||(size_t)n>=sizeof(selector))error="corrupt_index";else{hash_bytes(selector,(size_t)n,expected);if(fallback){
        int layer=json_get(&j,p,"text_layer_capture"),ocr=json_get(&j,p,"ocr_captures");
        if(layer<0||j.tokens[layer].type!=JT_OBJECT||ocr<0||j.tokens[ocr].type!=JT_ARRAY||j.tokens[ocr].count<1||j.tokens[ocr].count>4||strcmp(eid,fingerprint))error="corrupt_index";
        else {Sha256 h;unsigned char digest[32];sha256_init(&h);sha256_update(&h,"pdfium-index-fallback-v1:",25);sha256_update(&h,j.text+j.tokens[layer].begin,j.tokens[layer].end-j.tokens[layer].begin);for(int t=ocr+1;t<j.tokens[ocr].next;t=j.tokens[t].next)sha256_update(&h,j.text+j.tokens[t].begin,j.tokens[t].end-j.tokens[t].begin);sha256_final(&h,digest);sha256_hex(digest,expected);if(strcmp(expected,eid))error="corrupt_index";}
    }else if(strcmp(expected,eid))error="corrupt_index";}}
    char *literal=NULL;if(!error){literal=source_index_literal(body,bytes);if(!literal||strlen(literal)>SOURCES_READ_MAX||!utf8_valid((const unsigned char*)literal,strlen(literal))||(!pdf&&strlen(literal)!=end-start))error="corrupt_index";}
    sqlite3_stmt *stmt=NULL;
    if(!error&&sqlite3_prepare_v2(db,"SELECT snapshots.bytes FROM snapshots JOIN source_claims c ON c.id=snapshots.id WHERE c.owner=? AND c.id=? AND c.media=? LIMIT 1",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
    if(!error){sqlite3_bind_text(stmt,1,source_owner(cfg),-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,media,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";else{int n=sqlite3_column_bytes(stmt,0);const void *data=sqlite3_column_blob(stmt,0);char digest[65];if(n<0||(size_t)n>SOURCES_INPUT_MAX||sqlite3_column_type(stmt,0)!=SQLITE_BLOB||(n&&!data))error="corrupt_snapshot";else{hash_bytes(data,(size_t)n,digest);if(strcmp(digest,source))error="corrupt_snapshot";else if(!strcmp(reader,"text-utf8-v1")&&(end>(size_t)n||(end>start&&memcmp((const unsigned char*)data+start,literal,end-start))))error="corrupt_index";}}}
    sqlite3_finalize(stmt);free(source);free(version);free(eid);free(kind);free(reader);free(fingerprint);free(literal);return error;
}
const char *sources_literal_capture(sqlite3 *db,const SourcesConfig *cfg,const char *source,const char *evidence,char **body,size_t *bytes){
    *body=NULL;*bytes=0;
    if(!id_valid(source)||!id_valid(evidence))return "invalid_request";
    const char *error=sources_interrupted(cfg);sqlite3_stmt *stmt=NULL;
    if(!error&&sqlite3_prepare_v2(db,"SELECT snapshots.bytes FROM snapshots JOIN source_claims ON source_claims.id=snapshots.id WHERE snapshots.id=? AND source_claims.owner=? LIMIT 1",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
    if(!error){sqlite3_bind_text(stmt,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,source_owner(cfg),-1,SQLITE_TRANSIENT);int rc=sqlite3_step(stmt);
        if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";
        else{const void *data=sqlite3_column_blob(stmt,0);int n=sqlite3_column_bytes(stmt,0);char digest[65];if(n<0||(n&&!data)||(size_t)n>SOURCES_INPUT_MAX)error="corrupt_snapshot";else{hash_bytes(data,(size_t)n,digest);if(strcmp(digest,source))error="corrupt_snapshot";}}}
    sqlite3_finalize(stmt);
    if(!error){Sha256 hash;unsigned char sum[32];char key[65];const char *owner=source_owner(cfg);sha256_init(&hash);sha256_update(&hash,"source-owner-cache-v1",sizeof("source-owner-cache-v1"));sha256_update(&hash,owner,strlen(owner)+1);sha256_update(&hash,evidence,64);sha256_final(&hash,sum);sha256_hex(sum,key);error=cache_lookup(db,key,evidence,body,bytes);if(!error&&!*body)error="unknown_evidence";}
    if(!error){Json j;if(!json_parse(&j,*body,*bytes))error="corrupt_cache";else{
        int p=json_get(&j,0,"provenance"),range=json_get(&j,0,"range");char *actual_source=json_string(&j,json_get(&j,0,"source_id")),*version=json_string(&j,json_get(&j,0,"version")),*reader=json_string(&j,json_get(&j,p,"reader")),*fingerprint=json_string(&j,json_get(&j,p,"extractor_fingerprint"));
        int pdf=reader&&(!strcmp(reader,"pdfium-text-v1")||!strcmp(reader,"pdfium-ocr-v1"));size_t start=0,end=0;
        int known=reader&&(!strcmp(reader,"text-utf8-v1")||!strcmp(reader,"html-readable-v1")||!strcmp(reader,"docx-readable-v1")||!strcmp(reader,"image-ocr-v1")||pdf);
        if(!actual_source||strcmp(actual_source,source)||!version||strcmp(version,source)||!known||!id_valid(fingerprint)||range<0||j.tokens[range].type!=JT_OBJECT||!json_uint(&j,json_get(&j,range,pdf?"start_page":"start_byte"),&start)||!json_uint(&j,json_get(&j,range,pdf?"end_page":"end_byte"),&end)||end<start||(pdf?(start<1||end-start>=4):end-start>SOURCES_READ_MAX))error="corrupt_cache";
        if(!error){char selector[256],identity[65];int n=snprintf(selector,sizeof(selector),"%s:%s:%s:%zu:%zu",reader,fingerprint,source,start,end);if(n<0||(size_t)n>=sizeof(selector))error="corrupt_cache";else{hash_bytes(selector,(size_t)n,identity);if(strcmp(identity,evidence))error="corrupt_cache";}}
        free(actual_source);free(version);free(reader);free(fingerprint);
    }}
    if(!error)error=sources_interrupted(cfg);
    if(error){free(*body);*body=NULL;*bytes=0;}return error;
}
const char *sources_watch_admit(const SourcesConfig *cfg,const char *id,int *directory,int *runtime){
    sqlite3 *db=NULL;SourceBoundDb *binding=NULL;*directory=*runtime=-1;
    if(!cfg||!owner_valid(source_owner(cfg))||!store_open(cfg,&db,directory,&binding))return "state_unavailable";
    const char *error=source_watch_admit(cfg,db,*directory,id,runtime);store_close(db,binding);
    if(error){close(*directory);*directory=-1;}return error;
}
const char *sources_queue_admit(const SourcesConfig *cfg,int *directory,int *runtime){
    sqlite3 *db=NULL;SourceBoundDb *binding=NULL;*directory=*runtime=-1;
    if(!cfg||!owner_valid(source_owner(cfg))||!store_open(cfg,&db,directory,&binding))return "state_unavailable";
    store_close(db,binding);const char *error=sources_queue_create_lease(*directory,runtime);
    if(error){close(*directory);*directory=-1;}return error;
}
const char *sources_watch_record(const SourcesConfig *cfg,const char *id,const char *failure_code){
    sqlite3 *db=NULL;SourceBoundDb *binding=NULL;int directory=-1;
    if(!cfg||!owner_valid(source_owner(cfg))||!store_open(cfg,&db,&directory,&binding))return "state_unavailable";
    const char *error=source_watch_health(cfg,db,id,failure_code);
    store_close(db,binding);close(directory);return error;
}
int sources_dispatch_core(const SourcesConfig *cfg,const char *input,size_t length,FILE *out) {
    if(length>SOURCES_REQUEST_MAX) return failure(out,"request_too_large");
    Json request;
    size_t version;
    if(!json_parse(&request,input,length)||request.tokens[0].type!=JT_OBJECT) return failure(out,"invalid_json");
    if(!json_uint(&request,json_get(&request,0,"contract_version"),&version)||version!=1) return failure(out,"unsupported_contract");
    char *op=json_string(&request,json_get(&request,0,"op"));
    if(!op)return failure(out,"invalid_request");
    const char *fields=!strcmp(op,"index_embedding_status")?"|op|contract_version|job_id|after|":!strcmp(op,"index_embed_queue_step")?"|op|contract_version|":!strcmp(op,"index_embed_step")?"|op|contract_version|job_id|":!strcmp(op,"index_enrichment_status")?"|op|contract_version|job_id|after|":!strcmp(op,"index_enrich_queue_step")?"|op|contract_version|":!strcmp(op,"index_enrich_step")?"|op|contract_version|job_id|":(!strcmp(op,"summarize_evidence")||!strcmp(op,"summarize_evidence_opening")||!strcmp(op,"publish_summary"))?"|op|contract_version|source_id|version|evidence_id|":!strcmp(op,"memory_search_semantic")?"|op|contract_version|vector|":(!strcmp(op,"analyze_video")||!strcmp(op,"analyze_visual")||!strcmp(op,"publish_visual"))?"|op|contract_version|source_id|version|start_ms|end_ms|prompt|":(!strcmp(op,"index_audio_search")||!strcmp(op,"index_visual_search"))?"|op|contract_version|job_id|query|":!strcmp(op,"index_visual_read")?"|op|contract_version|job_id|visual_job_id|ordinal|":!strcmp(op,"index_audio_read")?"|op|contract_version|job_id|audio_job_id|ordinal|":(!strcmp(op,"index_audio_history")||!strcmp(op,"index_visual_history"))?"|op|contract_version|job_id|after|":!strcmp(op,"audio_index_create")?"|op|contract_version|source_id|version|start_ms|end_ms|":!strcmp(op,"audio_index_queue_step")?"|op|contract_version|":!strcmp(op,"audio_index_read")?"|op|contract_version|job_id|ordinal|":(!strcmp(op,"audio_index_step")||!strcmp(op,"audio_index_get"))?"|op|contract_version|job_id|":!strcmp(op,"memory_read")?"|op|contract_version|store_id|external_source_id|external_artifact_id|start_byte|end_byte|":(!strcmp(op,"search")||!strcmp(op,"memory_search")||!strcmp(op,"memory_search_text"))?"|op|contract_version|query|":!strcmp(op,"fetch")?"|op|contract_version|acquisition_id|url|format|":!strcmp(op,"upload_append")?"|op|contract_version|upload_id|offset|data|":!strcmp(op,"upload_begin")?"|op|contract_version|operation_id|format|bytes|":(!strcmp(op,"upload_status")||!strcmp(op,"upload_abort")||!strcmp(op,"upload_commit"))?"|op|contract_version|upload_id|":!strcmp(op,"describe")?"|op|contract_version|":(!strcmp(op,"register_text")||!strcmp(op,"register_pdf")||!strcmp(op,"register_html")||!strcmp(op,"register_docx")||!strcmp(op,"register_image")||!strcmp(op,"register_audio")||!strcmp(op,"register_media"))?"|op|contract_version|path|expected_file_identity|":(!strcmp(op,"publish_transcript")||!strcmp(op,"transcribe_audio")||!strcmp(op,"prepare_audio")||!strcmp(op,"prepare_visual"))?"|op|contract_version|source_id|version|start_ms|end_ms|":!strcmp(op,"locator_reconcile")?"|op|contract_version|operation_id|locators|":!strcmp(op,"locator_watch_cleanup")?"|op|contract_version|":!strcmp(op,"locator_watch_create")?"|op|contract_version|operation_id|locators|":!strcmp(op,"locator_watch_step")?"|op|contract_version|watch_id|expected_cycle|":(!strcmp(op,"locator_watch_get")||!strcmp(op,"locator_watch_stop")||!strcmp(op,"locator_watch_remove"))?"|op|contract_version|watch_id|":!strcmp(op,"locator_read")?"|op|contract_version|path|expected_device|expected_inode|read|":!strcmp(op,"locator_get")?"|op|contract_version|locator_id|":(!strcmp(op,"index_queue_step")||!strcmp(op,"index_refresh_step")||!strcmp(op,"index_publish_queue_step"))?"|op|contract_version|":!strcmp(op,"index_publication_status")?"|op|contract_version|job_id|after|":!strcmp(op,"index_discover")?"|op|contract_version|job_id|operation_id|":!strcmp(op,"index_create")?"|op|contract_version|path|operation_id|":(!strcmp(op,"index_step")||!strcmp(op,"index_reindex_step"))?"|op|contract_version|job_id|operation_id|max_items|":(!strcmp(op,"index_get")||!strcmp(op,"index_pending")||!strcmp(op,"index_publish_step"))?"|op|contract_version|job_id|":!strcmp(op,"index_search")?"|op|contract_version|job_id|query|":!strcmp(op,"inventory")?"|op|contract_version|path|":(!strcmp(op,"read_text")||!strcmp(op,"read_html")||!strcmp(op,"read_docx")||!strcmp(op,"read_image"))?"|op|contract_version|source_id|version|start_byte|end_byte|":(!strcmp(op,"read_pdf")||!strcmp(op,"read_pdf_ocr"))?"|op|contract_version|source_id|version|start_page|end_page|":NULL;
    if(!strncmp(op,"visual_index_",13))fields=!strcmp(op,"visual_index_create")?"|op|contract_version|source_id|version|start_ms|end_ms|":!strcmp(op,"visual_index_queue_step")?"|op|contract_version|":!strcmp(op,"visual_index_read")?"|op|contract_version|job_id|ordinal|":(!strcmp(op,"visual_index_get")||!strcmp(op,"visual_index_step"))?"|op|contract_version|job_id|":NULL;
    if(!strcmp(op,"inspect_audio")||!strcmp(op,"inspect_visual"))fields="|op|contract_version|source_id|version|";
    if(!fields) {
        free(op);
        return failure(out,"unsupported_operation");
    }
    for(int t=1;t<request.tokens[0].next;) {
        char *key=json_string(&request,t);
        char match[128];
        int n=snprintf(match,sizeof(match),"|%s|",key?key:"");
        if(key&&strchr(key,'|'))n=-1;
        free(key);
        if(n<0 || (size_t)n>=sizeof(match)||!strstr(fields,match)) {
            free(op);
            return failure(out,"unknown_field");
        }
        t=request.tokens[request.tokens[t].next].next;
    }
    if(!strcmp(op,"describe")) {
        fputs("{\"contract_version\":1,\"component\":\"samosa-sources\",\"version\":\"" SOURCES_VERSION "\",\"operations\":[\"index_embedding_status\",\"index_embed_step\",\"index_embed_queue_step\",\"index_enrichment_status\",\"index_enrich_step\",\"index_enrich_queue_step\",\"summarize_evidence\",\"summarize_evidence_opening\",\"publish_summary\",\"memory_read\",\"memory_search\",\"memory_search_semantic\",\"memory_search_text\",\"search\",\"fetch\",\"upload_begin\",\"upload_status\",\"upload_append\",\"upload_abort\",\"upload_commit\",\"operation_get\",\"operation_cancel\",\"locator_reconcile\",\"locator_get\",\"locator_read\",\"locator_watch_create\",\"locator_watch_get\",\"locator_watch_stop\",\"locator_watch_step\",\"locator_watch_remove\",\"locator_watch_cleanup\",\"index_publication_status\",\"index_publish_step\",\"index_publish_queue_step\",\"index_create\",\"index_discover\",\"index_step\",\"index_reindex_step\",\"index_queue_step\",\"index_refresh_step\",\"index_get\",\"index_pending\",\"index_search\",\"index_audio_read\",\"index_audio_history\",\"index_visual_search\",\"index_visual_read\",\"index_visual_history\",\"index_audio_search\",\"inventory\",\"register_text\",\"read_text\",\"register_pdf\",\"read_pdf\",\"register_html\",\"read_html\",\"register_docx\",\"read_docx\",\"register_audio\",\"register_media\",\"inspect_audio\",\"inspect_visual\",\"prepare_audio\",\"prepare_visual\",\"analyze_video\",\"analyze_visual\",\"publish_visual\",\"transcribe_audio\",\"publish_transcript\",\"visual_index_create\",\"visual_index_get\",\"visual_index_step\",\"visual_index_queue_step\",\"visual_index_read\",\"audio_index_create\",\"audio_index_get\",\"audio_index_step\",\"audio_index_queue_step\",\"audio_index_read\",\"register_image\",\"read_image\",\"read_pdf_ocr\"],",out);
        fprintf(out,"\"embedding_inference_adapter_configured\":%s,",cfg&&cfg->embedding?"true":"false");
        fprintf(out,"\"summary_inference_adapter_configured\":%s,\"visual_inference_adapter_configured\":%s,\"inference_adapter_configured\":%s,\"pdf_adapter_configured\":%s,\"ocr_adapter_configured\":%s,\"media_adapter_configured\":%s,\"fetch_adapter_configured\":%s,\"search_adapter_configured\":%s,\"chutni_adapter_configured\":%s,\"chutni_publication_enabled\":%s,\"capabilities\":[\"request_correlation\",\"request_budgets\",\"operations\",\"cancellation\",\"operation_parents\"],\"limits\":{\"media_index_shared_jobs\":8,\"media_index_shared_capture_bytes\":8388608,\"visual_index_windows_per_job\":480,\"visual_index_window_ms\":30000,\"audio_index_jobs\":8,\"audio_index_windows_per_job\":497,\"audio_index_capture_bytes\":8388608,\"audio_index_window_ms\":30000,\"audio_index_overlap_ms\":1000,\"audio_index_attempt_ms\":60000,\"publication_receipts\":4096,\"publication_step_items\":1,\"publication_retry_min_ms\":1000,\"publication_retry_max_ms\":60000,\"memory_artifact_bytes\":67108864,\"memory_read_bytes\":65536,\"memory_read_response_bytes\":262144,\"memory_query_bytes\":512,\"memory_results\":8,\"memory_snippet_bytes\":4096,\"memory_worker_response_bytes\":65536,\"fetch_body_bytes\":5242880,\"fetch_header_bytes\":65536,\"acquisition_receipts\":128,\"search_query_bytes\":512,\"search_results\":8,\"search_description_chars\":400,\"upload_sessions\":8,\"upload_history\":128,\"upload_reserved_bytes\":67108864,\"upload_chunk_bytes\":8192,\"upload_chunk_rows\":8200,\"request_bytes\":16384,\"source_bytes\":67108864,\"read_bytes\":65536,\"pdf_pages\":4,\"visual_axis\":512,\"visual_source_axis\":4096,\"visual_source_pixels\":4194304,\"media_window_ms\":30000,\"media_duration_ms\":14400000,\"ocr_pixels\":4194304,\"ocr_axis\":4096,\"ocr_text_bytes\":65536,\"ocr_model_bytes\":33554432,\"snapshots\":128,\"source_claims\":256,\"live_locators\":256,\"locator_watches\":8,\"watch_cleanup_pending\":8,\"locator_operations\":256,\"locator_batch\":8,\"locator_path_bytes\":1024,\"locator_response_bytes\":65536,\"live_read_response_bytes\":2101248,\"snapshot_bytes\":134217728,\"cache_entries\":128,\"cache_bytes\":8388608,\"cache_response_bytes\":2097152,\"extractor_artifact_bytes\":134217728,\"semantic_dimensions\":1024,\"semantic_results\":8,\"semantic_profile_bytes\":4096,\"index_jobs\":8,\"index_items_per_job\":256,\"index_retry_rows\":2048,\"index_retry_min_ms\":1000,\"index_retry_max_ms\":60000,\"index_runtime_workers\":1,\"index_runtime_interval_min_ms\":100,\"index_runtime_interval_max_ms\":60000,\"index_runtime_attempt_ms\":5000,\"index_schedule_rows\":8,\"index_refresh_schedule_rows\":8,\"index_refresh_scans\":1,\"index_refresh_items\":1,\"index_recovery_items\":1,\"index_recovery_rows\":2048,\"index_freshness_schedule_rows\":8,\"index_freshness_observations\":1,\"index_queue_step_items\":1,\"index_pending_items\":2048,\"index_retained_versions\":256,\"index_operations\":512,\"index_step_items\":4,\"index_text_evidence_bytes\":8388608,\"database_bytes\":167772160}}\n",cfg&&cfg->summary_inference?"true":"false",cfg&&cfg->visual_inference?"true":"false",cfg&&cfg->inference?"true":"false",cfg&&cfg->pdfium_library?"true":"false",cfg&&cfg->ocr?"true":"false",cfg&&cfg->media_helper&&*cfg->media_helper?"true":"false",cfg&&cfg->fetch_helper&&*cfg->fetch_helper&&cfg->fetch_origin&&*cfg->fetch_origin?"true":"false",cfg&&cfg->search&&cfg->fetch_helper&&*cfg->fetch_helper?"true":"false",cfg&&cfg->chutni_library&&cfg->chutni_store&&cfg->chutni_helper?"true":"false",cfg&&cfg->chutni_publish?"true":"false");
        free(op);
        return 0;
    }
    const char *control_error=sources_interrupted(cfg);
    if(control_error){free(op);return failure(out,control_error);}
    if(!owner_valid(source_owner(cfg))){
        free(op);
        return failure(out,"invalid_configuration");
    }
    if(!strcmp(op,"inventory")){
        char *selected=json_string(&request,json_get(&request,0,"path"));
        int rc=inventory_run(cfg,selected,out);
        free(selected);
        free(op);
        return rc;
    }
    sqlite3 *db;SourceBoundDb *binding=NULL;
    int dir;
    if(!cfg || !store_open(cfg,&db,&dir,&binding)) {
        free(op);
        return failure(out,"state_unavailable");
    }
    int rc=!strcmp(op,"analyze_video")?sources_temporal_dispatch(cfg,&request,db,out):!strcmp(op,"publish_summary")?source_summary_publish(cfg,&request,db,out):!strcmp(op,"summarize_evidence_opening")?sources_summary_opening_dispatch(cfg,&request,db,out):!strcmp(op,"summarize_evidence")?sources_summary_dispatch(cfg,&request,db,out):!strncmp(op,"visual_index_",13)?sources_visual_index(cfg,&request,db,dir,out):!strcmp(op,"publish_visual")?source_visual_publish(cfg,&request,db,out):!strcmp(op,"inspect_visual")?sources_visual_inspect(cfg,&request,db,out):!strcmp(op,"analyze_visual")?sources_visual_dispatch(cfg,&request,db,out):!strcmp(op,"inspect_audio")?sources_audio_inspect(cfg,&request,db,out):!strncmp(op,"audio_index_",12)?sources_audio_index(cfg,&request,db,dir,out):!strcmp(op,"publish_transcript")?source_transcript_publish(cfg,&request,db,out):(!strcmp(op,"memory_search")||!strcmp(op,"memory_read")||!strcmp(op,"memory_search_semantic")||!strcmp(op,"memory_search_text"))?source_chutni_dispatch(cfg,input,length,db,out):!strcmp(op,"search")?source_search_dispatch(cfg,input,length,db,out):!strcmp(op,"fetch")?source_fetch_dispatch(cfg,&request,db,out):!strncmp(op,"upload_",7)?source_upload_session(cfg,&request,op,db,out):!strncmp(op,"locator_",8)?source_locator_dispatch(cfg,input,length,db,dir,out):!strncmp(op,"index_",6)?source_index_dispatch(cfg,input,length,db,dir,out):!strcmp(op,"register_text")?registration(cfg,&request,db,out,0):!strcmp(op,"register_pdf")?registration(cfg,&request,db,out,1):!strcmp(op,"register_html")?registration(cfg,&request,db,out,2):!strcmp(op,"register_media")?registration(cfg,&request,db,out,7):!strcmp(op,"register_audio")?registration(cfg,&request,db,out,6):(!strcmp(op,"transcribe_audio")||!strcmp(op,"prepare_audio")||!strcmp(op,"prepare_visual"))?(!strcmp(op,"transcribe_audio")?sources_transcript_dispatch(cfg,&request,db,out):sources_media_prepare(cfg,&request,db,out)):!strcmp(op,"register_image")?registration(cfg,&request,db,out,4):!strcmp(op,"read_image")?cached_read(cfg,&request,db,out,4):!strcmp(op,"register_docx")?registration(cfg,&request,db,out,3):!strcmp(op,"read_docx")?cached_read(cfg,&request,db,out,3):!strcmp(op,"read_html")?cached_read(cfg,&request,db,out,2):!strcmp(op,"read_pdf_ocr")?cached_read(cfg,&request,db,out,5):!strcmp(op,"read_pdf")?cached_read(cfg,&request,db,out,1):cached_read(cfg,&request,db,out,0);
    store_close(db,binding);
    close(dir);
    free(op);
    return rc;
}
