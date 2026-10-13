#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 2048
#include "locator.h"
#include "index.h"
#include "control.h"
#include "watch.h"
#include "json.h"
#include "sha256.h"
#include "inventory.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define LOCATOR_MAX 256
#define OPERATION_MAX 256
#define PATH_MAX_BYTES 1024
#define RESPONSE_MAX 65536
typedef struct {
    char *path,*version,*parent_path,*leaf;
    char id[65],hash[65],identity[65];
    size_t expected_device,expected_inode,checked;
    int pinned,parent,file,missing,node_leaf;
    struct stat parent_info,file_info;
    const char *status,*reason;
    sqlite3_int64 revision,observed_ms;
}
Observation;
typedef struct {
    const SourcesConfig *cfg;
    sqlite3 *db;
    int directory,lease,root;
    const char *owner,*host;
    char scope[65];
    struct stat root_info;
}
Locators;
static int fail(FILE *out,const char *error){
    fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",error);
    return 1;
}
static int hash_valid(const char *s){
    if(!s||strlen(s)!=64)return 0;
    for(size_t i=0;i<64;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;
    return 1;
}
static int identity_valid(const char *s){
    if(!s||!*s||strlen(s)>128)return 0;
    for(const char *p=s;*p;p++)if(!isalnum((unsigned char)*p)&&!strchr("-_:.",*p))return 0;
    return 1;
}
static int path_valid(const char *s){
    if(!s||!*s||*s=='/'||strlen(s)>PATH_MAX_BYTES)return 0;
    const char *part=s;
    for(;;){
        const char *end=strchr(part,'/');
        size_t n=end?(size_t)(end-part):strlen(part);
        if(!n||(n==1&&*part=='.')||(n==2&&!memcmp(part,"..",2)))return 0;
        if(!end)return 1;
        part=end+1;
    }
}
static int allowed(const Json *j,int object,const char *fields){
    if(object<0||j->tokens[object].type!=JT_OBJECT)return 0;
    for(int t=object+1;t<j->tokens[object].next;t=j->tokens[j->tokens[t].next].next){
        char *key=json_string(j,t),name[128];
        int n=snprintf(name,sizeof(name),"|%s|",key?key:"");
        free(key);
        if(n<0||(size_t)n>=sizeof(name)||!strstr(fields,name))return 0;
    }
    return 1;
}
static int same(const struct stat *a,const struct stat *b){
#ifdef __APPLE__
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&a->st_size==b->st_size&&a->st_mtimespec.tv_sec==b->st_mtimespec.tv_sec&&a->st_mtimespec.tv_nsec==b->st_mtimespec.tv_nsec&&a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec&&a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;
#else
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mode==b->st_mode&&a->st_size==b->st_size&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
#endif
}
static int prepare(Locators *l,sqlite3_stmt **stmt,const char *sql){
    return sqlite3_prepare_v2(l->db,sql,-1,stmt,NULL)==SQLITE_OK;
}
static void bind(sqlite3_stmt *stmt,int index,const char *value){
    if(value)sqlite3_bind_text(stmt,index,value,-1,SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt,index);
}
static int sql(Locators *l,const char *value){
    return sqlite3_exec(l->db,value,NULL,NULL,NULL)==SQLITE_OK;
}
static sqlite3_int64 scalar(Locators *l,const char *query){
    sqlite3_stmt *stmt=NULL;
    sqlite3_int64 value=-1;
    if(prepare(l,&stmt,query)&&sqlite3_step(stmt)==SQLITE_ROW)value=sqlite3_column_int64(stmt,0);
    sqlite3_finalize(stmt);
    return value;
}
static const char *schema(Locators *l){
    return sql(l,"CREATE TABLE IF NOT EXISTS watch_cleanup(id TEXT PRIMARY KEY,owner TEXT NOT NULL);CREATE TABLE IF NOT EXISTS watch_health(id TEXT PRIMARY KEY,attempts INTEGER NOT NULL,successes INTEGER NOT NULL,failures INTEGER NOT NULL,error TEXT,attempted_ms INTEGER NOT NULL);CREATE TABLE IF NOT EXISTS watch_checkpoints(id TEXT PRIMARY KEY,cycle INTEGER NOT NULL,result TEXT NOT NULL,digest TEXT NOT NULL);CREATE TABLE IF NOT EXISTS locator_watches(id TEXT PRIMARY KEY,owner TEXT NOT NULL,operation TEXT NOT NULL,scope TEXT NOT NULL,request TEXT NOT NULL,status TEXT NOT NULL,result TEXT NOT NULL,digest TEXT NOT NULL,UNIQUE(owner,operation));CREATE TABLE IF NOT EXISTS live_locators(id TEXT PRIMARY KEY,owner TEXT NOT NULL,scope TEXT NOT NULL,path TEXT NOT NULL,revision INTEGER NOT NULL,result TEXT NOT NULL,digest TEXT NOT NULL);CREATE TABLE IF NOT EXISTS locator_operations(owner TEXT NOT NULL,operation TEXT NOT NULL,request TEXT NOT NULL,result TEXT NOT NULL,digest TEXT NOT NULL,PRIMARY KEY(owner,operation));")?NULL:((sqlite3_errcode(l->db)==SQLITE_BUSY||sqlite3_errcode(l->db)==SQLITE_LOCKED)?"store_busy":"store_failed");
}
static void free_observation(Observation *o){
    free(o->path);
    free(o->version);
    free(o->parent_path);
    free(o->leaf);
    if(o->parent>=0)close(o->parent);
    if(o->file>=0)close(o->file);
}
static const char *normalize(Locators *l,const Json *j,char **operation,Observation rows[8],size_t *count,char **encoded){
    *operation=json_string(j,json_get(j,0,"operation_id"));
    int entries=json_get(j,0,"locators");
    if(!identity_valid(*operation)||entries<0||j->tokens[entries].type!=JT_ARRAY||!j->tokens[entries].count||j->tokens[entries].count>8)return "invalid_request";
    size_t length;
    FILE *stream=open_memstream(encoded,&length);
    if(!stream)return "resource_limit";
    fputs("{\"op\":\"locator_reconcile\",\"operation_id\":",stream);
    json_quote(stream,*operation,strlen(*operation));
    fputs(",\"host_id\":",stream);
    json_quote(stream,l->host,strlen(l->host));
    fputs(",\"locators\":[",stream);
    const char *error=NULL;
    for(int t=entries+1;!error&&t<j->tokens[entries].next;t=j->tokens[t].next){
        Observation *o=&rows[(*count)++];
        if(!allowed(j,t,"|path|expected_version|expected_device|expected_inode|")){
            error="unknown_field";
            break;
        }
        o->path=json_string(j,json_get(j,t,"path"));
        o->version=json_string(j,json_get(j,t,"expected_version"));
        if(!path_valid(o->path)||!hash_valid(o->version)){
            error="invalid_request";
            break;
        }
        int device=json_get(j,t,"expected_device"),inode=json_get(j,t,"expected_inode");
        o->pinned=device>=0||inode>=0;
        if(o->pinned&&(!json_uint(j,device,&o->expected_device)||!json_uint(j,inode,&o->expected_inode)||o->expected_device>INT64_MAX||o->expected_inode>INT64_MAX)){
            error="invalid_request";
            break;
        }
        for(size_t prior=0;prior+1<*count;prior++)if(!strcmp(rows[prior].path,o->path))error="duplicate_locator";
        if(error)break;
        if(*count>1)fputc(',',stream);
        fputs("{\"path\":",stream);
        json_quote(stream,o->path,strlen(o->path));
        fprintf(stream,",\"expected_version\":\"%s\"",o->version);
        if(o->pinned)fprintf(stream,",\"expected_device\":%zu,\"expected_inode\":%zu",o->expected_device,o->expected_inode);
        fputc('}',stream);
    }
    fputs("]}",stream);
    if(fclose(stream))error="resource_limit";
    return error;
}
static const char *validated_result(const char *value,const char *digest,char **copy){
    if(!value||!digest||strlen(value)>RESPONSE_MAX)return "corrupt_locator";
    char actual[65];
    hash_bytes(value,strlen(value),actual);
    Json j;
    if(strcmp(actual,digest)||!json_parse(&j,value,strlen(value))||j.tokens[0].type!=JT_OBJECT)return "corrupt_locator";
    *copy=strdup(value);
    return *copy?NULL:"resource_limit";
}
static const char *retry(Locators *l,const char *operation,const char *request,char **result){
    sqlite3_stmt *stmt=NULL;
    const char *error=NULL;
    if(!prepare(l,&stmt,"SELECT request,result,digest FROM locator_operations WHERE owner=? AND operation=?"))return "store_failed";
    bind(stmt,1,l->owner);
    bind(stmt,2,operation);
    int rc=sqlite3_step(stmt);
    if(rc==SQLITE_ROW){
        const char *saved=(const char*)sqlite3_column_text(stmt,0);
        if(!saved||strcmp(saved,request))error="idempotency_conflict";
        else error=validated_result((const char*)sqlite3_column_text(stmt,1),(const char*)sqlite3_column_text(stmt,2),result);
    }
    else if(rc!=SQLITE_DONE)error="store_failed";
    sqlite3_finalize(stmt);
    return error;
}
static const char *lease(Locators *l){
    l->lease=openat(l->directory,"locators.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    struct stat info;
    if(l->lease<0||fstat(l->lease,&info)||!S_ISREG(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&077)||info.st_nlink!=1)return "state_unavailable";
    return flock(l->lease,LOCK_EX|LOCK_NB)?errno==EWOULDBLOCK?"busy":"store_failed":NULL;
}
static const char *open_root(Locators *l){
    if(!l->cfg->root)return "scope_unavailable";
    l->root=open(l->cfg->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(l->root<0||fstat(l->root,&l->root_info))return "scope_unavailable";
    char material[512];
    int n=snprintf(material,sizeof(material),"directory-scope-v1:%s:%llu:%llu:",l->host,(unsigned long long)l->root_info.st_dev,(unsigned long long)l->root_info.st_ino);
    if(n<0||(size_t)n>=sizeof(material))return "resource_limit";
    hash_bytes(material,(size_t)n,l->scope);
    return NULL;
}
static const char *admit(Locators *l,Observation rows[8],size_t count,int history){
    sqlite3_int64 operations=scalar(l,"SELECT count(*) FROM locator_operations"),locations=scalar(l,"SELECT count(*) FROM live_locators");
    if(operations<0||locations<0)return "store_failed";
    if(history&&operations>=OPERATION_MAX)return "locator_operation_limit";
    size_t added=0;
    for(size_t i=0;i<count;i++){
        Observation *o=&rows[i];
        Sha256 hash;
        unsigned char sum[32];
        sha256_init(&hash);
        sha256_update(&hash,"source-locator-v1",sizeof("source-locator-v1"));
        sha256_update(&hash,l->owner,strlen(l->owner)+1);
        sha256_update(&hash,l->scope,65);
        sha256_update(&hash,o->path,strlen(o->path));
        sha256_final(&hash,sum);
        sha256_hex(sum,o->id);
        sqlite3_stmt *stmt=NULL;
        if(!prepare(l,&stmt,"SELECT revision FROM live_locators WHERE id=? AND owner=?"))return "store_failed";
        bind(stmt,1,o->id);
        bind(stmt,2,l->owner);
        int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            sqlite3_int64 saved=sqlite3_column_int64(stmt,0);
            if(sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||saved<1){sqlite3_finalize(stmt);return "corrupt_locator";}
            if(saved==INT64_MAX){sqlite3_finalize(stmt);return "locator_revision_limit";}
            o->revision=saved+1;
        }
        else if(rc==SQLITE_DONE){
            o->revision=1;
            added++;
        }
        sqlite3_finalize(stmt);
        if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)return "store_failed";
        if(!prepare(l,&stmt,"SELECT 1 FROM source_claims WHERE owner=? AND id=? LIMIT 1"))return "store_failed";
        bind(stmt,1,l->owner);
        bind(stmt,2,o->version);
        rc=sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if(rc!=SQLITE_ROW)return rc==SQLITE_DONE?"unknown_source":"store_failed";
    }
    return (size_t)locations+added>LOCATOR_MAX?"locator_limit":NULL;
}
/* Keep only the final observed parent and file descriptors. Rewalking the root
 * proves that a held directory remains at the admitted relative name. */ static const char *observe(Locators *l,Observation *o){
    char *copy=strdup(o->path);
    if(!copy)return "resource_limit";
    int directory=dup(l->root);
    if(directory<0){
        free(copy);
        return "resource_limit";
    }
    char prefix[PATH_MAX_BYTES+1]={
        0
    }
    ;
    char *part=copy;
    const char *error=NULL;
    for(;;){
        char *slash=strchr(part,'/');
        if(slash)*slash=0;
        struct stat before,node,after;
        if(fstat(directory,&before)){
            error="source_changed";
            break;
        }
        if(fstatat(directory,part,&node,AT_SYMLINK_NOFOLLOW)){
            if(errno!=ENOENT){
                error="observation_unavailable";
                break;
            }
            o->missing=1;
            o->status="missing";
            o->reason="path_absent";
        }
        else if(slash&&S_ISDIR(node.st_mode)){
            int next=openat(directory,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(next<0&&(errno==EACCES||errno==EPERM)){
                o->status="uncertain";
                o->reason="parent_unavailable";
                o->file_info=node;
                goto captured_parent;
            }
            if(next<0||fstat(directory,&after)||!same(&before,&after)){
                if(next>=0)close(next);
                error="source_changed";
                break;
            }
            size_t used=strlen(prefix),n=strlen(part);
            if(used){
                prefix[used++]='/';
                prefix[used]=0;
            }
            memcpy(prefix+used,part,n+1);
            close(directory);
            directory=next;
            part=slash+1;
            continue;
        }
        else if(slash||!S_ISREG(node.st_mode)||node.st_size<0||node.st_size>SOURCES_INPUT_MAX){
            o->status="unsupported";
            o->reason=slash?"unsupported_parent":!S_ISREG(node.st_mode)?"unsupported_file":"source_too_large";
            o->file_info=node;
        }
        else {
            o->file=openat(directory,part,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            if(o->file<0&&(errno==EACCES||errno==EPERM)){
                o->status="uncertain";
                o->reason="file_unavailable";
                o->file_info=node;
                goto captured_parent;
            }
            if(o->file<0||fstat(o->file,&o->file_info)||!same(&node,&o->file_info)){
                error="source_changed";
                break;
            }
            inventory_file_identity(&o->file_info,o->identity);
            if(o->pinned&&((size_t)o->file_info.st_dev!=o->expected_device||(size_t)o->file_info.st_ino!=o->expected_inode)){
                o->status="changed";
                o->reason="file_identity_changed";
            }
            else {
                Sha256 hash;
                unsigned char sum[32],buffer[65536];
                sha256_init(&hash);
                for(;;){
                    error=sources_interrupted(l->cfg);
                    if(error)break;
                    ssize_t n=read(o->file,buffer,sizeof(buffer));
                    if(n<0){
                        if(errno==EINTR)continue;
                        error="read_failed";
                        break;
                    }
                    if(!n)break;
                    if((size_t)n>SOURCES_INPUT_MAX-o->checked){
                        error="source_changed";
                        break;
                    }
                    o->checked+=(size_t)n;
                    sha256_update(&hash,buffer,(size_t)n);
                }
                if(error)break;
                if(fstat(o->file,&after)||!same(&o->file_info,&after)||o->checked!=(size_t)o->file_info.st_size){
                    error="source_changed";
                    break;
                }
                sha256_final(&hash,sum);
                sha256_hex(sum,o->hash);
                o->status=!strcmp(o->hash,o->version)?"available":"changed";
                o->reason=!strcmp(o->hash,o->version)?"verified_content":"content_changed";
            }
        }
        captured_parent: o->node_leaf=!slash;
        if(fstat(directory,&after)||!same(&before,&after)){
            error="source_changed";
            break;
        }
        o->parent=directory;
        directory=-1;
        o->parent_info=before;
        o->parent_path=strdup(prefix);
        o->leaf=strdup(part);
        if(!o->parent_path||!o->leaf)error="resource_limit";
        break;
    }
    if(directory>=0)close(directory);
    free(copy);
    return error;
}
static int reopen_directory(Locators *l,const char *path){
    int fd=dup(l->root);
    if(fd<0)return -1;
    if(!*path)return fd;
    char *copy=strdup(path);
    if(!copy){
        close(fd);
        return -1;
    }
    char *part=copy;
    for(;;){
        char *slash=strchr(part,'/');
        if(slash)*slash=0;
        int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        close(fd);
        fd=next;
        if(fd<0||!slash)break;
        part=slash+1;
    }
    free(copy);
    return fd;
}
static const char *verify(Locators *l,Observation *o){
    struct stat info;
    if(fstat(o->parent,&info)||!same(&info,&o->parent_info))return "source_changed";
    int parent=reopen_directory(l,o->parent_path);
    if(parent<0)return "source_changed";
    int valid=!fstat(parent,&info)&&same(&info,&o->parent_info);
    close(parent);
    if(!valid)return "source_changed";
    if(o->missing){
        if(!fstatat(o->parent,o->leaf,&info,AT_SYMLINK_NOFOLLOW)||errno!=ENOENT)return "source_changed";
    }
    else {
        if(fstatat(o->parent,o->leaf,&info,AT_SYMLINK_NOFOLLOW)||!same(&info,&o->file_info))return "source_changed";
        if(o->file>=0&&(fstat(o->file,&info)||!same(&info,&o->file_info)))return "source_changed";
    }
    struct timespec now;
    if(clock_gettime(CLOCK_REALTIME,&now)||now.tv_sec<0||(uint64_t)now.tv_sec>(uint64_t)INT64_MAX/1000)return "clock_unavailable";
    o->observed_ms=(sqlite3_int64)now.tv_sec*1000+now.tv_nsec/1000000;
    return NULL;
}
static void row_reply(Locators *l,const Observation *o,FILE *out){
    fprintf(out,"{\"locator_id\":\"%s\",\"revision\":%lld,\"host_id\":",o->id,(long long)o->revision);
    json_quote(out,l->host,strlen(l->host));
    fprintf(out,",\"scope_identity\":\"%s\",\"path\":",l->scope);
    json_quote(out,o->path,strlen(o->path));
    fprintf(out,",\"identity_constraint\":\"%s\"",o->pinned?"content_and_inode":"content_only");
    if(o->pinned)fprintf(out,",\"expected_device\":%zu,\"expected_inode\":%zu",o->expected_device,o->expected_inode);
    fprintf(out,",\"expected_version\":\"%s\",\"status\":\"%s\",\"reason\":\"%s\",\"content_version\":",o->version,o->status,o->reason);
    if(!strcmp(o->status,"available"))json_quote(out,o->version,64);
    else fputs("null",out);
    fputs(",\"observed_content_hash\":",out);
    if(*o->hash)json_quote(out,o->hash,64);
    else fputs("null",out);
    fputs(",\"file_identity\":",out);
    if(*o->identity)json_quote(out,o->identity,64);
    else fputs("null",out);
    if(!o->missing&&o->node_leaf)fprintf(out,",\"device\":%llu,\"inode\":%llu",(unsigned long long)o->file_info.st_dev,(unsigned long long)o->file_info.st_ino);
    else fputs(",\"device\":null,\"inode\":null",out);
    fprintf(out,",\"checked_bytes\":%zu,\"observed_at_ms\":%lld}",o->checked,(long long)o->observed_ms);
}
static const char *commit(Locators *l,const char *operation,const char *request,Observation rows[8],size_t count,char **result,const char *watch,size_t cycle){
    size_t length;
    FILE *out=open_memstream(result,&length);
    if(!out)return "resource_limit";
    fprintf(out,"{\"contract_version\":1,\"operation_id\":");
    if(operation)json_quote(out,operation,strlen(operation));else fputs("null",out);
    if(watch){fputs(",\"watch_id\":",out);json_quote(out,watch,64);fprintf(out,",\"cycle\":%zu",cycle);}
    fprintf(out,",\"freshness\":\"observed_checkpoint\",\"result_semantics\":\"committed_observation_history\",\"observation_consistency\":\"per_locator\",\"exhaustive_current_filesystem\":false,\"scope_identity\":\"%s\",\"observations\":[",l->scope);
    size_t checked=0;
    for(size_t i=0;i<count;i++){
        if(i)fputc(',',out);
        row_reply(l,&rows[i],out);
        checked+=rows[i].checked;
    }
    fprintf(out,"],\"checked_bytes\":%zu}\n",checked);
    if(fclose(out))return "resource_limit";
    if(length>RESPONSE_MAX)return "response_too_large";
    if(!sql(l,"BEGIN IMMEDIATE"))return "store_busy";
    const char *error=NULL;
    sqlite3_stmt *stmt=NULL;
    for(size_t i=0;!error&&i<count;i++){
        char *row=NULL;
        size_t n;
        out=open_memstream(&row,&n);
        if(!out){
            error="resource_limit";
            break;
        }
        row_reply(l,&rows[i],out);
        if(fclose(out)){
            free(row);
            error="resource_limit";
            break;
        }
        char digest[65];
        hash_bytes(row,n,digest);
        if(!prepare(l,&stmt,"INSERT INTO live_locators(id,owner,scope,path,revision,result,digest) VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET revision=excluded.revision,result=excluded.result,digest=excluded.digest"))error="store_failed";
        else {
            bind(stmt,1,rows[i].id);
            bind(stmt,2,l->owner);
            bind(stmt,3,l->scope);
            bind(stmt,4,rows[i].path);
            sqlite3_bind_int64(stmt,5,rows[i].revision);
            bind(stmt,6,row);
            bind(stmt,7,digest);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
        stmt=NULL;
        free(row);
        if(!error)error=source_index_observed(l->db,l->owner,l->scope,rows[i].path,rows[i].revision,rows[i].identity,rows[i].hash);
    }
    if(!error&&operation){
        char digest[65];
        hash_bytes(*result,length,digest);
        if(!prepare(l,&stmt,"INSERT INTO locator_operations(owner,operation,request,result,digest) VALUES(?,?,?,?,?)"))error="store_failed";
        else {
            bind(stmt,1,l->owner);
            bind(stmt,2,operation);
            bind(stmt,3,request);
            bind(stmt,4,*result);
            bind(stmt,5,digest);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
    }
    if(!error&&watch){
        char digest[65];hash_bytes(*result,length,digest);
        if(!prepare(l,&stmt,"INSERT INTO watch_checkpoints VALUES(?,?,?,?) ON CONFLICT(id) DO UPDATE SET cycle=excluded.cycle,result=excluded.result,digest=excluded.digest"))error="store_failed";
        else {bind(stmt,1,watch);sqlite3_bind_int64(stmt,2,(sqlite3_int64)cycle);bind(stmt,3,*result);bind(stmt,4,digest);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);
    }
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *marker=getenv("SAMOSA_LOCATOR_TEST_PRECOMMIT_MARKER"),*release=getenv("SAMOSA_LOCATOR_TEST_PRECOMMIT_RELEASE");
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
    if(!error)error=sources_interrupted(l->cfg);
    for(size_t i=0;!error&&i<count;i++)error=verify(l,&rows[i]);
    if(!error){
        struct stat info;int root=open(l->cfg->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(root<0||fstat(root,&info)||!same(&info,&l->root_info))error="source_changed";
        if(root>=0)close(root);
    }
    if(!error)error=sources_interrupted(l->cfg);
    if(error)sql(l,"ROLLBACK");
    else if(!sql(l,"COMMIT")){
        sql(l,"ROLLBACK");
        error="store_failed";
    }
    return error;
}
static int reconcile(Locators *l,const Json *j,FILE *out){
    Observation rows[8]={
        0
    }
    ;
    for(size_t i=0;i<8;i++)rows[i].parent=rows[i].file=-1;
    char *operation=NULL,*request=NULL,*result=NULL;
    size_t count=0;
    const char *error=normalize(l,j,&operation,rows,&count,&request);
    if(!error)error=retry(l,operation,request,&result);
    if(!error&&result)goto done;
    if(!error)error=sources_interrupted(l->cfg);
    if(!error)error=lease(l);
    if(!error)error=retry(l,operation,request,&result);
    if(!error&&result)goto done;
    if(!error&&scalar(l,"SELECT count(*) FROM locator_operations")>=OPERATION_MAX)error="locator_operation_limit";
    if(!error)error=open_root(l);
    if(!error)error=admit(l,rows,count,1);
    for(size_t i=0;!error&&i<count;i++){
        error=sources_interrupted(l->cfg);
        if(!error)error=observe(l,&rows[i]);
    }
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *marker=getenv("SAMOSA_LOCATOR_TEST_MARKER"),*release=getenv("SAMOSA_LOCATOR_TEST_RELEASE");
    if(!error&&marker&&release){
        FILE *ready=fopen(marker,"w");
        if(!ready)_exit(98);
        fputs("observed",ready);
        fclose(ready);
        int released=0;
        for(int i=0;i<500;i++){
            if(!access(release,F_OK)){
                released=1;
                break;
            }
            struct timespec delay={
                0,10000000
            }
            ;
            nanosleep(&delay,NULL);
        }
        if(!released)_exit(99);
    }
#endif
    for(size_t i=0;!error&&i<count;i++)error=verify(l,&rows[i]);
    if(!error){
        struct stat info;
        int root=open(l->cfg->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(root<0||fstat(root,&info)||!same(&info,&l->root_info))error="source_changed";
        if(root>=0)close(root);
    }
    if(!error)error=commit(l,operation,request,rows,count,&result,NULL,0);
    done: for(size_t i=0;i<count;i++)free_observation(&rows[i]);
    free(operation);
    free(request);
    int rc=error?fail(out,error):0;
    if(!error)fputs(result,out);
    free(result);
    return rc;
}
const char *source_locator_observe_index(const SourcesConfig *cfg,sqlite3 *db,int directory,const char *scope,const char *path,const char *version){
    if(!cfg||!hash_valid(scope)||!path_valid(path)||!hash_valid(version))return "invalid_request";
    Locators l={.cfg=cfg,.db=db,.directory=directory,.lease=-1,.root=-1,.owner=cfg->owner?cfg->owner:"local-owner",.host=cfg->host_id?cfg->host_id:"local-endpoint"};
    Observation rows[8]={{0}};rows[0].parent=rows[0].file=-1;
    rows[0].path=strdup(path);rows[0].version=strdup(version);
    const char *error=rows[0].path&&rows[0].version?schema(&l):"resource_limit";
    if(!error)error=sources_interrupted(cfg);
    if(!error)error=lease(&l);
    if(!error)error=open_root(&l);
    if(!error&&strcmp(l.scope,scope))error="scope_unavailable";
    if(!error)error=admit(&l,rows,1,0);
    if(!error)error=observe(&l,&rows[0]);
    if(!error)error=verify(&l,&rows[0]);
    if(!error){
        struct stat info;int root=open(cfg->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(root<0||fstat(root,&info)||!same(&info,&l.root_info))error="source_changed";
        if(root>=0)close(root);
    }
    char *result=NULL;
    if(!error)error=commit(&l,NULL,NULL,rows,1,&result,NULL,0);
    free(result);free_observation(&rows[0]);
    if(l.root>=0)close(l.root);
    if(l.lease>=0)close(l.lease);
    return error;
}
static int live_read(Locators *l,const Json *j,FILE *out){
    Observation row={.parent=-1,.file=-1};
    row.path=json_string(j,json_get(j,0,"path"));
    int read=json_get(j,0,"read");
    char *op=json_string(j,json_get(j,read,"op"));
    char *source=json_string(j,json_get(j,read,"source_id"));
    row.version=json_string(j,json_get(j,read,"version"));
    int device=json_get(j,0,"expected_device"),inode=json_get(j,0,"expected_inode");
    row.pinned=device>=0||inode>=0;
    const char *error=NULL;
    size_t contract=0;
    if(read<0||j->tokens[read].type!=JT_OBJECT||!path_valid(row.path)||
       !hash_valid(row.version)||!source||strcmp(source,row.version)||!op||
       !json_uint(j,json_get(j,read,"contract_version"),&contract)||contract!=1||
       (strcmp(op,"read_text")&&strcmp(op,"read_html")&&strcmp(op,"read_docx")&&
        strcmp(op,"read_image")&&strcmp(op,"read_pdf")&&strcmp(op,"read_pdf_ocr")&&
        strcmp(op,"prepare_audio")&&strcmp(op,"prepare_visual")))error="invalid_request";
    if(!error&&row.pinned&&(!json_uint(j,device,&row.expected_device)||
       !json_uint(j,inode,&row.expected_inode)||row.expected_device>INT64_MAX||
       row.expected_inode>INT64_MAX))error="invalid_request";
    if(!error&&!allowed(j,read,"|contract_version|op|source_id|version|start_byte|end_byte|start_page|end_page|start_ms|end_ms|"))error="unknown_field";
    sqlite3_stmt *stmt=NULL;
    if(!error){
        if(!prepare(l,&stmt,"SELECT 1 FROM source_claims WHERE owner=? AND id=? LIMIT 1"))error="store_failed";
        else {
            bind(stmt,1,l->owner);bind(stmt,2,row.version);
            int rc=sqlite3_step(stmt);
            if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";
        }
        sqlite3_finalize(stmt);
    }
    if(!error)error=open_root(l);
    if(!error)error=observe(l,&row);
    if(!error&&strcmp(row.status,"available"))error="live_source_unavailable";
    if(!error)error=verify(l,&row);
    char *evidence=NULL;size_t length=0;int rc=1;
    if(!error){
        FILE *stream=open_memstream(&evidence,&length);
        if(!stream)error="resource_limit";
        else {
            rc=sources_dispatch(l->cfg,j->text+j->tokens[read].begin,
                j->tokens[read].end-j->tokens[read].begin,stream);
            if(fclose(stream))error="resource_limit";
        }
    }
    if(!error)error=sources_interrupted(l->cfg);
    if(!error&&length>2u*1048576u)error="response_too_large";
    if(!error)error=verify(l,&row);
    if(!error){
        int root=open(l->cfg->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        struct stat info;
        if(root<0||fstat(root,&info)||!same(&info,&l->root_info))error="source_changed";
        if(root>=0)close(root);
    }
    if(error)rc=fail(out,error);
    else if(rc)fwrite(evidence,1,length,out);
    else {
        fputs("{\"contract_version\":1,\"freshness\":\"verified_live_read\",\"path\":",out);
        json_quote(out,row.path,strlen(row.path));
        fprintf(out,",\"file_identity\":\"%s\",\"observed_at_ms\":%lld,\"evidence\":",row.identity,(long long)row.observed_ms);
        fwrite(evidence,1,length,out);fputs("}\n",out);
    }
    free(evidence);free(source);free(op);free_observation(&row);return rc;
}
static int get(Locators *l,const Json *j,FILE *out){
    char *id=json_string(j,json_get(j,0,"locator_id")),*result=NULL;
    const char *error=hash_valid(id)?NULL:"invalid_request";
    sqlite3_stmt *stmt=NULL;
    if(!error){
        if(!prepare(l,&stmt,"SELECT result,digest FROM live_locators WHERE id=? AND owner=?"))error="store_failed";
        else {
            bind(stmt,1,id);
            bind(stmt,2,l->owner);
            int rc=sqlite3_step(stmt);
            if(rc==SQLITE_ROW)error=validated_result((const char*)sqlite3_column_text(stmt,0),(const char*)sqlite3_column_text(stmt,1),&result);
            else error=rc==SQLITE_DONE?"unknown_locator":"store_failed";
        }
    }
    sqlite3_finalize(stmt);
    free(id);
    if(error){
        free(result);
        return fail(out,error);
    }
    fprintf(out,"{\"contract_version\":1,\"freshness\":\"committed_history\",\"observation\":%s}\n",result);
    free(result);
    return 0;
}
static const char *watch_checkpoint(const char *id,sqlite3_int64 cycle,const char *value,const char *digest,char **copy){
    const char *error=validated_result(value,digest,copy);if(error)return error;
    Json j;size_t recorded=0;char *watch=NULL;
    if(json_parse(&j,*copy,strlen(*copy)))watch=json_string(&j,json_get(&j,0,"watch_id"));
    if(cycle<=0||!watch||strcmp(watch,id)||!json_uint(&j,json_get(&j,0,"cycle"),&recorded)||recorded!=(size_t)cycle)error="corrupt_locator";
    free(watch);if(error){free(*copy);*copy=NULL;}return error;
}
static int watch_reply(Locators *l,const char *id,FILE *out){
    sqlite3_stmt *stmt=NULL;char *result=NULL;const char *error=NULL;
    if(!prepare(l,&stmt,"SELECT w.result,w.digest,w.status,c.result,c.digest,c.cycle,h.attempts,h.successes,h.failures,h.error,h.attempted_ms FROM locator_watches w LEFT JOIN watch_checkpoints c ON c.id=w.id LEFT JOIN watch_health h ON h.id=w.id WHERE w.id=? AND w.owner=?"))error="store_failed";
    else {
        bind(stmt,1,id);bind(stmt,2,l->owner);int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            error=validated_result((const char*)sqlite3_column_text(stmt,0),(const char*)sqlite3_column_text(stmt,1),&result);
            const char *status=(const char*)sqlite3_column_text(stmt,2);
            if(!error&&(!status||(strcmp(status,"registered")&&strcmp(status,"stopped"))))error="corrupt_locator";
            if(!error&&sqlite3_column_type(stmt,6)!=SQLITE_NULL){
                const int columns[]={6,7,8,10};
                for(size_t i=0;i<4;i++)if(sqlite3_column_type(stmt,columns[i])!=SQLITE_INTEGER||sqlite3_column_int64(stmt,columns[i])<0)error="corrupt_locator";
                sqlite3_int64 attempts=sqlite3_column_int64(stmt,6),successes=sqlite3_column_int64(stmt,7),failures=sqlite3_column_int64(stmt,8);
                const char *health_error=(const char*)sqlite3_column_text(stmt,9);
                if(attempts<1||sqlite3_column_int64(stmt,10)<1||failures>attempts||successes>attempts-failures||(health_error&&!identity_valid(health_error)))error="corrupt_locator";
            }
            if(!error){
                char *checkpoint=NULL;
                if(sqlite3_column_type(stmt,3)!=SQLITE_NULL)error=watch_checkpoint(id,sqlite3_column_int64(stmt,5),(const char*)sqlite3_column_text(stmt,3),(const char*)sqlite3_column_text(stmt,4),&checkpoint);
                if(!error){
                    fputs("{\"contract_version\":1,\"watch_id\":",out);json_quote(out,id,strlen(id));
                    fputs(",\"state\":",out);json_quote(out,status,strlen(status));
                    fputs(",\"monitoring\":",out);const char *monitor=sources_watch_monitor(l->directory,id);json_quote(out,monitor,strlen(monitor));fputs(",\"registration\":",out);fputs(result,out);fputs(",\"checkpoint\":",out);fputs(checkpoint?checkpoint:"null",out);
                    fputs(",\"health\":{\"attempts\":",out);fprintf(out,"%lld,\"successes\":%lld,\"failures\":%lld,\"last_attempt_ms\":%lld,\"last_error\":",(long long)sqlite3_column_int64(stmt,6),(long long)sqlite3_column_int64(stmt,7),(long long)sqlite3_column_int64(stmt,8),(long long)sqlite3_column_int64(stmt,10));
                    const char *health_error=(const char*)sqlite3_column_text(stmt,9);if(health_error)json_quote(out,health_error,strlen(health_error));else fputs("null",out);fputs("}}\n",out);
                }
                free(checkpoint);
            }
        }
        else error=rc==SQLITE_DONE?"unknown_watch":"store_failed";
    }
    sqlite3_finalize(stmt);free(result);return error?fail(out,error):0;
}
static int watch_create(Locators *l,const Json *j,FILE *out){
    Observation rows[8]={0};for(size_t i=0;i<8;i++)rows[i].parent=rows[i].file=-1;
    char *operation=NULL,*request=NULL;size_t count=0;
    const char *error=normalize(l,j,&operation,rows,&count,&request);char id[65]={0};
    sqlite3_stmt *stmt=NULL;
    if(!error&&!prepare(l,&stmt,"SELECT id,request FROM locator_watches WHERE owner=? AND operation=?"))error="store_failed";
    if(!error){
        bind(stmt,1,l->owner);bind(stmt,2,operation);int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            const char *old=(const char*)sqlite3_column_text(stmt,1),*saved_id=(const char*)sqlite3_column_text(stmt,0);
            if(!old||strcmp(old,request))error="idempotency_conflict";
            else if(!hash_valid(saved_id))error="corrupt_locator";
            else memcpy(id,saved_id,65);
        }
        else if(rc!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&*id)goto done;
    if(!error)error=lease(l);
    if(!error&&!prepare(l,&stmt,"SELECT id,request FROM locator_watches WHERE owner=? AND operation=?"))error="store_failed";
    if(!error){
        bind(stmt,1,l->owner);bind(stmt,2,operation);int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            const char *old=(const char*)sqlite3_column_text(stmt,1),*saved_id=(const char*)sqlite3_column_text(stmt,0);
            if(!old||strcmp(old,request))error="idempotency_conflict";
            else if(!hash_valid(saved_id))error="corrupt_locator";
            else memcpy(id,saved_id,65);
        }
        else if(rc!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&*id)goto done;
    if(!error)error=open_root(l);
    if(!error){sqlite3_int64 capacity=scalar(l,"SELECT count(*) FROM locator_watches");if(capacity<0)error="store_failed";else if(capacity>=8)error="watch_limit";}
    for(size_t i=0;!error&&i<count;i++){
        if(!prepare(l,&stmt,"SELECT 1 FROM source_claims WHERE owner=? AND id=? LIMIT 1"))error="store_failed";
        else {bind(stmt,1,l->owner);bind(stmt,2,rows[i].version);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    char *result=NULL;size_t length=0;
    unsigned char nonce[32];
    if(!error){
        int entropy=open("/dev/urandom",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);size_t used=0;
        if(entropy<0)error="resource_limit";
        while(!error&&used<sizeof(nonce)){
            ssize_t n=read(entropy,nonce+used,sizeof(nonce)-used);
            if(n<0&&errno==EINTR)continue;
            if(n<=0)error="resource_limit";else used+=(size_t)n;
        }
        if(entropy>=0)close(entropy);
    }
    if(!error){
        Sha256 hash;unsigned char sum[32];sha256_init(&hash);sha256_update(&hash,l->owner,strlen(l->owner)+1);sha256_update(&hash,request,strlen(request));sha256_update(&hash,l->scope,65);sha256_update(&hash,nonce,sizeof(nonce));sha256_final(&hash,sum);sha256_hex(sum,id);
        FILE *stream=open_memstream(&result,&length);
        if(!stream)error="resource_limit";
        else {
            fputs("{\"contract_version\":1,\"scope_identity\":",stream);json_quote(stream,l->scope,64);
            fputs(",\"request\":",stream);fputs(request,stream);fputc('}',stream);
            if(fclose(stream)||length>RESPONSE_MAX)error="resource_limit";
        }
    }
    int transaction=0;
    if(!error){if(!sql(l,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    if(!error){
        if(!prepare(l,&stmt,"INSERT INTO locator_watches VALUES(?,?,?,?,?,'registered',?,?)"))error="store_failed";
        else {
            char digest[65];hash_bytes(result,length,digest);
            bind(stmt,1,id);bind(stmt,2,l->owner);bind(stmt,3,operation);bind(stmt,4,l->scope);bind(stmt,5,request);bind(stmt,6,result);bind(stmt,7,digest);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
    }
    if(!error)error=sources_interrupted(l->cfg);
    if(transaction){if(error)sql(l,"ROLLBACK");else if(!sql(l,"COMMIT")){sql(l,"ROLLBACK");error="store_failed";}}
    free(result);
    done: for(size_t i=0;i<count;i++)free_observation(&rows[i]);free(operation);free(request);
    return error?fail(out,error):watch_reply(l,id,out);
}
static int watch_control(Locators *l,const Json *j,int stop,FILE *out){
    char *id=json_string(j,json_get(j,0,"watch_id"));const char *error=hash_valid(id)?NULL:"invalid_request";
    sqlite3_stmt *stmt=NULL;
    if(!error&&stop){
        error=lease(l);
        if(!error&&!prepare(l,&stmt,"UPDATE locator_watches SET status='stopped' WHERE id=? AND owner=?"))error="store_failed";
        if(!error){bind(stmt,1,id);bind(stmt,2,l->owner);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);
    }
    int rc=error?fail(out,error):watch_reply(l,id,out);free(id);return rc;
}
static const char *cleanup_one(Locators *l,const char *id,int *cleaned){
    sqlite3_stmt *registered=NULL;
    if(!prepare(l,&registered,"SELECT count(*) FROM locator_watches WHERE id=?"))return "store_failed";
    bind(registered,1,id);int rc=sqlite3_step(registered);
    int exists=rc==SQLITE_ROW&&sqlite3_column_int(registered,0)>0;sqlite3_finalize(registered);
    if(rc!=SQLITE_ROW)return "store_failed";
    if(exists)return "watch_not_removed";
    int runtime=-1;*cleaned=0;const char *error=sources_watch_lock(l->directory,id,&runtime);
    if(!error){
        *cleaned=sources_watch_unlink(l->directory,id,runtime);
        if(*cleaned){
            sqlite3_stmt *stmt=NULL;
            if(!prepare(l,&stmt,"DELETE FROM watch_cleanup WHERE id=? AND owner=?"))error="store_failed";
            else {bind(stmt,1,id);bind(stmt,2,l->owner);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
            sqlite3_finalize(stmt);
        }
    }
    if(runtime>=0)close(runtime);
    return error;
}
static int watch_cleanup(Locators *l,FILE *out){
    const char *error=lease(l);sqlite3_stmt *stmt=NULL;char *ids[8]={0};size_t count=0,cleaned=0,retained=0;
    if(!error&&!prepare(l,&stmt,"SELECT id FROM watch_cleanup WHERE owner=? LIMIT 8"))error="store_failed";
    if(!error){bind(stmt,1,l->owner);int rc;while((rc=sqlite3_step(stmt))==SQLITE_ROW){const char *id=(const char*)sqlite3_column_text(stmt,0);if(!hash_valid(id)){error="corrupt_locator";break;}ids[count]=strdup(id);if(!ids[count++]){error="resource_limit";break;}}if(rc!=SQLITE_DONE&&!error)error="store_failed";}
    sqlite3_finalize(stmt);
    for(size_t i=0;!error&&i<count;i++){
        error=sources_interrupted(l->cfg);if(error)break;
        int removed=0;const char *failure=cleanup_one(l,ids[i],&removed);
        if(failure&&!strcmp(failure,"watch_busy")){retained++;continue;}
        if(failure)error=failure;else if(removed)cleaned++;else retained++;
    }
    for(size_t i=0;i<count;i++)free(ids[i]);
    if(error)return fail(out,error);
    fprintf(out,"{\"contract_version\":1,\"cleaned\":%zu,\"retained\":%zu}\n",cleaned,retained);return 0;
}
static int watch_remove(Locators *l,const Json *j,FILE *out){
    char *id=json_string(j,json_get(j,0,"watch_id"));const char *error=hash_valid(id)?NULL:"invalid_request";
    sqlite3_stmt *stmt=NULL;int runtime=-1,transaction=0;
    if(!error)error=lease(l);
    if(!error&&!prepare(l,&stmt,"SELECT status FROM locator_watches WHERE id=? AND owner=?"))error="store_failed";
    if(!error){
        bind(stmt,1,id);bind(stmt,2,l->owner);int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){const char *status=(const char*)sqlite3_column_text(stmt,0);if(!status||strcmp(status,"stopped"))error="watch_not_stopped";}
        else error=rc==SQLITE_DONE?"unknown_watch":"store_failed";
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error)error=sources_watch_lock(l->directory,id,&runtime);
    if(!error){if(!sql(l,"BEGIN IMMEDIATE"))error="store_busy";else transaction=1;}
    if(!error){sqlite3_int64 pending=scalar(l,"SELECT count(*) FROM watch_cleanup");if(pending<0)error="store_failed";else if(pending>=8)error="watch_cleanup_limit";}
    if(!error){
        if(!prepare(l,&stmt,"INSERT INTO watch_cleanup VALUES(?,?)"))error="store_failed";
        else {bind(stmt,1,id);bind(stmt,2,l->owner);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    const char *queries[]={"DELETE FROM watch_health WHERE id=?","DELETE FROM watch_checkpoints WHERE id=?","DELETE FROM locator_watches WHERE id=? AND owner=?"};
    for(size_t i=0;!error&&i<3;i++){
        if(!prepare(l,&stmt,queries[i]))error="store_failed";
        else {bind(stmt,1,id);if(i==2)bind(stmt,2,l->owner);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error)error=sources_interrupted(l->cfg);
    if(transaction){if(error)sql(l,"ROLLBACK");else if(!sql(l,"COMMIT")){sql(l,"ROLLBACK");error="store_failed";}}
    if(runtime>=0){close(runtime);runtime=-1;}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *crash_marker=getenv("SAMOSA_WATCH_REMOVE_COMMITTED");
    if(!error&&crash_marker){FILE *marker=fopen(crash_marker,"w");if(!marker)_exit(98);fputs("committed",marker);fclose(marker);for(;;)pause();}
#endif
    if(!error){int cleaned=0;const char *cleanup_error=cleanup_one(l,id,&cleaned);if(cleanup_error)cleaned=0;fprintf(out,"{\"contract_version\":1,\"watch_id\":\"%s\",\"state\":\"removed\",\"lease_cleanup\":\"%s\"}\n",id,cleaned?"complete":"retained");}
    if(runtime>=0)close(runtime);
    free(id);return error?fail(out,error):0;
}
static int watch_step(Locators *l,const Json *j,FILE *out){
    char *id=json_string(j,json_get(j,0,"watch_id")),*request=NULL,*scope=NULL,*result=NULL;
    size_t expected=0,cycle=0;const char *error=NULL;sqlite3_stmt *stmt=NULL;
    if(!hash_valid(id)||!json_uint(j,json_get(j,0,"expected_cycle"),&expected)||expected>=INT64_MAX)error="invalid_request";
    if(!error)error=lease(l);
    if(!error&&!prepare(l,&stmt,"SELECT request,scope,status FROM locator_watches WHERE id=? AND owner=?"))error="store_failed";
    if(!error){
        bind(stmt,1,id);bind(stmt,2,l->owner);int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            const char *status=(const char*)sqlite3_column_text(stmt,2);
            const char *saved_request=(const char*)sqlite3_column_text(stmt,0),*saved_scope=(const char*)sqlite3_column_text(stmt,1);
            request=saved_request?strdup(saved_request):NULL;scope=saved_scope?strdup(saved_scope):NULL;
            if(!request||!scope)error="resource_limit";
            else if(!status||strcmp(status,"registered"))error="watch_stopped";
        }
        else error=rc==SQLITE_DONE?"unknown_watch":"store_failed";
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(l,&stmt,"SELECT cycle,result,digest FROM watch_checkpoints WHERE id=?"))error="store_failed";
    if(!error){
        bind(stmt,1,id);int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            sqlite3_int64 stored=sqlite3_column_int64(stmt,0);
            if(stored<=0)error="corrupt_locator";else cycle=(size_t)stored;
            if(!error)error=watch_checkpoint(id,stored,(const char*)sqlite3_column_text(stmt,1),(const char*)sqlite3_column_text(stmt,2),&result);
        }
        else if(rc!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&cycle&&expected==cycle-1)goto done;
    if(!error&&expected!=cycle)error="watch_cycle_conflict";
    Observation rows[8]={0};for(size_t i=0;i<8;i++)rows[i].parent=rows[i].file=-1;
    char *operation=NULL,*encoded=NULL;size_t count=0;Json captured;
    if(!error&&(!json_parse(&captured,request,strlen(request))))error="corrupt_locator";
    if(!error)error=normalize(l,&captured,&operation,rows,&count,&encoded);
    if(!error){int host=json_get(&captured,0,"host_id");char *saved_host=json_string(&captured,host);if(!saved_host||strcmp(saved_host,l->host))error="scope_unavailable";free(saved_host);}
    if(!error)error=open_root(l);
    if(!error&&strcmp(scope,l->scope))error="scope_unavailable";
    if(!error)error=admit(l,rows,count,0);
    for(size_t i=0;!error&&i<count;i++){error=sources_interrupted(l->cfg);if(!error)error=observe(l,&rows[i]);}
    for(size_t i=0;!error&&i<count;i++)error=verify(l,&rows[i]);
    if(!error){int root=open(l->cfg->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat info;if(root<0||fstat(root,&info)||!same(&info,&l->root_info))error="source_changed";if(root>=0)close(root);}
    if(!error){free(result);result=NULL;error=commit(l,NULL,NULL,rows,count,&result,id,cycle+1);}
    for(size_t i=0;i<count;i++)free_observation(&rows[i]);
    free(operation);free(encoded);
    done: if(!error)fputs(result,out);else fail(out,error);
    free(id);free(request);free(scope);free(result);return error?1:0;
}
const char *source_watch_admit(const SourcesConfig *cfg,sqlite3 *db,int directory,const char *id,int *runtime){
    *runtime=-1;Locators l={.cfg=cfg,.db=db,.directory=directory,.lease=-1,.root=-1,.owner=cfg->owner?cfg->owner:"local-owner",.host=cfg->host_id?cfg->host_id:"local-endpoint"};
    const char *error=hash_valid(id)?schema(&l):"invalid_request";sqlite3_stmt *stmt=NULL;char *scope=NULL;
    if(!error)error=lease(&l);
    if(!error&&!prepare(&l,&stmt,"SELECT status,scope FROM locator_watches WHERE id=? AND owner=?"))error="store_failed";
    if(!error){
        bind(stmt,1,id);bind(stmt,2,l.owner);int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW){
            const char *status=(const char*)sqlite3_column_text(stmt,0),*saved=(const char*)sqlite3_column_text(stmt,1);
            if(!status||strcmp(status,"registered"))error="watch_stopped";
            else if(!hash_valid(saved))error="corrupt_locator";
            else {scope=strdup(saved);if(!scope)error="resource_limit";}
        }
        else error=rc==SQLITE_DONE?"unknown_watch":"store_failed";
    }
    sqlite3_finalize(stmt);
    if(!error)error=open_root(&l);
    if(!error&&strcmp(scope,l.scope))error="scope_unavailable";
    if(!error)error=sources_interrupted(cfg);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *admit_marker=getenv("SAMOSA_WATCH_ADMIT_MARKER"),*admit_stage=getenv("SAMOSA_WATCH_ADMIT_STAGE");
    if(!error&&admit_marker&&admit_stage&&!strcmp(admit_stage,"before")){FILE *f=fopen(admit_marker,"w");if(!f)_exit(98);fputs("held",f);fclose(f);for(;;)pause();}
#endif
    if(!error)error=sources_watch_create_lease(directory,id,runtime);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if(!error&&admit_marker&&admit_stage&&!strcmp(admit_stage,"after")){FILE *f=fopen(admit_marker,"w");if(!f)_exit(98);fputs("held",f);fclose(f);for(;;)pause();}
#endif
    if(l.root>=0)close(l.root);
    if(l.lease>=0)close(l.lease);
    free(scope);return error;
}
const char *source_watch_health(const SourcesConfig *cfg,sqlite3 *db,const char *id,const char *failure){
    Locators l={.cfg=cfg,.db=db,.owner=cfg->owner?cfg->owner:"local-owner"};
    if(!hash_valid(id)||(failure&&!identity_valid(failure)))return "invalid_request";
    const char *error=schema(&l);struct timespec now;
    if(!error&&(clock_gettime(CLOCK_REALTIME,&now)||now.tv_sec<0||(uint64_t)now.tv_sec>(uint64_t)INT64_MAX/1000))error="clock_unavailable";
    sqlite3_stmt *stmt=NULL;
    if(!error&&!prepare(&l,&stmt,"INSERT INTO watch_health SELECT id,1,?,?,?,? FROM locator_watches WHERE id=? AND owner=? ON CONFLICT(id) DO UPDATE SET attempts=MIN(attempts,9223372036854775806)+1,successes=CASE WHEN attempts=9223372036854775807 THEN successes ELSE successes+excluded.successes END,failures=CASE WHEN attempts=9223372036854775807 THEN failures ELSE failures+excluded.failures END,error=excluded.error,attempted_ms=excluded.attempted_ms"))error="store_failed";
    if(!error){
        sqlite3_bind_int(stmt,1,failure?0:1);sqlite3_bind_int(stmt,2,failure?1:0);bind(stmt,3,failure);sqlite3_bind_int64(stmt,4,(sqlite3_int64)now.tv_sec*1000+now.tv_nsec/1000000);bind(stmt,5,id);bind(stmt,6,l.owner);
        if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";else if(sqlite3_changes(db)!=1)error="unknown_watch";
    }
    sqlite3_finalize(stmt);return error;
}
const char *source_locator_schema(sqlite3 *db){Locators l={.db=db};return schema(&l);}
const char *source_locator_freshness(const SourcesConfig *cfg,sqlite3 *db,const char *scope,const char *path,const char *version,FILE *out){
    Locators l={.cfg=cfg,.db=db,.owner=cfg->owner?cfg->owner:"local-owner"};
    const char *error=NULL;sqlite3_stmt *stmt=NULL;char *result=NULL;
    if(!error&&!prepare(&l,&stmt,"SELECT result,digest FROM live_locators WHERE owner=? AND scope=? AND path=?"))error="store_failed";
    if(!error){
        bind(stmt,1,l.owner);bind(stmt,2,scope);bind(stmt,3,path);
        int rc=sqlite3_step(stmt);
        if(rc==SQLITE_ROW)error=validated_result((const char*)sqlite3_column_text(stmt,0),(const char*)sqlite3_column_text(stmt,1),&result);
        else if(rc!=SQLITE_DONE)error="store_failed";
    }
    sqlite3_finalize(stmt);
    if(error){free(result);return error;}
    if(!result){fputs("{\"status\":\"unobserved\",\"semantics\":\"committed_observation_history\"}",out);return NULL;}
    Json j;char *status=NULL,*hash=NULL,*recorded_path=NULL,*recorded_scope=NULL;size_t revision,observed;
    if(!json_parse(&j,result,strlen(result)))error="corrupt_locator";
    else {
        status=json_string(&j,json_get(&j,0,"status"));hash=json_string(&j,json_get(&j,0,"observed_content_hash"));
        recorded_path=json_string(&j,json_get(&j,0,"path"));recorded_scope=json_string(&j,json_get(&j,0,"scope_identity"));
        if(!status||(!strcmp(status,"available")&&!hash_valid(hash))||
           (strcmp(status,"available")&&strcmp(status,"changed")&&strcmp(status,"missing")&&strcmp(status,"unsupported")&&strcmp(status,"uncertain"))||
           !recorded_path||strcmp(recorded_path,path)||!recorded_scope||strcmp(recorded_scope,scope)||
           !json_uint(&j,json_get(&j,0,"revision"),&revision)||!revision||
           !json_uint(&j,json_get(&j,0,"observed_at_ms"),&observed))error="corrupt_locator";
    }
    if(!error){
        const char *qualified=((!strcmp(status,"available")||!strcmp(status,"changed"))&&hash_valid(hash))?(!strcmp(hash,version)?"available":"changed"):status;
        fprintf(out,"{\"status\":\"%s\",\"semantics\":\"committed_observation_history\",\"locator_revision\":%zu,\"observed_at_ms\":%zu}",qualified,revision,observed);
    }
    free(status);free(hash);free(recorded_path);free(recorded_scope);free(result);return error;
}
const char *source_locator_index_target(const SourcesConfig *cfg,sqlite3 *db,const char *scope,const char *path,sqlite3_int64 revision,char identity[65],char hash[65]){
    Locators l={.db=db,.owner=cfg->owner?cfg->owner:"local-owner"};sqlite3_stmt *stmt=NULL;char *result=NULL;const char *error=NULL;
    if(!prepare(&l,&stmt,"SELECT result,digest FROM live_locators WHERE owner=? AND scope=? AND path=?"))return "store_failed";
    bind(stmt,1,l.owner);bind(stmt,2,scope);bind(stmt,3,path);int rc=sqlite3_step(stmt);
    if(rc==SQLITE_ROW)error=validated_result((const char*)sqlite3_column_text(stmt,0),(const char*)sqlite3_column_text(stmt,1),&result);
    else error=rc==SQLITE_DONE?"stale_observation":"store_failed";
    sqlite3_finalize(stmt);
    Json j;char *saved_path=NULL,*saved_scope=NULL,*saved_identity=NULL,*saved_hash=NULL,*status=NULL;size_t saved_revision=0;
    if(!error){
        if(!json_parse(&j,result,strlen(result)))error="corrupt_locator";
        else {saved_path=json_string(&j,json_get(&j,0,"path"));saved_scope=json_string(&j,json_get(&j,0,"scope_identity"));saved_identity=json_string(&j,json_get(&j,0,"file_identity"));saved_hash=json_string(&j,json_get(&j,0,"observed_content_hash"));status=json_string(&j,json_get(&j,0,"status"));
            if(!saved_path||strcmp(saved_path,path)||!saved_scope||strcmp(saved_scope,scope)||!json_uint(&j,json_get(&j,0,"revision"),&saved_revision))error="corrupt_locator";
            else if(saved_revision!=(size_t)revision)error="stale_observation";
            else if(!status||(strcmp(status,"available")&&strcmp(status,"changed"))||!hash_valid(saved_identity)||!hash_valid(saved_hash))error="live_unavailable";
            else {memcpy(identity,saved_identity,65);memcpy(hash,saved_hash,65);}
        }
    }
    free(result);free(saved_path);free(saved_scope);free(saved_identity);free(saved_hash);free(status);return error;
}
int source_locator_dispatch(const SourcesConfig *cfg,const char *input,size_t length,sqlite3 *db,int directory,FILE *out){
    Json j;
    if(!json_parse(&j,input,length))return fail(out,"invalid_json");
    char *op=json_string(&j,json_get(&j,0,"op"));
    if(!op)return fail(out,"invalid_request");
    Locators l={
        .cfg=cfg,.db=db,.directory=directory,.lease=-1,.root=-1,.owner=cfg->owner?cfg->owner:"local-owner",.host=cfg->host_id?cfg->host_id:"local-endpoint"
    }
    ;
    const char *error=identity_valid(l.owner)&&identity_valid(l.host)?schema(&l):"invalid_configuration";
    int rc=1;
    char *response=NULL;
    size_t response_length=0;
    FILE *stream=NULL;
    if(!error){
        stream=open_memstream(&response,&response_length);
        if(!stream)error="resource_limit";
    }
    if(!error){
        if(!strcmp(op,"locator_watch_create"))rc=watch_create(&l,&j,stream);
        else if(!strcmp(op,"locator_watch_cleanup"))rc=watch_cleanup(&l,stream);
        else if(!strcmp(op,"locator_watch_remove"))rc=watch_remove(&l,&j,stream);
        else if(!strcmp(op,"locator_watch_step"))rc=watch_step(&l,&j,stream);
        else if(!strcmp(op,"locator_watch_get"))rc=watch_control(&l,&j,0,stream);
        else if(!strcmp(op,"locator_watch_stop"))rc=watch_control(&l,&j,1,stream);
        else if(!strcmp(op,"locator_reconcile"))rc=reconcile(&l,&j,stream);
        else if(!strcmp(op,"locator_read"))rc=live_read(&l,&j,stream);
        else if(!strcmp(op,"locator_get"))rc=get(&l,&j,stream);
        else error="unsupported_operation";
    }
    if(stream&&fclose(stream))error="resource_limit";
    if(l.root>=0)close(l.root);
    if(l.lease>=0)close(l.lease);
    size_t response_limit=!strcmp(op,"locator_read")?2u*1048576u+4096u:RESPONSE_MAX;
    free(op);
    if(!error&&response_length>response_limit)error="response_too_large";
    if(!error)fwrite(response,1,response_length,out);
    free(response);
    return error?fail(out,error):rc;
}
