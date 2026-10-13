#include "visual_fixture.h"
#include "visual_index.h"
#include "index.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
static sqlite3_int64 scalar(sqlite3 *db,const char *query){sqlite3_stmt *s=NULL;CHECK(sqlite3_prepare_v2(db,query,-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_ROW);sqlite3_int64 value=sqlite3_column_int64(s,0);CHECK(sqlite3_finalize(s)==SQLITE_OK);return value;}
static void sql(sqlite3 *db,const char *query){CHECK(sqlite3_exec(db,query,NULL,NULL,NULL)==SQLITE_OK);}
static char *index_request(const char *op,const char *job,const char *operation){char *body=NULL;size_t n;FILE *f=open_memstream(&body,&n);CHECK(f);fprintf(f,"{\"contract_version\":1,\"op\":\"%s\",\"job_id\":\"%s\"",op,job);if(operation)fprintf(f,",\"operation_id\":\"%s\",\"max_items\":1",operation);fputc('}',f);CHECK(!fclose(f));return body;}
static char *linked(sqlite3 *db){sqlite3_stmt *s=NULL;CHECK(sqlite3_prepare_v2(db,"SELECT visual_job FROM index_visual LIMIT 1",-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_ROW);char *id=strdup((const char*)sqlite3_column_text(s,0));CHECK(id);CHECK(sqlite3_finalize(s)==SQLITE_OK);return id;}
int main(int argc,char **argv){
    if(argc>1&&!strcmp(argv[1],"--sources-native-worker"))return sources_native_worker_main(argc,argv);
    CHECK(argc==3);char temp[]="/tmp/samosa-index-visual-XXXXXX";CHECK(mkdtemp(temp));char *base=realpath(temp,NULL);CHECK(base);
    char root[512],state[512],path[600],database[600];snprintf(root,sizeof(root),"%s/files",base);snprintf(state,sizeof(state),"%s/state",base);CHECK(!mkdir(root,0700));snprintf(path,sizeof(path),"%s/video.mp4",root);snprintf(database,sizeof(database),"%s/sources.sqlite3",state);
    FILE *from=fopen(argv[2],"rb"),*to=fopen(path,"wb");CHECK(from&&to);unsigned char buffer[8192];size_t n;while((n=fread(buffer,1,sizeof(buffer),from)))CHECK(fwrite(buffer,1,n,to)==n);CHECK(!ferror(from));CHECK(!fclose(from)&&!fclose(to));
    char engine[65],model[65];memset(engine,'a',64);engine[64]=0;memset(model,'b',64);model[64]=0;
    Fixture fixture={.mode="valid"};SourcesInferenceConfig inference={.provider_id="fixture-visionpsy",.engine_sha256=engine,.model_sha256=model,.call=transport,.context=&fixture};
    SourcesConfig cfg={.root=root,.state=state,.owner="alice",.media_helper=argv[1],.visual_inference=&inference};fixture.inference=&inference;fixture.sources=&cfg;
    char *body=call(&cfg,"{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"video.mp4\"}",NULL),*source=field(body,"source_id");free(body);
    sqlite3 *db=NULL;CHECK(sqlite3_open(database,&db)==SQLITE_OK);int directory=open(state,O_RDONLY|O_DIRECTORY),lease=-1;CHECK(directory>=0&&!source_index_lease(directory,&lease));char id[65],failure[65],rolled[65];
    sql(db,"BEGIN IMMEDIATE");CHECK(!sources_visual_index_admit(&cfg,db,source,2000,id,failure));memcpy(rolled,id,65);CHECK(!sqlite3_get_autocommit(db));CHECK(scalar(db,"SELECT count(*) FROM visual_index_jobs")==1);sql(db,"ROLLBACK");CHECK(scalar(db,"SELECT count(*) FROM sqlite_master WHERE name='visual_index_jobs'")==0);close(lease);close(directory);CHECK(sqlite3_close(db)==SQLITE_OK);
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"discover-visual\"}",NULL);char *job=field(body,"job_id");free(body);
    char *req=index_request("index_step",job,"queue-visual");SourcesConfig unavailable=cfg;unavailable.visual_inference=NULL;
    body=call(&unavailable,req,"inference_unconfigured");free(body);CHECK(sqlite3_open(database,&db)==SQLITE_OK);CHECK(scalar(db,"SELECT count(*) FROM index_visual")==0);CHECK(scalar(db,"SELECT count(*) FROM index_items WHERE status='pending'")==1);CHECK(sqlite3_close(db)==SQLITE_OK);free(req);
    req=index_request("index_step",job,"queue-configured");body=call(&cfg,req,NULL);CHECK(strstr(body,"waiting_visual")&&strstr(body,"\"queued_visual_items\":1")&&strstr(body,"\"indexed_items\":0"));char *saved=strdup(body);CHECK(saved);free(body);
    SourcesConfig offline=cfg;offline.root=NULL;offline.visual_inference=NULL;body=call(&offline,req,NULL);CHECK(!strcmp(body,saved));free(body);free(saved);free(req);
    CHECK(sqlite3_open(database,&db)==SQLITE_OK);char *old=linked(db);CHECK(!strcmp(old,rolled));CHECK(scalar(db,"SELECT count(*) FROM index_visual a JOIN visual_index_jobs q ON a.visual_job=q.id AND a.source=q.source")==1);CHECK(sqlite3_close(db)==SQLITE_OK);
    req=index_request("visual_index_step",old,NULL);body=call(&cfg,req,NULL);CHECK(strstr(body,"captured")&&fixture.calls==1);free(body);free(req);inference.call=NULL;
    req=index_request("index_get",job,NULL);body=call(&cfg,req,NULL);CHECK(strstr(body,"\"completed_visual_items\":1")&&strstr(body,"\"status\":\"completed\"")&&strstr(body,"\"content_coverage\":\"partial\""));free(body);free(req);
    /* Valid trailing container bytes change the immutable version while keeping
     * the media decodable. Replacement must preserve the original derived job. */
    to=fopen(path,"ab");CHECK(to&&fputc(0,to)!=EOF&&!fclose(to));cfg.discovery_interval_ms=100;
    body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",NULL);CHECK(strstr(body,"\"observed_items\":1")&&strstr(body,"\"freshness_error\":null"));free(body);cfg.discovery_interval_ms=0;
    req=index_request("index_pending",job,NULL);body=call(&offline,req,NULL);CHECK(strstr(body,"\"pending_items\":1")&&strstr(body,source));free(body);free(req);
    req=index_request("index_reindex_step",job,"replace-unconfigured");body=call(&unavailable,req,"inference_unconfigured");free(body);free(req);CHECK(sqlite3_open(database,&db)==SQLITE_OK);char *current=linked(db);CHECK(!strcmp(current,old));free(current);CHECK(scalar(db,"SELECT count(*) FROM index_visual_history")==0);CHECK(sqlite3_close(db)==SQLITE_OK);
    /* Death after durable queue admission must roll back association/history and
 * leave the last good capture readable; the next operation recovers the lease. */
    char marker[600];snprintf(marker,sizeof(marker),"%s/admitted",base);CHECK(!setenv("SAMOSA_VISUAL_REINDEX_TEST_ADMITTED_MARKER",marker,1));
    req=index_request("index_reindex_step",job,"replace-killed");pid_t child=fork();CHECK(child>=0);
    if(!child){char *result=call(&cfg,req,NULL);free(result);_exit(2);}
    CHECK(!unsetenv("SAMOSA_VISUAL_REINDEX_TEST_ADMITTED_MARKER"));int ready=0;
    for(size_t i=0;i<1200;i++){if(!access(marker,F_OK)){ready=1;break;}int exited;pid_t ended=waitpid(child,&exited,WNOHANG);CHECK(ended>=0);if(ended==child){fprintf(stderr,"replacement exited before admission: %d\n",exited);exit(1);}struct timespec pause={0,100000000};nanosleep(&pause,NULL);}if(!ready){CHECK(!kill(child,SIGKILL));int exited;CHECK(waitpid(child,&exited,0)==child);}CHECK(ready);CHECK(!kill(child,SIGKILL));int status;CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);free(req);
    CHECK(sqlite3_open(database,&db)==SQLITE_OK);current=linked(db);CHECK(!strcmp(current,old));free(current);CHECK(scalar(db,"SELECT count(*) FROM visual_index_jobs")==1);CHECK(scalar(db,"SELECT count(*) FROM index_visual_history")==0);CHECK(scalar(db,"SELECT count(*) FROM index_pending")==1);CHECK(sqlite3_close(db)==SQLITE_OK);

req=index_request("index_reindex_step",job,"replace-visual");body=call(&cfg,req,NULL);CHECK(strstr(body,"\"reindexed_items\":1"));free(body);free(req);CHECK(sqlite3_open(database,&db)==SQLITE_OK);current=linked(db);CHECK(strcmp(current,old));CHECK(scalar(db,"SELECT count(*) FROM index_visual_history")==1);CHECK(sqlite3_close(db)==SQLITE_OK);
    /* Later discovery and automatic initial processing reuse the same admission
 * path, including deduplication against the already captured immutable job. */
    char added[600],discover[600];snprintf(added,sizeof(added),"%s/new.mp4",root);
    from=fopen(argv[2],"rb");to=fopen(added,"wb");CHECK(from&&to);while((n=fread(buffer,1,sizeof(buffer),from)))CHECK(fwrite(buffer,1,n,to)==n);CHECK(!ferror(from)&&!fclose(from)&&!fclose(to));
    snprintf(discover,sizeof(discover),"{\"contract_version\":1,\"op\":\"index_discover\",\"job_id\":\"%s\",\"operation_id\":\"later-visual\"}",job);body=call(&cfg,discover,NULL);CHECK(strstr(body,"\"total_items\":2"));free(body);
    cfg.discovery_interval_ms=100;body=call(&cfg,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",NULL);CHECK(strstr(body,"\"processed_items\":1")&&strstr(body,"\"initial_error\":null"));free(body);cfg.discovery_interval_ms=0;
    CHECK(sqlite3_open(database,&db)==SQLITE_OK);CHECK(scalar(db,"SELECT count(*) FROM index_visual")==2);CHECK(scalar(db,"SELECT count(*) FROM visual_index_jobs")==2);CHECK(sqlite3_close(db)==SQLITE_OK);

req=index_request("index_visual_history",job,NULL);body=call(&cfg,req,NULL);CHECK(strstr(body,"retained_derived_visual")&&strstr(body,old)&&strstr(body,"\"returned_artifacts\":1"));free(body);SourcesConfig foreign=cfg;foreign.owner="bob";body=call(&foreign,req,"unknown_index");free(body);free(req);
    char read[600];snprintf(read,sizeof(read),"{\"contract_version\":1,\"op\":\"index_visual_read\",\"job_id\":\"%s\",\"visual_job_id\":\"%s\",\"ordinal\":0}",job,old);CHECK(!unlink(path));body=call(&cfg,read,NULL);CHECK(strstr(body,"A colored frame.")&&strstr(body,"selected_frame_only")&&!strstr(body,"\"kind\":\"evidence\""));free(body);body=call(&foreign,read,"unknown_index_artifact");free(body);
    snprintf(read,sizeof(read),"{\"contract_version\":1,\"op\":\"index_visual_search\",\"job_id\":\"%s\",\"query\":\"COLORED\"}",job);body=call(&cfg,read,NULL);CHECK(strstr(body,old)&&strstr(body,"\"returned_candidates\":1")&&strstr(body,"derived_preview")&&strstr(body,"\"uncaptured_windows\":1"));free(body);CHECK(fixture.calls==1);
    CHECK(sqlite3_open(database,&db)==SQLITE_OK);sql(db,"UPDATE visual_index_windows SET digest=printf('%064d',0)");CHECK(sqlite3_close(db)==SQLITE_OK);body=call(&cfg,read,"corrupt_visual_index");free(body);
    free(source);free(job);free(old);free(current);free(base);printf("PASS visual discovery/reindex/history/search: %u checks; atomic rollback, missing model retry, immutable replacement, offline retained uncertain captions, owner and corruption refusal; fixture model only\n",checks);return 0;
}
