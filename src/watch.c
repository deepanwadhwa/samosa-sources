#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 2048
#include "watch.h"
#include "inventory.h"
#include "control.h"
#include "json.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t stopping;
static void stop_signal(int signal_number){(void)signal_number;stopping=1;}
static int stop_requested(void *context){const SourcesConfig *parent=context;return stopping||(parent&&parent->cancelled&&parent->cancelled(parent->cancellation));}
static int regular_private(int fd){struct stat s;return fd>=0&&!fstat(fd,&s)&&S_ISREG(s.st_mode)&&s.st_uid==geteuid()&&!(s.st_mode&077)&&s.st_nlink==1;}
static int lease_name(const char *id,char name[96]){
    if(!id||strlen(id)!=64)return 0;
    for(size_t i=0;i<64;i++)if(!strchr("0123456789abcdef",id[i]))return 0;
    return snprintf(name,96,"watch-%s.lock",id)>0;
}
const char *sources_watch_monitor(int directory,const char *id){
    char name[96];if(directory<0||!lease_name(id,name))return "unknown";
    int fd=openat(directory,name,O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?"not_running":"unknown";
    const char *status="unknown";
    if(regular_private(fd)){
        if(!flock(fd,LOCK_EX|LOCK_NB)){status="not_running";flock(fd,LOCK_UN);}
        else if(errno==EWOULDBLOCK||errno==EAGAIN)status="running";
    }
    close(fd);return status;
}
const char *sources_watch_lock(int directory,const char *id,int *lease){
    char name[96];*lease=-1;if(!lease_name(id,name))return "invalid_request";
    int fd=openat(directory,name,O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0)return errno==ENOENT?NULL:"state_unavailable";
    if(!regular_private(fd)){close(fd);return "state_unavailable";}
    if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);return "watch_busy";}
    *lease=fd;return NULL;
}
const char *sources_watch_create_lease(int directory,const char *id,int *lease){
    char name[96];*lease=-1;if(!lease_name(id,name))return "invalid_request";
    int fd=openat(directory,name,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(!regular_private(fd)){if(fd>=0)close(fd);return "state_unavailable";}
    if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);return "watch_busy";}
    *lease=fd;return NULL;
}
int sources_watch_unlink(int directory,const char *id,int lease){
    char name[96];struct stat held,named;
    if(lease<0)return 1;
    return lease_name(id,name)&&!fstat(lease,&held)&&!fstatat(directory,name,&named,AT_SYMLINK_NOFOLLOW)&&held.st_dev==named.st_dev&&held.st_ino==named.st_ino&&!unlinkat(directory,name,0);
}
static char *dispatch(const SourcesConfig *cfg,const char *request,int *rc){
    char *body=NULL;size_t length;FILE *out=open_memstream(&body,&length);
    if(!out){*rc=1;return NULL;}
    *rc=sources_dispatch(cfg,request,strlen(request),out);
    if(fclose(out)){free(body);body=NULL;*rc=1;}return body;
}
static double monotonic(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int failure(FILE *out,const char *code){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",code);return 1;}
int sources_watch_run(const SourcesConfig *cfg,const char *id,size_t interval,FILE *out){
    char name[96],request[256];if(!cfg||!cfg->state||!lease_name(id,name)||interval<100||interval>60000)return failure(out,"invalid_request");
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_watch_get\",\"watch_id\":\"%s\"}",id);
    int rc;char *body=dispatch(cfg,request,&rc);
    if(rc){if(body)fputs(body,out);else failure(out,"resource_limit");free(body);return 1;}Json admitted;char *admission_state=NULL;
    if(body&&json_parse(&admitted,body,strlen(body)))admission_state=json_string(&admitted,json_get(&admitted,0,"state"));
    int already_stopped=admission_state&&!strcmp(admission_state,"stopped");free(admission_state);free(body);
    if(already_stopped){fputs("{\"contract_version\":1,\"watch_runtime\":\"stopped\"}\n",out);return 0;}
    int directory=-1,lease=-1;struct stat state;
    double admission_started=monotonic();if(admission_started<0)return failure(out,"state_unavailable");
    const char *admission;
    do {
        admission=sources_watch_admit(cfg,id,&directory,&lease);
        if(!admission||(strcmp(admission,"busy")&&strcmp(admission,"store_busy")))break;
        double current=monotonic();if(current<0||current-admission_started>=5)break;
        struct timespec delay={0,20000000};nanosleep(&delay,NULL);
    }while(1);
    if(admission)return failure(out,admission);
    if(fstat(directory,&state)){close(lease);close(directory);return failure(out,"state_unavailable");}
    SourcesConfig pinned=*cfg;pinned.state_pinned=1;pinned.state_device=(size_t)state.st_dev;pinned.state_inode=(size_t)state.st_ino;cfg=&pinned;
    struct sigaction action={0},old_term,old_int;action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);stopping=0;
    if(sigaction(SIGTERM,&action,&old_term)){close(lease);close(directory);return failure(out,"state_unavailable");}
    if(sigaction(SIGINT,&action,&old_int)){sigaction(SIGTERM,&old_term,NULL);close(lease);close(directory);return failure(out,"state_unavailable");}
    int result=0,cleanup_removed=0;
    while(!stopping){
        struct stat held,named;
        if(fstat(lease,&held)||fstatat(directory,name,&named,AT_SYMLINK_NOFOLLOW)||held.st_dev!=named.st_dev||held.st_ino!=named.st_ino){result=failure(out,"state_unavailable");break;}
        int current_state=open(cfg->state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int bound=current_state>=0&&!fstat(current_state,&named)&&named.st_dev==state.st_dev&&named.st_ino==state.st_ino;
        if(current_state>=0)close(current_state);
        if(!bound){result=failure(out,"state_unavailable");break;}
        body=dispatch(cfg,request,&rc);Json j;size_t cycle=0;
        if(rc||!body||!json_parse(&j,body,strlen(body))){
            if(body&&json_parse(&j,body,strlen(body))){char *code=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));cleanup_removed=code&&!strcmp(code,"unknown_watch");free(code);}
            if(body)fputs(body,out);else failure(out,"resource_limit");free(body);result=1;break;}
        char *status=json_string(&j,json_get(&j,0,"state"));int checkpoint=json_get(&j,0,"checkpoint");
        if(!status){free(body);result=failure(out,"invalid_response");break;}
        int stopped=!strcmp(status,"stopped");free(status);
        if(checkpoint>=0&&j.tokens[checkpoint].type!=JT_NULL&&!json_uint(&j,json_get(&j,checkpoint,"cycle"),&cycle)){free(body);result=failure(out,"invalid_response");break;}
        free(body);if(stopped)break;
        char step[256];snprintf(step,sizeof(step),"{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":\"%s\",\"expected_cycle\":%zu}",id,cycle);
        SourcesConfig work=*cfg;double current=monotonic();if(current<0){result=failure(out,"state_unavailable");break;}
        double deadline=current+5;if(!work.deadline_monotonic||deadline<work.deadline_monotonic)work.deadline_monotonic=deadline;
        work.cancelled=stop_requested;work.cancellation=(void*)cfg;
        body=dispatch(&work,step,&rc);
        if(rc&&!stopping){
            char *code=NULL;if(body&&json_parse(&j,body,strlen(body)))code=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));
            int transient=code&&(!strcmp(code,"busy")||!strcmp(code,"store_busy")||!strcmp(code,"watch_cycle_conflict")||!strcmp(code,"source_changed")||!strcmp(code,"deadline_exceeded"));
            int terminal_stop=code&&!strcmp(code,"watch_stopped");
            const char *health=sources_watch_record(cfg,id,code?code:"invalid_response");free(code);
            if(health){free(body);result=failure(out,health);break;}
            if(!transient){if(!terminal_stop){if(body)fputs(body,out);else failure(out,"resource_limit");result=1;}free(body);break;}
        }
        if(!rc&&!stopping){const char *health=sources_watch_record(cfg,id,NULL);if(health){free(body);result=failure(out,health);break;}}
        free(body);
        struct timespec delay={(time_t)(interval/1000),(long)(interval%1000)*1000000};
        while(!stopping&&nanosleep(&delay,&delay)&&errno==EINTR){}
    }
    sigaction(SIGTERM,&old_term,NULL);sigaction(SIGINT,&old_int,NULL);if(cleanup_removed)sources_watch_unlink(directory,id,lease);close(lease);close(directory);
    if(!result)fputs("{\"contract_version\":1,\"watch_runtime\":\"stopped\"}\n",out);
    return result;
}

const char *sources_queue_create_lease(int directory,int *lease){
    *lease=-1;int fd=openat(directory,"reindex-runtime.lock",O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    if(!regular_private(fd)){if(fd>=0)close(fd);return "state_unavailable";}
    if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);return "queue_busy";}
    *lease=fd;return NULL;
}
static int queue_retry(const char *code){
    return code&&(!strcmp(code,"busy")||!strcmp(code,"store_busy")||!strcmp(code,"stale_source")||!strcmp(code,"stale_observation")||!strcmp(code,"live_unavailable")||!strcmp(code,"source_changed")||!strcmp(code,"read_failed")||!strcmp(code,"deadline_exceeded"));
}
int sources_queue_run(const SourcesConfig *cfg,size_t interval,FILE *out){
    if(!cfg||!cfg->root||!cfg->state||interval<100||interval>60000)return failure(out,"invalid_request");
    if(cfg->discovery_interval_ms&&(cfg->discovery_interval_ms<100||cfg->discovery_interval_ms>60000))return failure(out,"invalid_configuration");
    char scope[65];const char *error=inventory_scope_identity(cfg,"",scope);if(error)return failure(out,error);
    int directory=-1,lease=-1;error=sources_queue_admit(cfg,&directory,&lease);if(error)return failure(out,error);
    struct stat state;if(fstat(directory,&state)){close(lease);close(directory);return failure(out,"state_unavailable");}
    SourcesConfig pinned=*cfg;pinned.root_scope_pin=scope;pinned.state_pinned=1;pinned.state_device=(size_t)state.st_dev;pinned.state_inode=(size_t)state.st_ino;cfg=&pinned;
    struct sigaction action={0},old_term,old_int;action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);stopping=0;
    if(sigaction(SIGTERM,&action,&old_term)){close(lease);close(directory);return failure(out,"state_unavailable");}
    if(sigaction(SIGINT,&action,&old_int)){sigaction(SIGTERM,&old_term,NULL);close(lease);close(directory);return failure(out,"state_unavailable");}
    int result=0;
    while(!stop_requested((void*)cfg)){
        struct stat held,named;int current=open(cfg->state,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        int bound=current>=0&&!fstat(current,&named)&&named.st_dev==state.st_dev&&named.st_ino==state.st_ino;if(current>=0)close(current);
        if(!bound||fstat(lease,&held)||fstatat(directory,"reindex-runtime.lock",&named,AT_SYMLINK_NOFOLLOW)||held.st_dev!=named.st_dev||held.st_ino!=named.st_ino){result=failure(out,"state_unavailable");break;}
        char actual[65];error=inventory_scope_identity(cfg,"",actual);if(!error&&strcmp(actual,scope))error="scope_unavailable";
        if(!error)error=sources_interrupted(cfg);
        if(error){result=failure(out,error);break;}
        SourcesConfig work=*cfg;double started=monotonic();if(started<0){result=failure(out,"state_unavailable");break;}
        work.cancelled=stop_requested;work.cancellation=(void*)cfg;
        if(!work.deadline_monotonic||started+5<work.deadline_monotonic)work.deadline_monotonic=started+5;
        if(cfg->discovery_interval_ms){
            int refresh_rc;char *refresh_body=dispatch(&work,"{\"contract_version\":1,\"op\":\"index_refresh_step\"}",&refresh_rc);
            Json response;char *refresh_error=NULL;size_t scans=0,items=0,recovered=0,observed=0;
            int valid_refresh=refresh_body&&json_parse(&response,refresh_body,strlen(refresh_body));
            if(valid_refresh&&refresh_rc)refresh_error=json_string(&response,json_get(&response,json_get(&response,0,"error"),"code"));
            if(!valid_refresh||(!refresh_rc&&(!json_uint(&response,json_get(&response,0,"scanned_jobs"),&scans)||scans>1||!json_uint(&response,json_get(&response,0,"processed_items"),&items)||items>1||!json_uint(&response,json_get(&response,0,"recovered_items"),&recovered)||recovered>1||!json_uint(&response,json_get(&response,0,"observed_items"),&observed)||observed>1))){
                free(refresh_body);free(refresh_error);result=failure(out,"invalid_response");break;
            }
            if(refresh_rc&&!stop_requested((void*)cfg)&&!queue_retry(refresh_error)){
                if(refresh_body)fputs(refresh_body,out);else failure(out,"resource_limit");
                free(refresh_error);free(refresh_body);result=1;break;
            }
            free(refresh_error);free(refresh_body);
            if(stop_requested((void*)cfg))break;
        }
        int rc;char *body=dispatch(&work,"{\"contract_version\":1,\"op\":\"index_queue_step\"}",&rc);
        Json j;char *code=NULL;int valid=body&&json_parse(&j,body,strlen(body));
        if(valid&&rc)code=json_string(&j,json_get(&j,json_get(&j,0,"error"),"code"));
        if(!valid||(!rc&&json_get(&j,0,"reindexed_items")<0)){free(body);free(code);result=failure(out,"invalid_response");break;}
        if(rc&&!stop_requested((void*)cfg)&&!queue_retry(code)){if(body)fputs(body,out);else failure(out,"resource_limit");free(code);free(body);result=1;break;}
        free(code);free(body);
        if(cfg->chutni_publish&&!stop_requested((void*)cfg)){
            SourcesConfig publication=*cfg;double began=monotonic();if(began<0){result=failure(out,"state_unavailable");break;}
            publication.cancelled=stop_requested;publication.cancellation=(void*)cfg;
            if(!publication.deadline_monotonic||began+5<publication.deadline_monotonic)publication.deadline_monotonic=began+5;
            int publication_rc;char *publication_body=dispatch(&publication,"{\"contract_version\":1,\"op\":\"index_publish_queue_step\"}",&publication_rc);
            Json response;char *publication_error=NULL;size_t published=0;int valid_publication=publication_body&&json_parse(&response,publication_body,strlen(publication_body));
            if(valid_publication&&publication_rc)publication_error=json_string(&response,json_get(&response,json_get(&response,0,"error"),"code"));
            if(!valid_publication||(!publication_rc&&(!json_uint(&response,json_get(&response,0,"published_items"),&published)||published>1))){free(publication_error);free(publication_body);result=failure(out,"invalid_response");break;}
            int retry=queue_retry(publication_error)||(publication_error&&(!strncmp(publication_error,"chutni_",7)||!strcmp(publication_error,"native_helper_failed")||!strcmp(publication_error,"native_helper_unavailable")));
            if(publication_rc&&!stop_requested((void*)cfg)&&!retry){fputs(publication_body,out);free(publication_error);free(publication_body);result=1;break;}
            free(publication_error);free(publication_body);
        }
        if(cfg->inference&&!stop_requested((void*)cfg)){
            SourcesConfig audio=*cfg;double began=monotonic();if(began<0){result=failure(out,"state_unavailable");break;}
            audio.cancelled=stop_requested;audio.cancellation=(void*)cfg;
            if(!audio.deadline_monotonic||began+60<audio.deadline_monotonic)audio.deadline_monotonic=began+60;
            int audio_rc;char *audio_body=dispatch(&audio,"{\"contract_version\":1,\"op\":\"audio_index_queue_step\"}",&audio_rc);Json response;size_t processed=0;char *audio_error=NULL;
            int valid_audio=audio_body&&json_parse(&response,audio_body,strlen(audio_body));if(valid_audio&&audio_rc)audio_error=json_string(&response,json_get(&response,json_get(&response,0,"error"),"code"));
            if(!valid_audio||(!audio_rc&&(!json_uint(&response,json_get(&response,0,"processed_windows"),&processed)||processed>1))){free(audio_body);free(audio_error);result=failure(out,"invalid_response");break;}
            int retry=queue_retry(audio_error)||(audio_error&&(!strncmp(audio_error,"inference_",10)||!strncmp(audio_error,"chutni_",7)||!strcmp(audio_error,"native_helper_failed")||!strcmp(audio_error,"native_helper_unavailable")||!strcmp(audio_error,"audio_index_configuration_changed")));
            if(audio_rc&&!stop_requested((void*)cfg)&&!retry){fputs(audio_body,out);free(audio_body);free(audio_error);result=1;break;}free(audio_body);free(audio_error);
        }
        if(cfg->visual_inference&&!stop_requested((void*)cfg)){
            SourcesConfig visual=*cfg;double began=monotonic();if(began<0){result=failure(out,"state_unavailable");break;}
            visual.cancelled=stop_requested;visual.cancellation=(void*)cfg;
            if(!visual.deadline_monotonic||began+60<visual.deadline_monotonic)visual.deadline_monotonic=began+60;
            int visual_rc;char *visual_body=dispatch(&visual,"{\"contract_version\":1,\"op\":\"visual_index_queue_step\"}",&visual_rc);Json response;size_t processed=0;char *visual_error=NULL;
            int valid_visual=visual_body&&json_parse(&response,visual_body,strlen(visual_body));if(valid_visual&&visual_rc)visual_error=json_string(&response,json_get(&response,json_get(&response,0,"error"),"code"));
            if(!valid_visual||(!visual_rc&&(!json_uint(&response,json_get(&response,0,"processed_windows"),&processed)||processed>1))){free(visual_body);free(visual_error);result=failure(out,"invalid_response");break;}
            int retry=queue_retry(visual_error)||(visual_error&&(!strncmp(visual_error,"inference_",10)||!strncmp(visual_error,"chutni_",7)||!strcmp(visual_error,"native_helper_failed")||!strcmp(visual_error,"native_helper_unavailable")||!strcmp(visual_error,"visual_index_configuration_changed")));
            if(visual_rc&&!stop_requested((void*)cfg)&&!retry){fputs(visual_body,out);free(visual_body);free(visual_error);result=1;break;}free(visual_body);free(visual_error);
        }
        if(cfg->summary_inference&&!stop_requested((void*)cfg)){
            SourcesConfig enrichment=*cfg;double began=monotonic();if(began<0){result=failure(out,"state_unavailable");break;}
            enrichment.cancelled=stop_requested;enrichment.cancellation=(void*)cfg;
            if(!enrichment.deadline_monotonic||began+60<enrichment.deadline_monotonic)enrichment.deadline_monotonic=began+60;
            int enrichment_rc;char *enrichment_body=dispatch(&enrichment,"{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}",&enrichment_rc);
            Json response;size_t processed=0;char *enrichment_error=NULL;
            int valid_enrichment=enrichment_body&&json_parse(&response,enrichment_body,strlen(enrichment_body));
            if(valid_enrichment&&enrichment_rc)enrichment_error=json_string(&response,json_get(&response,json_get(&response,0,"error"),"code"));
            if(!valid_enrichment||(!enrichment_rc&&(!json_uint(&response,json_get(&response,0,"processed_items"),&processed)||processed>1))){free(enrichment_body);free(enrichment_error);result=failure(out,"invalid_response");break;}
            int retry=queue_retry(enrichment_error)||(enrichment_error&&(!strncmp(enrichment_error,"inference_",10)||!strncmp(enrichment_error,"chutni_",7)||!strcmp(enrichment_error,"native_helper_failed")||!strcmp(enrichment_error,"native_helper_unavailable")));
            if(enrichment_rc&&!stop_requested((void*)cfg)&&!retry){fputs(enrichment_body,out);free(enrichment_body);free(enrichment_error);result=1;break;}
            free(enrichment_body);free(enrichment_error);
        }
        if(cfg->embedding&&cfg->chutni_publish&&!stop_requested((void*)cfg)){
            SourcesConfig embedding=*cfg;double began=monotonic();if(began<0){result=failure(out,"state_unavailable");break;}
            embedding.cancelled=stop_requested;embedding.cancellation=(void*)cfg;
            if(!embedding.deadline_monotonic||began+60<embedding.deadline_monotonic)embedding.deadline_monotonic=began+60;
            int embedding_rc;char *embedding_body=dispatch(&embedding,"{\"contract_version\":1,\"op\":\"index_embed_queue_step\"}",&embedding_rc);
            Json response;size_t processed=0;char *embedding_error=NULL;
            int valid_embedding=embedding_body&&json_parse(&response,embedding_body,strlen(embedding_body));
            if(valid_embedding&&embedding_rc)embedding_error=json_string(&response,json_get(&response,json_get(&response,0,"error"),"code"));
            if(!valid_embedding||(!embedding_rc&&(!json_uint(&response,json_get(&response,0,"processed_items"),&processed)||processed>1))){free(embedding_body);free(embedding_error);result=failure(out,"invalid_response");break;}
            int retry=queue_retry(embedding_error)||(embedding_error&&(!strncmp(embedding_error,"inference_",10)||!strncmp(embedding_error,"chutni_",7)||!strncmp(embedding_error,"embedding_",10)||!strcmp(embedding_error,"native_helper_failed")||!strcmp(embedding_error,"native_helper_unavailable")));
            if(embedding_rc&&!stop_requested((void*)cfg)&&!retry){fputs(embedding_body,out);free(embedding_body);free(embedding_error);result=1;break;}
            free(embedding_body);free(embedding_error);
        }
        /* Bounded pacing applies after successful, idle and failed attempts. */
        double wait_started=monotonic();if(wait_started<0){result=failure(out,"state_unavailable");break;}
        double until=wait_started+(double)interval/1000;
        while(!stop_requested((void*)cfg)){
            error=sources_interrupted(cfg);if(error){result=failure(out,error);break;}
            double now=monotonic();if(now<0){result=failure(out,"state_unavailable");break;}if(now>=until)break;
            double left=until-now;long nanos=left>.1?100000000L:(long)(left*1e9);struct timespec delay={0,nanos};nanosleep(&delay,NULL);
        }
        if(result)break;
    }
    sigaction(SIGTERM,&old_term,NULL);sigaction(SIGINT,&old_int,NULL);close(lease);close(directory);
    if(!result)fputs("{\"contract_version\":1,\"index_runtime\":\"stopped\"}\n",out);
    return result;
}
