/* Shared audio/visual association, retained history and retrieval implementation.
 * Included twice with fixed internal names and SQL namespaces. */
static const char *IM_REPORT(Index *index,const char *id,size_t expected,char **report,size_t *completed){
    *report=NULL;*completed=0;size_t bytes=0,count=0;FILE *out=open_memstream(report,&bytes);if(!out)return "resource_limit";
    const char *error=NULL;sqlite3_stmt *stmt=NULL;fputc('[',out);
    if(!prepare(index,&stmt,"SELECT a.ordinal,a.source,a." IM_KIND "_job,i.status FROM index_" IM_KIND " a JOIN index_items i ON i.job=a.job AND i.ordinal=a.ordinal WHERE a.job=? ORDER BY a.ordinal"))error="store_failed";
    if(!error)text(stmt,1,id);
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}
        char *source=column(stmt,1),*job=column(stmt,2),*state=column(stmt,3);sqlite3_int64 ordinal=sqlite3_column_int64(stmt,0);
        if(!source||!job||!state||strcmp(state,"queued_" IM_KIND "")||ordinal<0||ordinal>=256||count>=expected)error="corrupt_index";
        char *body=NULL;size_t length=0;FILE *stream=NULL;int done=0;
        if(!error){stream=open_memstream(&body,&length);if(!stream)error="resource_limit";else {error=IM_QUEUE_REPORT(index->cfg,index->db,job,source,stream,&done);if(fclose(stream)&&!error)error="resource_limit";}}
        if(!error){fprintf(out,"%s{\"ordinal\":%lld,\"" IM_KIND "_job\":",count?",":"",(long long)ordinal);fwrite(body,1,length,out);fputc('}',out);*completed+=(size_t)done;count++;}
        free(source);free(job);free(state);free(body);
    }
    sqlite3_finalize(stmt);if(!error&&count!=expected)error="corrupt_index";fputc(']',out);if(fclose(out)&&!error)error="resource_limit";
    if(error){free(*report);*report=NULL;}return error;
}
static int IM_READ(Index *index,const Json *request,FILE *out){
    char *job=json_string(request,json_get(request,0,"job_id")),*audio=json_string(request,json_get(request,0,"" IM_KIND "_job_id"));size_t ordinal=0;
    const char *error=NULL;char *source=NULL;sqlite3_stmt *stmt=NULL;
    if(!job||strlen(job)!=64||strspn(job,"0123456789abcdef")!=64||!audio||strlen(audio)!=64||strspn(audio,"0123456789abcdef")!=64||!json_uint(request,json_get(request,0,"ordinal"),&ordinal)||ordinal>=IM_MAX_WINDOWS)error="invalid_request";
    if(!error&&!prepare(index,&stmt,"SELECT a.source,s.scope FROM (SELECT job,source," IM_KIND "_job FROM index_" IM_KIND " UNION SELECT job,source," IM_KIND "_job FROM index_" IM_KIND "_history) a JOIN index_jobs j ON j.id=a.job JOIN index_root_scopes s ON s.job=j.id WHERE a.job=? AND j.owner=? AND a." IM_KIND "_job=? LIMIT 2"))error="store_failed";
    if(!error){text(stmt,1,job);text(stmt,2,index->owner);text(stmt,3,audio);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index_artifact":"store_failed";else {source=column(stmt,0);char actual[65];const char *scope=(const char*)sqlite3_column_text(stmt,1);if(!source||!scope||strlen(scope)!=64||strspn(scope,"0123456789abcdef")!=64)error="corrupt_index";else {error=inventory_scope_identity(index->cfg,"",actual);if(!error&&strcmp(scope,actual))error="scope_unavailable";}if(!error&&sqlite3_step(stmt)!=SQLITE_DONE)error="corrupt_index";}}
    sqlite3_finalize(stmt);
    if(!error)error=IM_QUEUE_READ(index->cfg,index->db,audio,source,ordinal,out);
    free(source);free(job);free(audio);return error?fail(out,error):0;
}
static int IM_HISTORY(Index *index,const Json *request,FILE *out){
    char *job=json_string(request,json_get(request,0,"job_id"));size_t after=0;
    const char *error=NULL;sqlite3_stmt *stmt=NULL;int token=json_get(request,0,"after");
    if(!job||strlen(job)!=64||strspn(job,"0123456789abcdef")!=64||(token>=0&&!json_uint(request,token,&after))||after>INT64_MAX)error="invalid_request";
    if(!error&&!prepare(index,&stmt,"SELECT s.scope FROM index_jobs j JOIN index_root_scopes s ON s.job=j.id WHERE j.id=? AND j.owner=?"))error="store_failed";
    if(!error){text(stmt,1,job);text(stmt,2,index->owner);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index":"store_failed";else {char actual[65];const char *scope=(const char*)sqlite3_column_text(stmt,0);if(!scope||strlen(scope)!=64||strspn(scope,"0123456789abcdef")!=64)error="corrupt_index";else {error=inventory_scope_identity(index->cfg,"",actual);if(!error&&strcmp(scope,actual))error="scope_unavailable";}}}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(index,&stmt,"SELECT ordinal,source," IM_KIND "_job,superseded_revision,rowid FROM index_" IM_KIND "_history WHERE job=? AND rowid>? ORDER BY rowid LIMIT 17"))error="store_failed";
    if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)after);}
    size_t count=0,next=after;int more=0;
    if(!error)fprintf(out,"{\"contract_version\":1,\"job_id\":\"%s\",\"semantics\":\"retained_derived_" IM_KIND "\",\"artifacts\":[",job);
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}if(count==16){more=1;break;}
        sqlite3_int64 ordinal=sqlite3_column_int64(stmt,0),revision=sqlite3_column_int64(stmt,3);char *source=column(stmt,1),*audio=column(stmt,2),*body=NULL;size_t bytes=0;int completed=0;FILE *stream=NULL;
        sqlite3_int64 cursor=sqlite3_column_int64(stmt,4);
        if(cursor<=0||sqlite3_column_type(stmt,4)!=SQLITE_INTEGER||ordinal<0||ordinal>=256||revision<=0||sqlite3_column_type(stmt,0)!=SQLITE_INTEGER||sqlite3_column_type(stmt,3)!=SQLITE_INTEGER||!source||!audio)error="corrupt_index";
        if(!error){stream=open_memstream(&body,&bytes);if(!stream)error="resource_limit";else {error=IM_QUEUE_REPORT(index->cfg,index->db,audio,source,stream,&completed);if(fclose(stream)&&!error)error="resource_limit";}}
        if(!error){fprintf(out,"%s{\"ordinal\":%lld,\"superseded_revision\":%lld,\"cursor\":%lld,\"" IM_KIND "_job\":",count?",":"",(long long)ordinal,(long long)revision,(long long)cursor);fwrite(body,1,bytes,out);fputc('}',out);count++;next=(size_t)cursor;}
        free(source);free(audio);free(body);
    }
    sqlite3_finalize(stmt);if(!error)fprintf(out,"],\"returned_artifacts\":%zu,\"next_after\":%zu,\"has_more\":%s}\n",count,next,more?"true":"false");free(job);return error?fail(out,error):0;
}

static const char *IM_COMMIT(Index *index,const char *job,size_t ordinal,sqlite3_int64 revision,const char *scope,const char *path,const char *identity,const char *source,size_t duration,char failure[65]){
    if(!execsql(index,"BEGIN IMMEDIATE"))return "store_busy";
    char checked_identity[65],checked_hash[65],actual_scope[65],audio_job[65];
    const char *error=source_locator_index_target(index->cfg,index->db,scope,path,revision,checked_identity,checked_hash);
    if(!error&&(strcmp(identity,checked_identity)||strcmp(source,checked_hash)))error="stale_observation";
    if(!error)error=inventory_scope_identity(index->cfg,"",actual_scope);
    if(!error&&strcmp(actual_scope,scope))error="stale_scope";
    sqlite3_int64 retained=scalar(index,"SELECT (SELECT count(*) FROM index_audio_history)+(SELECT count(*) FROM index_visual_history)");
    if(!error&&retained<0)error="store_failed";
    if(!error&&retained>=256)error="index_limit";
    if(!error)error=IM_ADMIT(index->cfg,index->db,source,duration,audio_job,failure);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *admitted_marker=getenv(IM_FAULT_MARKER);
    if(!error&&admitted_marker){FILE *ready=fopen(admitted_marker,"w");if(!ready)_exit(98);fputs("admitted",ready);fclose(ready);for(;;)pause();}
#endif
    sqlite3_stmt *stmt=NULL;
    if(!error&&!prepare(index,&stmt,"INSERT INTO index_" IM_KIND "_history SELECT job,ordinal,source," IM_KIND "_job,? FROM index_" IM_KIND " WHERE job=? AND ordinal=?"))error="store_failed";
    if(!error){sqlite3_bind_int64(stmt,1,revision);text(stmt,2,job);sqlite3_bind_int64(stmt,3,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="corrupt_index";}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(index,&stmt,"UPDATE index_" IM_KIND " SET source=?," IM_KIND "_job=? WHERE job=? AND ordinal=?"))error="store_failed";
    if(!error){text(stmt,1,source);text(stmt,2,audio_job);text(stmt,3,job);sqlite3_bind_int64(stmt,4,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="corrupt_index";}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(index,&stmt,"DELETE FROM index_pending WHERE job=? AND ordinal=? AND revision=?"))error="store_failed";
    if(!error){text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);sqlite3_bind_int64(stmt,3,revision);if(sqlite3_step(stmt)!=SQLITE_DONE||sqlite3_changes(index->db)!=1)error="stale_observation";}
    sqlite3_finalize(stmt);stmt=NULL;
    const char *cleanup[]={"DELETE FROM index_attempts WHERE job=? AND ordinal=?","DELETE FROM index_retry WHERE job=? AND ordinal=?"};
    for(size_t i=0;!error&&i<2;i++){
        if(!prepare(index,&stmt,cleanup[i]))error="store_failed";
        else {text(stmt,1,job);sqlite3_bind_int64(stmt,2,(sqlite3_int64)ordinal);if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";}
        sqlite3_finalize(stmt);stmt=NULL;
    }
    if(!error)error=sources_interrupted(index->cfg);
    if(error)execsql(index,"ROLLBACK");else if(!execsql(index,"COMMIT")){execsql(index,"ROLLBACK");error="store_failed";}
    return error;
}

typedef struct {Index *index;FILE *out;const char *query,*locator,*scope;size_t count,scanned;int truncated;} IM_SEARCH_TYPE;
static const char *IM_CANDIDATE(void *context,const char *job,size_t ordinal,const char *capture){
    IM_SEARCH_TYPE *search=context;search->scanned++;const char *error=sources_interrupted(search->index->cfg);Json j;char *value=NULL,*version=NULL;
    if(!error&&(!json_parse(&j,capture,strlen(capture))||!(value=json_string(&j,json_get(&j,0,"text")))))error="corrupt_" IM_KIND "_index";
    if(!error&&match_ascii(value,search->query)){
        if(search->count==10)search->truncated=1;
        else {
            if(search->count++)fputc(',',search->out);
            fprintf(search->out,"{\"" IM_KIND "_job_id\":\"%s\",\"ordinal\":%zu,\"locator\":",job,ordinal);json_quote(search->out,search->locator,strlen(search->locator));
            const char *keys[]={"kind","artifact_type","source_id","version","artifact_id","range","coverage","provenance"};
            for(size_t i=0;!error&&i<sizeof(keys)/sizeof(*keys);i++){int token=json_get(&j,0,keys[i]);if(token<0)error="corrupt_" IM_KIND "_index";else {fputc(',',search->out);json_quote(search->out,keys[i],strlen(keys[i]));fputc(':',search->out);raw(search->out,&j,token);}}
            size_t length=strlen(value);if(length>512){length=512;while(length&&((unsigned char)value[length]&192)==128)length--;}
            if(!error){fputs(",\"derived_preview\":",search->out);json_quote(search->out,value,length);version=json_string(&j,json_get(&j,0,"version"));if(!version)error="corrupt_" IM_KIND "_index";}
            if(!error){fputs(",\"live_observation\":",search->out);error=source_locator_freshness(search->index->cfg,search->index->db,search->scope,search->locator,version,search->out);}
            if(!error)fputc('}',search->out);
        }
    }
    free(value);free(version);return error;
}
static int IM_SEARCH(Index *index,const Json *request,FILE *out){
    char *job=json_string(request,json_get(request,0,"job_id")),*query=json_string(request,json_get(request,0,"query")),*scope=NULL;
    sqlite3_stmt *stmt=NULL;const char *error=NULL;size_t jobs=0,uncaptured=0;
    if(!job||strlen(job)!=64||strspn(job,"0123456789abcdef")!=64||!query||!*query||strlen(query)>128)error="invalid_request";
    if(!error&&!prepare(index,&stmt,"SELECT s.scope FROM index_jobs j JOIN index_root_scopes s ON s.job=j.id WHERE j.id=? AND j.owner=?"))error="store_failed";
    if(!error){text(stmt,1,job);text(stmt,2,index->owner);int rc=sqlite3_step(stmt);if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_index":"store_failed";else {scope=column(stmt,0);char actual[65];if(!scope||strlen(scope)!=64||strspn(scope,"0123456789abcdef")!=64)error="corrupt_index";else {error=inventory_scope_identity(index->cfg,"",actual);if(!error&&strcmp(scope,actual))error="scope_unavailable";}}}
    sqlite3_finalize(stmt);stmt=NULL;
    if(!error&&!prepare(index,&stmt,"SELECT a.source,a." IM_KIND "_job,min(a.ordinal) FROM (SELECT job,ordinal,source," IM_KIND "_job FROM index_" IM_KIND " UNION SELECT job,ordinal,source," IM_KIND "_job FROM index_" IM_KIND "_history) a WHERE a.job=? GROUP BY a.source,a." IM_KIND "_job ORDER BY a." IM_KIND "_job LIMIT 9"))error="store_failed";
    if(!error)text(stmt,1,job);
    char *body=NULL;size_t bytes=0;FILE *buffer=NULL;
    if(!error&&!(buffer=open_memstream(&body,&bytes)))error="resource_limit";
    IM_SEARCH_TYPE search={.index=index,.out=buffer,.query=query,.scope=scope};
    if(!error)fprintf(buffer,"{\"contract_version\":1,\"job_id\":\"%s\",\"match_policy\":\"derived_ascii_case_insensitive_substring\",\"exhaustive_current_filesystem\":false,\"candidates\":[",job);
    while(!error){
        int rc=sqlite3_step(stmt);if(rc==SQLITE_DONE)break;if(rc!=SQLITE_ROW){error="store_failed";break;}if(++jobs>8){error="corrupt_index";break;}
        char *source=column(stmt,0),*audio=column(stmt,1),*locator=NULL;sqlite3_int64 ordinal=sqlite3_column_int64(stmt,2);sqlite3_stmt *path=NULL;
        if(!source||!audio||ordinal<0||ordinal>=256)error="corrupt_index";
        if(!error&&!prepare(index,&path,"SELECT locator FROM index_items WHERE job=? AND ordinal=?"))error="store_failed";
        if(!error){text(path,1,job);sqlite3_bind_int64(path,2,ordinal);if(sqlite3_step(path)!=SQLITE_ROW||!(locator=column(path,0)))error="corrupt_index";}
        sqlite3_finalize(path);search.locator=locator;
        if(!error)error=IM_QUEUE_SCAN(index->cfg,index->db,audio,source,IM_CANDIDATE,&search,&uncaptured);
        free(source);free(audio);free(locator);
    }
    sqlite3_finalize(stmt);
    if(!error)fprintf(buffer,"],\"returned_candidates\":%zu,\"scanned_captures\":%zu,\"uncaptured_windows\":%zu,\"candidate_coverage\":\"%s\"}\n",search.count,search.scanned,uncaptured,search.truncated?"partial":"complete_for_retained_captures");
    if(buffer&&fclose(buffer)&&!error)error="resource_limit";
    if(!error)fwrite(body,1,bytes,out);
    free(body);free(scope);free(query);free(job);return error?fail(out,error):0;
}
