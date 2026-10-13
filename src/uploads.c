#define _POSIX_C_SOURCE 200809L
#include "uploads.h"
#include "control.h"
#include "sha256.h"
static int failure(FILE *out,const char *code){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",code);return 1;}
static int key_valid(const char *key){if(!key||!*key||strlen(key)>128)return 0;for(const char *p=key;*p;p++)if(!isalnum((unsigned char)*p)&&!strchr("-_:.",*p))return 0;return 1;}
static int format_valid(const char *format){return format&&(!strcmp(format,"text")||!strcmp(format,"pdf")||!strcmp(format,"html")||!strcmp(format,"docx")||!strcmp(format,"image")||!strcmp(format,"wav")||!strcmp(format,"media"));}
static int decode_chunk(const char *text,unsigned char bytes[8192],size_t *length){
    const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if(!text)return 0;
    size_t n=strlen(text);*length=0;if(!n||n%4||n>10924)return 0;
    for(size_t i=0;i<n;i+=4){
        unsigned value[4]={0};int padding=0;
        for(size_t j=0;j<4;j++){
            if(text[i+j]=='='){if(j<2||i+4!=n)return 0;padding++;}
            else{const char *digit=strchr(alphabet,text[i+j]);if(!digit||padding)return 0;value[j]=(unsigned)(digit-alphabet);}
        }
        if(padding>2||(padding==2&&(value[1]&15))||(padding==1&&(value[2]&3)))return 0;
        unsigned packed=(value[0]<<18)|(value[1]<<12)|(value[2]<<6)|value[3];
        for(int j=0;j<3-padding;j++){if(*length>=8192)return 0;bytes[(*length)++]=(unsigned char)(packed>>(16-8*j));}
    }
    return *length>0;
}
static const char *completed_receipt(sqlite3 *db,const char *owner,const char *id,const char *format,size_t length,char **receipt){
    sqlite3_stmt *stmt=NULL;const char *error=NULL;
    if(sqlite3_prepare_v2(db,"SELECT response,digest,(SELECT count(*) FROM upload_chunks WHERE id=?) FROM upload_receipts WHERE owner=? AND id=?",-1,&stmt,NULL)!=SQLITE_OK)return "store_failed";
    sqlite3_bind_text(stmt,1,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,id,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(stmt)!=SQLITE_ROW)error="corrupt_upload";
    else{const char *saved=(const char*)sqlite3_column_text(stmt,0),*digest=(const char*)sqlite3_column_text(stmt,1);int n=sqlite3_column_bytes(stmt,0);char actual[65];
        if(sqlite3_column_type(stmt,0)!=SQLITE_TEXT||sqlite3_column_type(stmt,1)!=SQLITE_TEXT||!saved||!digest||n<1||n>2048||strlen(saved)!=(size_t)n||sqlite3_column_bytes(stmt,1)!=64||strlen(digest)!=64||sqlite3_column_int64(stmt,2))error="corrupt_upload";
        else{hash_bytes(saved,(size_t)n,actual);if(strcmp(actual,digest))error="corrupt_upload";else if(!(*receipt=strdup(saved)))error="resource_limit";}}
    sqlite3_finalize(stmt);stmt=NULL;if(error)return error;
    Json result;char *source=NULL,*version=NULL,*media=NULL;size_t bytes=0,contract=0;
    const char *expected=!strcmp(format,"media")?"application/x-samosa-encoded-media":!strcmp(format,"wav")?"audio/wav":!strcmp(format,"image")?"image/raster":!strcmp(format,"docx")?"application/vnd.openxmlformats-officedocument.wordprocessingml.document":!strcmp(format,"html")?"text/html":!strcmp(format,"pdf")?"application/pdf":"text/plain";
    if(!json_parse(&result,*receipt,strlen(*receipt)))error="corrupt_upload";
    else{source=json_string(&result,json_get(&result,0,"source_id"));version=json_string(&result,json_get(&result,0,"version"));media=json_string(&result,json_get(&result,0,"media_type"));
        if(!source||strlen(source)!=64||!version||strcmp(source,version)||!media||strcmp(media,expected)||!json_uint(&result,json_get(&result,0,"bytes"),&bytes)||bytes!=length||!json_uint(&result,json_get(&result,0,"contract_version"),&contract)||contract!=1)error="corrupt_upload";
        if(!error)for(size_t i=0;i<64;i++)if(json_hex(source[i])<0)error="corrupt_upload";
    }
    if(!error){
        if(sqlite3_prepare_v2(db,"SELECT length(bytes),typeof(bytes),(SELECT count(*) FROM source_claims WHERE owner=? AND id=? AND media=?),(SELECT count(*) FROM formats WHERE id=? AND media=?) FROM snapshots WHERE id=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
        else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,expected,-1,SQLITE_STATIC);sqlite3_bind_text(stmt,4,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,5,expected,-1,SQLITE_STATIC);sqlite3_bind_text(stmt,6,source,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)!=SQLITE_ROW||sqlite3_column_int64(stmt,0)!=(sqlite3_int64)length||strcmp((const char*)sqlite3_column_text(stmt,1),"blob")||sqlite3_column_int64(stmt,2)!=1||sqlite3_column_int64(stmt,3)!=1)error="corrupt_upload";
        }sqlite3_finalize(stmt);
    }
    free(source);free(version);free(media);return error;
}
const char *source_upload_reserved(sqlite3 *db,size_t *count,size_t *bytes){
    sqlite3_stmt *stmt=NULL;const char *error=NULL;
    const char *sql="SELECT count(*),coalesce(sum(status='open'),0),coalesce(sum(CASE WHEN status='open' THEN bytes ELSE 0 END),0),coalesce(sum(CASE WHEN typeof(bytes)!='integer' OR bytes<0 OR bytes>67108864 OR typeof(received)!='integer' OR received<0 OR received>bytes OR status NOT IN ('open','aborted','completed') OR format NOT IN ('text','pdf','html','docx','image','wav','media') OR (status='aborted' AND received!=0) OR (status='completed' AND received!=bytes) OR (status='open' AND received!=bytes AND received%8192!=0) THEN 1 ELSE 0 END),0) FROM upload_sessions";
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
    else if(sqlite3_step(stmt)!=SQLITE_ROW)error="corrupt_upload";
    else{sqlite3_int64 retained=sqlite3_column_int64(stmt,0),active=sqlite3_column_int64(stmt,1),reserved=sqlite3_column_int64(stmt,2);
        if(retained<0||retained>128||active<0||active>8||reserved<0||reserved>SOURCES_INPUT_MAX||sqlite3_column_int64(stmt,3))error="corrupt_upload";
        else{*count=(size_t)active;*bytes=(size_t)reserved;}}
    sqlite3_finalize(stmt);return error;
}
int source_upload_session(const SourcesConfig *cfg,const Json *request,const char *op,sqlite3 *db,FILE *out){
    const char *owner=cfg->owner?cfg->owner:"local-owner",*error=NULL;int begin=!strcmp(op,"upload_begin"),abort=!strcmp(op,"upload_abort"),append=!strcmp(op,"upload_append"),commit=!strcmp(op,"upload_commit");
    char *key=NULL,*format=NULL,*selected=NULL,*receipt=NULL,id[65];size_t length=0,received=0;char state[16]={0};size_t requested_offset=0,chunk_length=0;unsigned char chunk[8192];
    if(append){char *encoded=json_string(request,json_get(request,0,"data"));if(!json_uint(request,json_get(request,0,"offset"),&requested_offset)||!decode_chunk(encoded,chunk,&chunk_length))error="invalid_upload_chunk";free(encoded);}
    if(begin){
        key=json_string(request,json_get(request,0,"operation_id"));format=json_string(request,json_get(request,0,"format"));
        if(!key_valid(key)||!format_valid(format)||!json_uint(request,json_get(request,0,"bytes"),&length)||length>SOURCES_INPUT_MAX)error="invalid_request";
        if(!error){char material[258];int n=snprintf(material,sizeof(material),"%s\n%s",owner,key);if(n<0||(size_t)n>=sizeof(material))error="invalid_request";else hash_bytes(material,(size_t)n,id);}
    }else{selected=json_string(request,json_get(request,0,"upload_id"));if(!selected||strlen(selected)!=64)error="invalid_request";else{for(size_t i=0;i<64;i++)if(json_hex(selected[i])<0)error="invalid_request";memcpy(id,selected,65);}}
    int transaction=0,found=0;sqlite3_stmt *stmt=NULL;
    if(!error){if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)error="store_busy";else transaction=1;}
    if(!error){
        if(sqlite3_prepare_v2(db,"SELECT format,bytes,received,status FROM upload_sessions WHERE owner=? AND id=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
        else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(stmt);
            if(rc==SQLITE_ROW){found=1;const char *stored=(const char*)sqlite3_column_text(stmt,0),*status=(const char*)sqlite3_column_text(stmt,3);sqlite3_int64 total=sqlite3_column_int64(stmt,1),offset=sqlite3_column_int64(stmt,2);
                if(!format_valid(stored)||!status||(strcmp(status,"open")&&strcmp(status,"aborted")&&strcmp(status,"completed"))||sqlite3_column_type(stmt,1)!=SQLITE_INTEGER||sqlite3_column_type(stmt,2)!=SQLITE_INTEGER||total<0||total>SOURCES_INPUT_MAX||offset<0||offset>total)error="corrupt_upload";
                else if(begin&&(strcmp(format,stored)||length!=(size_t)total))error="idempotency_conflict";
                else{if(!begin){format=strdup(stored);if(!format)error="resource_limit";}length=(size_t)total;received=(size_t)offset;snprintf(state,sizeof(state),"%s",status);}
            }else if(rc!=SQLITE_DONE)error="store_failed";
        }sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error&&found&&strcmp(state,"completed")){
        const char *sql="SELECT count(*),coalesce(sum(length(bytes)),0),coalesce(sum(CASE WHEN typeof(offset)!='integer' OR offset<0 OR offset%8192!=0 OR offset>=? OR typeof(bytes)!='blob' OR length(bytes)!=min(8192,?-offset) OR typeof(digest)!='text' OR length(digest)!=64 THEN 1 ELSE 0 END),0) FROM upload_chunks WHERE owner=? AND id=?";
        if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
        else{sqlite3_bind_int64(stmt,1,(sqlite3_int64)received);sqlite3_bind_int64(stmt,2,(sqlite3_int64)length);sqlite3_bind_text(stmt,3,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,4,id,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)!=SQLITE_ROW)error="store_failed";
            else if(sqlite3_column_int64(stmt,0)!=(sqlite3_int64)((received+8191)/8192)||sqlite3_column_int64(stmt,1)!=(sqlite3_int64)received||sqlite3_column_int64(stmt,2)||(!strcmp(state,"aborted")&&received)||(received!=length&&received%8192))error="corrupt_upload";
        }sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error&&found&&!strcmp(state,"completed")){
        if(received!=length)error="corrupt_upload";
        else error=completed_receipt(db,owner,id,format,length,&receipt);
        if(!commit){free(receipt);receipt=NULL;}
    }
    if(!error&&!found&&!begin)error="unknown_upload";
    if(!error&&!found){
        size_t checked_count=0,checked_bytes=0;error=source_upload_reserved(db,&checked_count,&checked_bytes);
    }
    if(!error&&!found){
        const char *sql="SELECT count(*),coalesce(sum(CASE WHEN status='open' THEN 1 ELSE 0 END),0),coalesce(sum(CASE WHEN status='open' THEN bytes ELSE 0 END),0),(SELECT count(*) FROM snapshots),(SELECT coalesce(sum(length(bytes)),0) FROM snapshots) FROM upload_sessions";
        if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK||sqlite3_step(stmt)!=SQLITE_ROW)error="store_failed";
        else{sqlite3_int64 retained=sqlite3_column_int64(stmt,0),active=sqlite3_column_int64(stmt,1),reserved=sqlite3_column_int64(stmt,2),snapshots=sqlite3_column_int64(stmt,3),stored=sqlite3_column_int64(stmt,4);size_t cap=cfg->byte_limit?cfg->byte_limit:134217728u,count=cfg->snapshot_limit?cfg->snapshot_limit:128u;if(cap>134217728u)cap=134217728u;if(count>128)count=128;
            if(retained<0||active<0||reserved<0||reserved>SOURCES_INPUT_MAX||snapshots<0||stored<0)error="corrupt_upload";
            else if(retained>=128)error="upload_history_limit";
            else if(active>=8||length>SOURCES_INPUT_MAX-(size_t)reserved)error="upload_capacity";
            else if((sqlite3_uint64)(snapshots+active)>=count||(sqlite3_uint64)stored+(sqlite3_uint64)reserved+length>cap)error="storage_limit";
        }sqlite3_finalize(stmt);stmt=NULL;
        if(!error){if(sqlite3_prepare_v2(db,"INSERT INTO upload_sessions VALUES(?,?,?,?,?,0,'open')",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
            else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,key,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,4,format,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,5,(sqlite3_int64)length);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;snprintf(state,sizeof(state),"open");}
    }
    if(!error&&append){
        if(strcmp(state,"open"))error="upload_closed";
        else if(requested_offset>received||requested_offset%8192||requested_offset>=length||chunk_length>length-requested_offset||(chunk_length!=8192&&chunk_length!=length-requested_offset))error="upload_offset";
        char digest[65];hash_bytes(chunk,chunk_length,digest);
        if(!error&&requested_offset<received){
            if(sqlite3_prepare_v2(db,"SELECT bytes,digest FROM upload_chunks WHERE owner=? AND id=? AND offset=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
            else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,3,(sqlite3_int64)requested_offset);
                if(sqlite3_step(stmt)!=SQLITE_ROW)error="corrupt_upload";
                else{int saved=sqlite3_column_bytes(stmt,0);const void *data=sqlite3_column_blob(stmt,0);const char *stored=(const char*)sqlite3_column_text(stmt,1);char actual[65];
                    if(sqlite3_column_type(stmt,0)!=SQLITE_BLOB||saved<1||saved>8192||!data||!stored||strlen(stored)!=64)error="corrupt_upload";
                    else{hash_bytes(data,(size_t)saved,actual);if(strcmp(actual,stored))error="corrupt_upload";
                        else if((size_t)saved!=chunk_length||memcmp(data,chunk,chunk_length)||strcmp(stored,digest))error="idempotency_conflict";}}
            }sqlite3_finalize(stmt);stmt=NULL;
        }else if(!error){
            if(sqlite3_prepare_v2(db,"SELECT count(*) FROM upload_chunks",-1,&stmt,NULL)!=SQLITE_OK||sqlite3_step(stmt)!=SQLITE_ROW)error="store_failed";
            else if(sqlite3_column_int64(stmt,0)>=8200)error="upload_chunk_limit";
            sqlite3_finalize(stmt);stmt=NULL;
            if(!error){if(sqlite3_prepare_v2(db,"INSERT INTO upload_chunks VALUES(?,?,?,?,?)",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
                else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(stmt,3,(sqlite3_int64)received);sqlite3_bind_blob(stmt,4,chunk,(int)chunk_length,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,5,digest,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
                sqlite3_finalize(stmt);stmt=NULL;}
            if(!error){if(sqlite3_prepare_v2(db,"UPDATE upload_sessions SET received=? WHERE owner=? AND id=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
                else{sqlite3_bind_int64(stmt,1,(sqlite3_int64)(received+chunk_length));sqlite3_bind_text(stmt,2,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,id,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
                sqlite3_finalize(stmt);stmt=NULL;if(!error)received+=chunk_length;}
        }
    }
    if(!error&&commit){
        if(!strcmp(state,"aborted"))error="upload_closed";
        else if(!strcmp(state,"completed")){ /* Receipt was validated above. */ }
        else if(received!=length)error="upload_incomplete";
        else{
            unsigned char *bytes=malloc(length+1);size_t offset=0;if(!bytes)error="resource_limit";
            if(!error&&sqlite3_prepare_v2(db,"SELECT offset,bytes,digest FROM upload_chunks WHERE owner=? AND id=? ORDER BY offset",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
            if(!error){sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);int rc;
                while(!error&&(rc=sqlite3_step(stmt))==SQLITE_ROW){int n=sqlite3_column_bytes(stmt,1);const void *data=sqlite3_column_blob(stmt,1);const char *digest=(const char*)sqlite3_column_text(stmt,2);char actual[65];
                    if(sqlite3_column_int64(stmt,0)!=(sqlite3_int64)offset||sqlite3_column_type(stmt,1)!=SQLITE_BLOB||n<1||n>8192||!data||!digest||(size_t)n>length-offset)error="corrupt_upload";
                    else{hash_bytes(data,(size_t)n,actual);if(strcmp(actual,digest))error="corrupt_upload";else{memcpy(bytes+offset,data,(size_t)n);offset+=(size_t)n;error=sources_interrupted(cfg);}}}
                if(!error&&(rc!=SQLITE_DONE||offset!=length))error="corrupt_upload";
            }sqlite3_finalize(stmt);stmt=NULL;
            /* Release the reservation inside this same transaction before admission. */
            if(!error&&sqlite3_prepare_v2(db,"UPDATE upload_sessions SET status='completed' WHERE owner=? AND id=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
            if(!error){sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;
            if(!error&&sqlite3_prepare_v2(db,"DELETE FROM upload_chunks WHERE owner=? AND id=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
            if(!error){sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;
            if(!error){size_t n=0;FILE *output=open_memstream(&receipt,&n);if(!output)error="resource_limit";
                else{int reader=!strcmp(format,"pdf")?1:!strcmp(format,"html")?2:!strcmp(format,"docx")?3:!strcmp(format,"image")?4:!strcmp(format,"wav")?6:!strcmp(format,"media")?7:0;
                    int failed=source_snapshot_accept(cfg,db,output,bytes,length,"upload:",reader);if(fclose(output))error="store_failed";
                    if(failed&&!error){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);transaction=0;fputs(receipt,out);free(bytes);free(key);free(format);free(selected);free(receipt);return 1;}
                }}
            free(bytes);
            if(!error){char digest[65];hash_bytes(receipt,strlen(receipt),digest);
                if(sqlite3_prepare_v2(db,"INSERT INTO upload_receipts VALUES(?,?,?,?)",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
                else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,3,receipt,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,4,digest,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
        }
    }
    if(!error&&abort&&!strcmp(state,"open")){
        if(sqlite3_prepare_v2(db,"UPDATE upload_sessions SET status='aborted',received=0 WHERE owner=? AND id=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
        else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;
        if(!error){if(sqlite3_prepare_v2(db,"DELETE FROM upload_chunks WHERE owner=? AND id=?",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
            else{sqlite3_bind_text(stmt,1,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_text(stmt,2,id,-1,SQLITE_TRANSIENT);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}sqlite3_finalize(stmt);stmt=NULL;}
        received=0;snprintf(state,sizeof(state),"aborted");
    }
    if(!error)error=sources_interrupted(cfg);
    if(transaction){if(error)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);error="store_failed";}}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if(!error&&transaction&&commit&&cfg->native_test_event)
        cfg->native_test_event(cfg->native_test_context,0,"upload_committed",(SourcesConfig*)cfg);
#endif
    free(key);free(format);free(selected);if(error){free(receipt);return failure(out,error);}
    if(receipt){fputs(receipt,out);free(receipt);return 0;}
    fprintf(out,"{\"contract_version\":1,\"upload_id\":\"%s\",\"status\":\"%s\",\"bytes\":%zu,\"received_bytes\":%zu}\n",id,state,length,received);return 0;
}
