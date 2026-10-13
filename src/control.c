#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 8192
#include "control.h"
#include "semantic.h"
#include "embedding.h"
#include "operations.h"
#include "json.h"
#include "sha256.h"
#include <math.h>
#include <time.h>
#include <sys/stat.h>
static double now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
static int fail_refs(FILE *out,const char *code,const char *id,const char *parent){fputs("{\"contract_version\":1,\"error\":{\"code\":",out);json_quote(out,code,strlen(code));fputc('}',out);if(id){fputs(",\"operation_id\":",out);json_quote(out,id,strlen(id));if(parent){fputs(",\"parent_operation_id\":",out);json_quote(out,parent,strlen(parent));}}fputs("}\n",out);return 1;}
const char *sources_interrupted(const SourcesConfig *cfg){
    if(!cfg)return NULL;
    if(!isfinite(cfg->deadline_monotonic)||cfg->deadline_monotonic<0)return "invalid_configuration";
    if(cfg->cancelled&&cfg->cancelled(cfg->cancellation))return "cancelled";
    if(cfg->deadline_monotonic){double current=now();if(current<0)return "state_unavailable";if(current>=cfg->deadline_monotonic)return "deadline_exceeded";}
    return NULL;
}
static int identity(const char *id){if(!id||!*id||strlen(id)>128)return 0;for(const unsigned char *p=(const unsigned char*)id;*p;p++)if(*p>127||(!isalnum(*p)&&!strchr("-_:.",*p)))return 0;return 1;}
/* Strict fields have one normalized value independent of key order or escapes. */
static char *canonical(const Json *j,const char *fields){
    struct Field{char *name;int value;} keys[16]={{0}};size_t count=0;char *body=NULL;size_t length;FILE *out=NULL;
    for(int t=1;t<j->tokens[0].next;t=j->tokens[j->tokens[t].next].next){if(count==16)goto done;char *key=json_string(j,t),match[128];int n=snprintf(match,sizeof(match),"|%s|",key?key:"");if(!key||strchr(key,'|')||n<0||(size_t)n>=sizeof(match)||!strstr(fields,match)){free(key);goto done;}keys[count++]=(struct Field){key,j->tokens[t].next};}
    for(size_t i=0;i<count;i++)for(size_t k=i+1;k<count;k++)if(strcmp(keys[i].name,keys[k].name)>0){struct Field tmp=keys[i];keys[i]=keys[k];keys[k]=tmp;}
    out=open_memstream(&body,&length);if(!out)goto done;fputc('{',out);
    for(size_t i=0;i<count;i++){if(i)fputc(',',out);json_quote(out,keys[i].name,strlen(keys[i].name));fputc(':',out);int t=keys[i].value;
        if(!strcmp(keys[i].name,"vector")){char *value=NULL;if(sources_semantic_vector_canonical(j,t,&value))goto invalid;fputs(value,out);free(value);continue;}
        if(!strcmp(keys[i].name,"read")){
            if(j->tokens[t].type!=JT_OBJECT)goto invalid;
            Json nested;
            if(!json_parse(&nested,j->text+j->tokens[t].begin,j->tokens[t].end-j->tokens[t].begin))goto invalid;
            char *value=canonical(&nested,"|contract_version|op|source_id|version|start_byte|end_byte|start_page|end_page|start_ms|end_ms|");
            if(!value)goto invalid;
            fputs(value,out);free(value);continue;
        }
        int string_field=!strcmp(keys[i].name,"evidence_id")||!strcmp(keys[i].name,"prompt")||!strcmp(keys[i].name,"store_id")||!strcmp(keys[i].name,"external_source_id")||!strcmp(keys[i].name,"external_artifact_id")||!strcmp(keys[i].name,"job_id")||!strcmp(keys[i].name,"query")||!strcmp(keys[i].name,"acquisition_id")||!strcmp(keys[i].name,"url")||!strcmp(keys[i].name,"format")||!strcmp(keys[i].name,"upload_id")||!strcmp(keys[i].name,"data")||!strcmp(keys[i].name,"op")||!strcmp(keys[i].name,"path")||!strcmp(keys[i].name,"watch_id")||!strcmp(keys[i].name,"source_id")||!strcmp(keys[i].name,"version")||!strcmp(keys[i].name,"expected_file_identity")||!strcmp(keys[i].name,"operation_id")||!strcmp(keys[i].name,"parent_operation_id");
        if(string_field!=(j->tokens[t].type==JT_STRING))goto invalid;
        if(j->tokens[t].type==JT_STRING){char *value=json_string(j,t);if(!value)goto invalid;json_quote(out,value,strlen(value));free(value);}
        else{size_t value;if(!json_uint(j,t,&value))goto invalid;fprintf(out,"%zu",value);}}
    fputc('}',out);if(fclose(out)){out=NULL;free(body);body=NULL;}else out=NULL;goto done;
invalid: fclose(out);out=NULL;free(body);body=NULL;
done: if(out)fclose(out);for(size_t i=0;i<count;i++)free(keys[i].name);return body;
}
int sources_dispatch(const SourcesConfig *cfg,const char *input,size_t length,FILE *out){
    double received=now();Json j;if(length>SOURCES_REQUEST_MAX||!json_parse(&j,input,length)||j.tokens[0].type!=JT_OBJECT)return sources_dispatch_core(cfg,input,length,out);
    size_t contract;if(!json_uint(&j,json_get(&j,0,"contract_version"),&contract)||contract!=1)return sources_dispatch_core(cfg,input,length,out);
    char *op=json_string(&j,json_get(&j,0,"op"));int public=op&&(!strcmp(op,"operation_get")||!strcmp(op,"operation_cancel"));
    if(public){char *normal=canonical(&j,"|contract_version|op|operation_id|"),*id=json_string(&j,json_get(&j,0,"operation_id"));int rc;if(!normal||!identity(id))rc=fail_refs(out,"invalid_request",NULL,NULL);else rc=source_operation_public(cfg,op,id,out);free(normal);free(id);free(op);return rc;}
    int registration=op&&(!strcmp(op,"register_text")||!strcmp(op,"register_pdf")||
        !strcmp(op,"register_html")||!strcmp(op,"register_docx")||!strcmp(op,"register_image")||
        !strcmp(op,"register_audio")||!strcmp(op,"register_media"));
    int live=op&&!strcmp(op,"locator_read");
    int removal=op&&!strcmp(op,"locator_watch_remove");
    int upload=op&&(!strcmp(op,"upload_append")||!strcmp(op,"upload_status")||!strcmp(op,"upload_abort")||!strcmp(op,"upload_commit"));
    int fetch=op&&!strcmp(op,"fetch");
    int embedding=op&&(!strcmp(op,"index_embed_step")||!strcmp(op,"index_embed_queue_step"));
    int text_query=op&&!strcmp(op,"memory_search_text");
    int search=op&&(!strcmp(op,"search")||!strcmp(op,"memory_search")||!strcmp(op,"memory_search_semantic")||!strcmp(op,"memory_search_text")||!strcmp(op,"memory_read"));
    int transcript_publication=op&&(!strcmp(op,"publish_transcript")||!strcmp(op,"publish_visual"));
    int audio_index=op&&(!strncmp(op,"audio_index_",12)||!strncmp(op,"visual_index_",13));
    int enrichment=op&&(!strcmp(op,"index_enrich_step")||!strcmp(op,"index_enrich_queue_step")||!strcmp(op,"index_enrichment_status"));
    int embedding_status=op&&!strcmp(op,"index_embedding_status");
    int publication=embedding_status||embedding||enrichment||audio_index||transcript_publication||(op&&(!strcmp(op,"index_publish_step")||!strcmp(op,"index_publish_queue_step")));
    int summary=op&&(!strcmp(op,"summarize_evidence")||!strcmp(op,"summarize_evidence_opening")||!strcmp(op,"publish_summary"));
    int temporal=op&&!strcmp(op,"analyze_video");
    int transcription=op&&!strcmp(op,"transcribe_audio");int visual=temporal||(op&&(!strcmp(op,"analyze_visual")||!strcmp(op,"publish_visual")));
    int inspection=op&&(!strcmp(op,"inspect_audio")||!strcmp(op,"inspect_visual"));
    int controlled=summary||visual||inspection||publication||search||fetch||upload||removal||live||registration||(op&&((!strcmp(op,"read_text")||!strcmp(op,"read_html")||!strcmp(op,"read_docx")||!strcmp(op,"read_image"))||!strcmp(op,"read_pdf")||!strcmp(op,"read_pdf_ocr")||!strcmp(op,"inventory")||!strcmp(op,"transcribe_audio")||!strcmp(op,"prepare_audio")||!strcmp(op,"prepare_visual")));
    if(!controlled){free(op);return sources_dispatch_core(cfg,input,length,out);}
    int id_token=json_get(&j,0,"operation_id"),parent_token=json_get(&j,0,"parent_operation_id"),budget_token=json_get(&j,0,"timeout_ms");
    if(id_token<0&&parent_token<0&&budget_token<0){free(op);return sources_dispatch_core(cfg,input,length,out);}
    char *id=json_string(&j,id_token),*parent=json_string(&j,parent_token);size_t budget=0;
    if((id_token>=0&&!identity(id))||(parent_token>=0&&(!identity(parent)||!id||!strcmp(id,parent)))||(budget_token>=0&&(!json_uint(&j,budget_token,&budget)||!budget||budget>(temporal?120000u:60000u)))){free(id);free(parent);free(op);return fail_refs(out,"invalid_request",NULL,NULL);}
    const char *fields=audio_index?((!strcmp(op,"audio_index_create")||!strcmp(op,"visual_index_create"))?"|contract_version|op|source_id|version|start_ms|end_ms|operation_id|parent_operation_id|timeout_ms|":(!strcmp(op,"audio_index_queue_step")||!strcmp(op,"visual_index_queue_step"))?"|contract_version|op|operation_id|parent_operation_id|timeout_ms|":(!strcmp(op,"audio_index_read")||!strcmp(op,"visual_index_read"))?"|contract_version|op|job_id|ordinal|operation_id|parent_operation_id|timeout_ms|":"|contract_version|op|job_id|operation_id|parent_operation_id|timeout_ms|"):transcript_publication?"|contract_version|op|source_id|version|start_ms|end_ms|operation_id|parent_operation_id|timeout_ms|":publication?(!strcmp(op,"index_publish_step")?"|contract_version|op|job_id|operation_id|parent_operation_id|timeout_ms|":"|contract_version|op|operation_id|parent_operation_id|timeout_ms|"):search?(!strcmp(op,"memory_search_semantic")?"|contract_version|op|vector|operation_id|parent_operation_id|timeout_ms|":!strcmp(op,"memory_read")?"|contract_version|op|store_id|external_source_id|external_artifact_id|start_byte|end_byte|operation_id|parent_operation_id|timeout_ms|":"|contract_version|op|query|operation_id|parent_operation_id|timeout_ms|"):fetch?"|contract_version|op|acquisition_id|url|format|operation_id|parent_operation_id|timeout_ms|":upload?(!strcmp(op,"upload_append")?"|contract_version|op|upload_id|offset|data|operation_id|parent_operation_id|timeout_ms|":"|contract_version|op|upload_id|operation_id|parent_operation_id|timeout_ms|"):removal?"|contract_version|op|watch_id|operation_id|parent_operation_id|timeout_ms|":live?"|contract_version|op|path|expected_device|expected_inode|read|operation_id|parent_operation_id|timeout_ms|":registration?"|contract_version|op|path|expected_file_identity|operation_id|parent_operation_id|timeout_ms|":(!strcmp(op,"transcribe_audio")||!strcmp(op,"prepare_audio")||!strcmp(op,"prepare_visual"))?"|contract_version|op|source_id|version|start_ms|end_ms|operation_id|parent_operation_id|timeout_ms|":!strcmp(op,"inventory")?"|contract_version|op|path|operation_id|parent_operation_id|timeout_ms|":(!strcmp(op,"read_text")||!strcmp(op,"read_html")||!strcmp(op,"read_docx")||!strcmp(op,"read_image"))?"|contract_version|op|source_id|version|start_byte|end_byte|operation_id|parent_operation_id|timeout_ms|":"|contract_version|op|source_id|version|start_page|end_page|operation_id|parent_operation_id|timeout_ms|";
    if(embedding_status)fields="|contract_version|op|job_id|after|operation_id|parent_operation_id|timeout_ms|";
    if(embedding)fields=!strcmp(op,"index_embed_step")?"|contract_version|op|job_id|operation_id|parent_operation_id|timeout_ms|":"|contract_version|op|operation_id|parent_operation_id|timeout_ms|";
    if(enrichment)fields=!strcmp(op,"index_enrichment_status")?"|contract_version|op|job_id|after|operation_id|parent_operation_id|timeout_ms|":!strcmp(op,"index_enrich_step")?"|contract_version|op|job_id|operation_id|parent_operation_id|timeout_ms|":"|contract_version|op|operation_id|parent_operation_id|timeout_ms|";
    if(summary)fields="|contract_version|op|source_id|version|evidence_id|operation_id|parent_operation_id|timeout_ms|";
    if(visual)fields="|contract_version|op|source_id|version|start_ms|end_ms|prompt|operation_id|parent_operation_id|timeout_ms|";
    if(inspection)fields="|contract_version|op|source_id|version|operation_id|parent_operation_id|timeout_ms|";
    char *normalized=canonical(&j,fields);
    if(normalized&&(text_query||embedding)){char binding[65];const char *e=sources_embedding_identity(cfg,binding);
        if(e){free(normalized);free(op);free(id);free(parent);return fail_refs(out,e,NULL,NULL);}
        char *bound=NULL;size_t bound_bytes=0;FILE *f=open_memstream(&bound,&bound_bytes);
        if(f){fputs("{\"request\":",f);fputs(normalized,f);fprintf(f,",\"embedding_binding\":\"%s\",\"store_id\":",binding);
            const char *store=cfg->chutni_store_id?cfg->chutni_store_id:"none";json_quote(f,store,strlen(store));fputc('}',f);
            if(fclose(f)){free(bound);bound=NULL;}}
        free(normalized);normalized=bound;}
    if(temporal&&normalized){
        const SourcesInferenceConfig *provider=cfg?cfg->temporal_inference:NULL;
        if(!provider||!provider->provider_id||!provider->engine_sha256||!provider->model_sha256){free(normalized);free(id);free(parent);free(op);return fail_refs(out,"inference_unconfigured",NULL,NULL);}
        char *bound=NULL;size_t bound_bytes=0;FILE *f=open_memstream(&bound,&bound_bytes);
        if(f){fputs("{\"request\":",f);fputs(normalized,f);fputs(",\"temporal_provider\":",f);json_quote(f,provider->provider_id,strlen(provider->provider_id));
            fputs(",\"engine_sha256\":",f);json_quote(f,provider->engine_sha256,strlen(provider->engine_sha256));
            fputs(",\"model_sha256\":",f);json_quote(f,provider->model_sha256,strlen(provider->model_sha256));fputc('}',f);
            if(fclose(f)){free(bound);bound=NULL;}}
        free(normalized);normalized=bound;
    }
    free(op);if(!normalized){free(id);free(parent);return fail_refs(out,"invalid_request",NULL,NULL);}
    SourcesConfig work=cfg?*cfg:(SourcesConfig){0};SourcesOperation operation={.directory=-1,.lease=-1,.cancel=-1};char *saved=NULL;const char *error=NULL;
    if(id){error=source_operation_begin_parent(&work,id,normalized,parent,&operation,&saved);if(!error&&saved){Json result;int rc=!json_parse(&result,saved,strlen(saved))||json_get(&result,0,"error")>=0;fputs(saved,out);free(saved);free(normalized);free(id);free(parent);source_operation_close(&operation);return rc;}
        if(!error){
            struct stat admitted;
            if(fstat(operation.directory,&admitted))error="state_unavailable";
            else{work.state_pinned=1;work.state_device=(size_t)admitted.st_dev;work.state_inode=(size_t)admitted.st_ino;}
            operation.parent_cancelled=work.cancelled;operation.parent_cancellation=work.cancellation;work.cancelled=source_operation_cancelled;work.cancellation=&operation;
        }}
    free(normalized);
    if(!error&&budget){if(received<0)error="state_unavailable";else{double deadline=received+(double)budget/1000;if(!work.deadline_monotonic||deadline<work.deadline_monotonic)work.deadline_monotonic=deadline;}}
    operation.deadline_monotonic=work.deadline_monotonic;
    char *body=NULL,*response=NULL;size_t body_length=0,response_length=0;FILE *body_stream=NULL,*response_stream=NULL;
    if(!error){body_stream=open_memstream(&body,&body_length);response_stream=open_memstream(&response,&response_length);if(!body_stream||!response_stream)error="resource_limit";}
    int rc=1;
    if(!error){fputc('{',body_stream);int first=1;
        for(int key=1;key<j.tokens[0].next;key=j.tokens[j.tokens[key].next].next){int value=j.tokens[key].next;if(value==id_token||value==parent_token||value==budget_token)continue;if(!first)fputc(',',body_stream);first=0;fwrite(j.text+j.tokens[key].begin,1,j.tokens[key].end-j.tokens[key].begin,body_stream);fputc(':',body_stream);fwrite(j.text+j.tokens[value].begin,1,j.tokens[value].end-j.tokens[value].begin,body_stream);}
        fputc('}',body_stream);int closed=fclose(body_stream);body_stream=NULL;if(closed)error="resource_limit";else rc=sources_dispatch_core(&work,body,body_length,response_stream);
        closed=fclose(response_stream);response_stream=NULL;if(closed)error="resource_limit";
    }
    if(body_stream)fclose(body_stream);
    if(response_stream)fclose(response_stream);
    free(body);
    Json result;if(!error&&(response_length>(live?2101248u:2097152u)||!json_parse(&result,response,response_length)||result.tokens[0].type!=JT_OBJECT))error="invalid_response";
    if(!error&&id){char *wrapped=NULL;size_t n;FILE *f=open_memstream(&wrapped,&n);if(!f)error="resource_limit";else{fwrite(response,1,result.tokens[0].end-1,f);fputs(",\"operation_id\":",f);json_quote(f,id,strlen(id));if(parent){fputs(",\"parent_operation_id\":",f);json_quote(f,parent,strlen(parent));}fputs("}\n",f);if(fclose(f)){free(wrapped);error="resource_limit";}else{free(response);response=wrapped;response_length=n;}}}
    if(!error&&id){operation.committed_result=(visual||transcription||publication||search||fetch||removal||upload)&&!rc;error=source_operation_finish(&operation,&response,&rc);}
    if(error)rc=fail_refs(out,error,id,parent);else fwrite(response,1,strlen(response),out);
    source_operation_close(&operation);free(response);free(id);free(parent);return rc;
}
