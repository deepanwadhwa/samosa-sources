#define _POSIX_C_SOURCE 200809L
#include "fetch.h"
#include "native.h"
#include "uploads.h"
#include "control.h"
#include "sha256.h"
#include "fetch_public.h"
#ifdef SAMOSA_SOURCES_TEST_FAULTS
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static void hook(const char *name){
    const char *marker=getenv(name);if(!marker||!*marker)return;
    int fd=open(marker,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return;close(fd);
    char release[4096];int n=snprintf(release,sizeof(release),"%s.release",marker);if(n<0||(size_t)n>=sizeof(release))return;
    for(int i=0;i<1000&&access(release,F_OK);i++){struct timespec t={0,10000000};nanosleep(&t,NULL);}
}
#endif
static const char *names[]={"text","pdf","html","docx","image","wav","media"};
static const char *media[]={"text/plain","application/pdf","text/html","application/vnd.openxmlformats-officedocument.wordprocessingml.document","image/raster","audio/wav","application/x-samosa-encoded-media"};
static const int readers[]={0,1,2,3,4,6,7};
static int failure(FILE *out,const char *error){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",error);return 1;}
static int key_valid(const char *key){
    if(!key||!*key||strlen(key)>128)return 0;
    for(const unsigned char *p=(const unsigned char *)key;*p;p++)if(*p>127||(!isalnum(*p)&&!strchr("-_:.",*p)))return 0;
    return 1;
}
static void bind_text(sqlite3_stmt *s,int i,const char *v){sqlite3_bind_text(s,i,v,-1,SQLITE_TRANSIENT);}
/* A receipt is evidence only while its bytes, shape, acquisition parameters,
 * immutable snapshot and owning format claim all still agree. */
static const char *lookup(sqlite3 *db,const char *owner,const char *id,const char *url,int format,char **saved){
    *saved=NULL;sqlite3_stmt *s=NULL;const char *error=NULL;
    if(sqlite3_prepare_v2(db,"SELECT url,format,response,digest FROM fetch_receipts WHERE owner=? AND id=?",-1,&s,NULL)!=SQLITE_OK)return "store_failed";
    bind_text(s,1,owner);bind_text(s,2,id);int rc=sqlite3_step(s);
    if(rc==SQLITE_ROW){
        for(int i=0;i<4;i++)if(sqlite3_column_type(s,i)!=SQLITE_TEXT||!sqlite3_column_text(s,i)||strlen((const char *)sqlite3_column_text(s,i))!=(size_t)sqlite3_column_bytes(s,i))error="corrupt_acquisition";
        if(!error&&(strcmp((const char *)sqlite3_column_text(s,0),url)||strcmp((const char *)sqlite3_column_text(s,1),names[format])))error="idempotency_conflict";
        if(!error){
            const char *response=(const char *)sqlite3_column_text(s,2),*expected=(const char *)sqlite3_column_text(s,3);char hash[65];
            size_t n=(size_t)sqlite3_column_bytes(s,2);
            if(!n||n>32768||strlen(expected)!=64)error="corrupt_acquisition";
            else{hash_bytes(response,n,hash);if(strcmp(hash,expected))error="corrupt_acquisition";else if(!(*saved=strdup(response)))error="resource_limit";}
        }
    }else if(rc!=SQLITE_DONE)error="store_failed";
    sqlite3_finalize(s);s=NULL;
    if(error||!*saved)return error;
    Json j;size_t version=0,length=0,status=0;
    char *source=NULL,*content=NULL,*type=NULL,*identity=NULL,*acquisition=NULL,*origin=NULL,*kind=NULL;
    if(!json_parse(&j,*saved,strlen(*saved))||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count!=8)error="corrupt_acquisition";
    else{
        source=json_string(&j,json_get(&j,0,"source_id"));content=json_string(&j,json_get(&j,0,"version"));
        type=json_string(&j,json_get(&j,0,"media_type"));identity=json_string(&j,json_get(&j,0,"identity"));
        acquisition=json_string(&j,json_get(&j,0,"acquisition_id"));int a=json_get(&j,0,"acquisition");
        origin=json_string(&j,json_get(&j,a,"url"));kind=json_string(&j,json_get(&j,a,"format"));
        if(!source||strlen(source)!=64||!content||strcmp(source,content)||!type||strcmp(type,media[format])||
           !identity||strcmp(identity,"immutable_sha256")||!acquisition||strcmp(acquisition,id)||!origin||strcmp(origin,url)||!kind||strcmp(kind,names[format])||
           a<0||j.tokens[a].type!=JT_OBJECT||(j.tokens[a].count!=3&&j.tokens[a].count!=5)||!json_uint(&j,json_get(&j,a,"status"),&status)||status!=200||
           !json_uint(&j,json_get(&j,0,"contract_version"),&version)||version!=1||!json_uint(&j,json_get(&j,0,"bytes"),&length)||length>SOURCES_FETCH_MAX)error="corrupt_acquisition";
        if(!error&&j.tokens[a].count==5){
            char *final=json_string(&j,json_get(&j,a,"final_url"));int array=json_get(&j,a,"urls");
            if(!final||array<0||j.tokens[array].type!=JT_ARRAY||!j.tokens[array].count||j.tokens[array].count>(int)SOURCES_FETCH_MAX_HOPS)error="corrupt_acquisition";
            size_t count=0;char *seen[SOURCES_FETCH_MAX_HOPS]={0};
            if(!error)for(int token=array+1;!error&&token<j.tokens[array].next;token=j.tokens[token].next){
                char authority[257];seen[count]=json_string(&j,token);
                if(!seen[count]||sources_fetch_url_origin(seen[count],authority)||(!count&&strcmp(seen[count],url)))error="corrupt_acquisition";
                for(size_t i=0;!error&&i<count;i++)if(!strcmp(seen[i],seen[count]))error="corrupt_acquisition";
                count++;
            }
            if(!error&&strcmp(seen[count-1],final))error="corrupt_acquisition";
            for(size_t i=0;i<count;i++)free(seen[i]);
            free(final);
        }
    }
    if(!error){
        if(sqlite3_prepare_v2(db,"SELECT bytes FROM snapshots WHERE id=? AND EXISTS(SELECT 1 FROM source_claims WHERE owner=? AND id=? AND media=?) AND EXISTS(SELECT 1 FROM formats WHERE id=? AND media=?)",-1,&s,NULL)!=SQLITE_OK)error="store_failed";
        else{
            bind_text(s,1,source);bind_text(s,2,owner);bind_text(s,3,source);bind_text(s,4,media[format]);bind_text(s,5,source);bind_text(s,6,media[format]);
            if(sqlite3_step(s)!=SQLITE_ROW||sqlite3_column_type(s,0)!=SQLITE_BLOB||sqlite3_column_bytes(s,0)!=(int)length)error="corrupt_acquisition";
            else{char hash[65];hash_bytes(sqlite3_column_blob(s,0),length,hash);if(strcmp(hash,source))error="corrupt_acquisition";}
        }
        sqlite3_finalize(s);
    }
    free(source);free(content);free(type);free(identity);free(acquisition);free(origin);free(kind);
    if(error){free(*saved);*saved=NULL;}return error;
}
int source_fetch_dispatch(const SourcesConfig *cfg,const Json *j,sqlite3 *db,FILE *out){
    char *id=json_string(j,json_get(j,0,"acquisition_id")),*url=json_string(j,json_get(j,0,"url")),*format=json_string(j,json_get(j,0,"format"));
    int selected=-1;for(int i=0;i<7;i++)if(format&&!strcmp(format,names[i]))selected=i;
    const char *owner=cfg->owner?cfg->owner:"local-owner",*error=NULL;
    if(!key_valid(id)||!url||strlen(url)>2048||selected<0)error="invalid_request";
    char *receipt=NULL,*final_url=NULL,*urls=NULL;unsigned char *bytes=NULL;size_t length=0;
    if(!error)error=lookup(db,owner,id,url,selected,&receipt);
    if(!error&&!receipt)error=sources_native_fetch_capture(cfg,url,&bytes,&length,&final_url,&urls);
    int transaction=0;sqlite3_stmt *s=NULL;
    if(!error&&!receipt){if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)error="store_busy";else transaction=1;}
    if(!error&&transaction)error=lookup(db,owner,id,url,selected,&receipt);
    if(!error&&transaction&&!receipt){
        if(sqlite3_prepare_v2(db,"SELECT count(*) FROM fetch_receipts",-1,&s,NULL)!=SQLITE_OK||sqlite3_step(s)!=SQLITE_ROW)error="store_failed";
        else if(sqlite3_column_int64(s,0)>=128)error="acquisition_limit";
        sqlite3_finalize(s);s=NULL;
        if(!error){
            char *admission=NULL;size_t n=0;FILE *stream=open_memstream(&admission,&n);
            if(!stream)error="resource_limit";
            else{
                int failed=source_snapshot_accept(cfg,db,stream,bytes,length,final_url?final_url:url,readers[selected]);
                if(fclose(stream))error="resource_limit";
                else if(failed){Json r;if(!json_parse(&r,admission,n))error="invalid_response";else{char *code=json_string(&r,json_get(&r,json_get(&r,0,"error"),"code"));sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);transaction=0;fputs(admission,out);free(code);free(admission);free(id);free(url);free(format);free(bytes);free(final_url);free(urls);return 1;}}
            }
            if(!error){
                Json r;if(!json_parse(&r,admission,n))error="invalid_response";
                else{
                    size_t size=0;stream=open_memstream(&receipt,&size);
                    if(!stream)error="resource_limit";
                    else{
                        fwrite(admission,1,r.tokens[0].end-1,stream);fputs(",\"acquisition_id\":",stream);json_quote(stream,id,strlen(id));
                        fputs(",\"acquisition\":{\"url\":",stream);json_quote(stream,url,strlen(url));fputs(",\"format\":",stream);json_quote(stream,format,strlen(format));fputs(",\"status\":200",stream);
                        if(final_url){fputs(",\"final_url\":",stream);json_quote(stream,final_url,strlen(final_url));fputs(",\"urls\":",stream);fputs(urls,stream);}fputs("}}\n",stream);
                        if(fclose(stream))error="resource_limit";
                    }
                }
            }
            free(admission);
        }
        if(!error){
            char hash[65];hash_bytes(receipt,strlen(receipt),hash);
            if(sqlite3_prepare_v2(db,"INSERT INTO fetch_receipts VALUES(?,?,?,?,?,?)",-1,&s,NULL)!=SQLITE_OK)error="store_failed";
            else{bind_text(s,1,owner);bind_text(s,2,id);bind_text(s,3,url);bind_text(s,4,format);bind_text(s,5,receipt);bind_text(s,6,hash);if(sqlite3_step(s)!=SQLITE_DONE)error="store_failed";}
            sqlite3_finalize(s);
        }
    }
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if(!error&&transaction)hook("SAMOSA_FETCH_TEST_PRECOMMIT_MARKER");
#endif
    if(!error)error=sources_interrupted(cfg);
    if(transaction){if(error)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);error="store_failed";}}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if(!error&&transaction)hook("SAMOSA_FETCH_TEST_POSTCOMMIT_MARKER");
#endif
    if(error)failure(out,error);else fputs(receipt,out);
    free(id);free(url);free(format);free(receipt);free(bytes);free(final_url);free(urls);return error?1:0;
}
