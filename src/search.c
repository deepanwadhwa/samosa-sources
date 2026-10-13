#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 8192
#include "search.h"
#include "fetch.h"
#include "uploads.h"
#include "control.h"
#include "sha256.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static int failure(FILE *out,const char *code){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",code);return 1;}
static int name_valid(const char *s){if(!s||!*s||strlen(s)>64)return 0;for(size_t i=0;s[i];i++)if((unsigned char)s[i]>127||(!isalnum((unsigned char)s[i])&&!strchr("_-.",s[i])))return 0;return 1;}
static char *encode(const char *s){
    size_t n=strlen(s);char *out=malloc(n*3+1);if(!out)return NULL;size_t w=0;const char *hex="0123456789ABCDEF";
    for(size_t i=0;i<n;i++){unsigned c=(unsigned char)s[i];if((c<128&&isalnum(c))||strchr("-._~",c))out[w++]=(char)c;else{out[w++]='%';out[w++]=hex[c>>4];out[w++]=hex[c&15];}}out[w]=0;return out;
}
typedef struct {const char *query;char session[33];Json values;char *secrets[16];size_t secret_count;} Render;
static char *expand(const char *text,const Render *r,int uri){
    char output[8193];size_t w=0;
    for(size_t i=0;text[i];){
        if(text[i]!='{'){if(w==8192)return NULL;output[w++]=text[i++];continue;}
        const char *end=strchr(text+i,'}');if(!end||end-text-(ptrdiff_t)i>65)return NULL;
        char key[65];size_t n=(size_t)(end-text)-i-1;if(!n||n>=sizeof(key))return NULL;memcpy(key,text+i+1,n);key[n]=0;
        char *value=!strcmp(key,"query")?strdup(r->query):!strcmp(key,"session_id")?strdup(r->session):json_string(&r->values,json_get(&r->values,0,key));
        if(!value||!*value){free(value);return NULL;}
        char *sub=uri&&strcmp(key,"base_url")?encode(value):strdup(value);free(value);
        if(!sub)return NULL;
        n=strlen(sub);if(n>8192-w){free(sub);return NULL;}memcpy(output+w,sub,n);w+=n;free(sub);i=(size_t)(end-text)+1;
    }
    output[w]=0;return strdup(output);
}
static int render_value(const Json *j,int t,const Render *r,FILE *out){
    if(j->tokens[t].type==JT_STRING){char *old=json_string(j,t),*value=old?expand(old,r,0):NULL;free(old);if(!value)return 0;json_quote(out,value,strlen(value));free(value);return 1;}
    if(j->tokens[t].type==JT_OBJECT||j->tokens[t].type==JT_ARRAY){
        int object=j->tokens[t].type==JT_OBJECT;fputc(object?'{':'[',out);int first=1;
        for(int child=t+1;child<j->tokens[t].next;){
            if(!first)fputc(',',out);
            first=0;int value=child;
            if(object){char *key=json_string(j,child);if(!key)return 0;json_quote(out,key,strlen(key));free(key);fputc(':',out);value=j->tokens[child].next;}
            if(!render_value(j,value,r,out))return 0;
            child=j->tokens[value].next;
        }
        fputc(object?'}':']',out);return 1;
    }
    fwrite(j->text+j->tokens[t].begin,1,j->tokens[t].end-j->tokens[t].begin,out);return 1;
}
static char *render_json(const char *text,const Render *r){
    if(!text)return NULL;
    Json j;if(strlen(text)>8192||!json_parse(&j,text,strlen(text)) )return NULL;
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);if(!out)return NULL;int good=render_value(&j,0,r,out);if(fclose(out)||!good||n>8192){free(body);return NULL;}return body;
}
static int path(const Json *j,int object,const char *name){
    if(!name||strlen(name)>128)return -1;
    if(!*name)return object;
    char key[129];const char *start=name;int value=object,count=0;
    while(*start){const char *dot=strchr(start,'.');size_t n=dot?(size_t)(dot-start):strlen(start);if(!n||n>=sizeof(key)||++count>8)return -1;memcpy(key,start,n);key[n]=0;value=json_get(j,value,key);if(value<0)return -1;if(!dot)break;start=dot+1;}
    return value;
}
static int secret_at(const Render *r,const char *text,size_t *length){
    for(size_t k=0;k<r->secret_count;k++){
        const char *value=r->secrets[k];size_t n=strlen(value);int match=1;
        for(size_t i=0;i<n;){
            if(!text[i]){match=0;break;}
            if((k&1)&&value[i]=='%'){
                if(text[i]!='%'||!text[i+1]||!text[i+2]||json_hex(text[i+1])!=json_hex(value[i+1])||json_hex(text[i+2])!=json_hex(value[i+2])){match=0;break;}i+=3;
            }else{if(text[i]!=value[i]){match=0;break;}i++;}
        }
        if(match&&n){*length=n;return 1;}
    }
    return 0;
}
static void render_close(Render *r){for(size_t i=0;i<r->secret_count;i++)free(r->secrets[i]);}
static char *sanitize(const Render *r,const char *text,size_t characters,size_t bytes){
    char *out=malloc(bytes+1);if(!out)return NULL;size_t w=0,used=0;
    for(size_t i=0;text[i]&&used<characters;){
        size_t secret=0;if(secret_at(r,text+i,&secret)){
            const char *label="<redacted>";size_t n=strlen(label);if(n>bytes-w||n>characters-used)break;memcpy(out+w,label,n);w+=n;used+=n;i+=secret;continue;
        }
        unsigned c=(unsigned char)text[i];size_t n=c<128?1:c<224?2:c<240?3:4;if(n>bytes-w)break;memcpy(out+w,text+i,n);w+=n;i+=n;used++;
    }
    out[w]=0;return out;
}
static int url_valid(const char *url,const Render *r){
    if(!url||!*url||strlen(url)>2048)return 0;
    size_t scheme=!strncmp(url,"https://",8)?8:!strncmp(url,"http://",7)?7:0;if(!scheme)return 0;
    const char *host=url+scheme,*end=host+strcspn(host,"/?#");if(end==host)return 0;
    for(const char *p=host;p<end;p++)if(!isalnum((unsigned char)*p)&&!strchr(".-:[]",*p))return 0;
    for(size_t i=0;url[i];i++){size_t secret=0;if((unsigned char)url[i]<=32||(unsigned char)url[i]>=127||url[i]=='\\'||secret_at(r,url+i,&secret))return 0;}
    return 1;
}
/* JSON or Streamable HTTP event data, bounded to32 records and one result. */
static int parse_provider(Json *j,const char *body,size_t n,const char *results,char **owned){
    *owned=NULL;if(json_parse(j,body,n))return 1;
    char *candidate=malloc(n+1),*selected=NULL;if(!candidate)return 0;size_t w=0,records=0;int good=1;
    for(size_t i=0;i<=n;){
        size_t start=i;while(i<n&&body[i]!='\n')i++;size_t length=i-start;if(length&&body[start+length-1]=='\r')length--;if(i<n)i++;else i=n+1;
        if(!length){
            if(w){if(++records>32){good=0;break;}candidate[w]=0;Json event;if(json_parse(&event,candidate,w)&&path(&event,0,results)>=0){if(selected){good=0;break;}selected=strdup(candidate);if(!selected){good=0;break;}}w=0;}
        }else if(length>=5&&!memcmp(body+start,"data:",5)){size_t offset=start+5;if(offset<start+length&&body[offset]==' ')offset++;size_t amount=start+length-offset;if(w)candidate[w++]='\n';if(amount>n-w){good=0;break;}memcpy(candidate+w,body+offset,amount);w+=amount;}
    }
    if(good&&w){candidate[w]=0;Json event;if(json_parse(&event,candidate,w)&&path(&event,0,results)>=0){if(selected)good=0;else selected=strdup(candidate);}}
    free(candidate);if(!good||!selected){free(selected);return 0;}*owned=selected;return json_parse(j,selected,strlen(selected));
}
static char *description(const Json *j,int t){
    if(t<0)return NULL;
    if(j->tokens[t].type==JT_STRING)return json_string(j,t);
    if(j->tokens[t].type!=JT_ARRAY||j->tokens[t].count>32)return NULL;
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);if(!out)return NULL;int good=1,first=1;
    for(int item=t+1;item<j->tokens[t].next;item=j->tokens[item].next){char *text=json_string(j,item);if(!text){good=0;break;}if(!first)fputc('\n',out);first=0;fputs(text,out);free(text);}
    if(fclose(out)||!good){free(body);return NULL;}return body;
}
static const char *render_init(Render *r,const char *query,const char *values){
    memset(r,0,sizeof(*r));r->query=query;if(!values)values="{}";
    if(strlen(values)>4096||!json_parse(&r->values,values,strlen(values))||r->values.tokens[0].type!=JT_OBJECT||r->values.tokens[0].count>8)return "invalid_search_configuration";
    for(int t=1;t<r->values.tokens[0].next;t=r->values.tokens[r->values.tokens[t].next].next){
        char *name=json_string(&r->values,t),*value=json_string(&r->values,r->values.tokens[t].next);int bad=!name_valid(name)||!strcmp(name?name:"","query")||!strcmp(name?name:"","session_id")||!value||!*value||strlen(value)>1024;free(name);
        if(bad){free(value);return "invalid_search_configuration";}
        r->secrets[r->secret_count++]=value;char *escaped=encode(value);if(!escaped)return "resource_limit";r->secrets[r->secret_count++]=escaped;
    }
    unsigned char random[16];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)return "state_unavailable";size_t offset=0;
    while(offset<sizeof(random)){ssize_t got=read(fd,random+offset,sizeof(random)-offset);if(got<0&&errno==EINTR)continue;if(got<=0){close(fd);return "state_unavailable";}offset+=(size_t)got;}close(fd);
    for(size_t i=0;i<sizeof(random);i++)snprintf(r->session+i*2,3,"%02x",random[i]);
    return NULL;
}
int source_search_dispatch(const SourcesConfig *cfg,const char *input,size_t length,sqlite3 *db,FILE *out){
    Json request;char *query=NULL;
    if(!json_parse(&request,input,length)||(query=json_string(&request,json_get(&request,0,"query")))==NULL||!*query||strlen(query)>512){free(query);return failure(out,"invalid_request");}
    const SourcesSearchConfig *c=cfg->search;const char *error=NULL;
    if(!c||!name_valid(c->provider)||!c->origin||!c->url_template||!c->results_path||!c->title_path||!c->url_path||!c->description_path){free(query);return failure(out,"search_unavailable");}
    Render r;error=render_init(&r,query,c->values);
    char *url=NULL,*headers=NULL,*post=NULL;
    if(!error){url=expand(c->url_template,&r,1);headers=c->headers?render_json(c->headers,&r):NULL;post=c->body?render_json(c->body,&r):NULL;if(!url||(c->headers&&!headers)||(c->body&&!post))error="search_credentials_missing";}
    SourcesConfig transfer=*cfg;transfer.fetch_origin=c->origin;
    unsigned char *bytes=NULL;size_t n=0;
    if(!error)error=sources_native_http_limited(&transfer,url,post?"POST":"GET",headers,post,SOURCES_SEARCH_HTTP_MAX,&bytes,&n);
    Json provider;char *owned=NULL,*payload=NULL;size_t size=0;FILE *stream=NULL;int array=-1;
    if(!error){
        if(!parse_provider(&provider,(const char *)bytes,n,c->results_path,&owned)||(array=path(&provider,0,c->results_path))<0||provider.tokens[array].type!=JT_ARRAY)error="search_response_invalid";
        else{
            int e=json_get(&provider,0,"error");if(e>=0&&provider.tokens[e].type!=JT_NULL)error="search_response_invalid";
            if(!strcmp(c->provider,"parallel")){size_t id=0;char *rpc=json_string(&provider,json_get(&provider,0,"jsonrpc"));int result=json_get(&provider,0,"result"),failed=json_get(&provider,result,"isError");if(!rpc||strcmp(rpc,"2.0")||!json_uint(&provider,json_get(&provider,0,"id"),&id)||id!=1||(failed>=0&&(provider.tokens[failed].type!=JT_BOOL||strncmp(provider.text+provider.tokens[failed].begin,"false",5))))error="search_response_invalid";free(rpc);}
        }
    }
    if(!error){stream=open_memstream(&payload,&size);if(!stream)error="resource_limit";}
    if(!error){
        struct timespec captured;if(clock_gettime(CLOCK_REALTIME,&captured)||captured.tv_sec<0)error="state_unavailable";
        else{
            char *safe_query=sanitize(&r,query,512,2048);if(!safe_query)error="resource_limit";
            else{fputs("{\"provider\":",stream);json_quote(stream,c->provider,strlen(c->provider));fputs(",\"query\":",stream);json_quote(stream,safe_query,strlen(safe_query));free(safe_query);fprintf(stream,",\"observed_at_unix_ms\":%llu,\"coverage\":{\"status\":\"partial\",\"reason\":\"provider_search_metadata\"},\"items\":[",(unsigned long long)captured.tv_sec*1000+(unsigned long long)captured.tv_nsec/1000000);}
        }
        size_t count=0;
        for(int item=array+1;!error&&item<provider.tokens[array].next&&count<8;item=provider.tokens[item].next){
            char *title=json_string(&provider,path(&provider,item,c->title_path)),*link=json_string(&provider,path(&provider,item,c->url_path)),*text=description(&provider,path(&provider,item,c->description_path));
            char *safe_title=title?sanitize(&r,title,256,1024):NULL,*safe_text=text?sanitize(&r,text,400,1600):NULL;
            if(!safe_title||!*safe_title||!safe_text||!url_valid(link,&r))error="search_response_invalid";
            else{if(count++)fputc(',',stream);fputs("{\"title\":",stream);json_quote(stream,safe_title,strlen(safe_title));fputs(",\"url\":",stream);json_quote(stream,link,strlen(link));fputs(",\"description\":",stream);json_quote(stream,safe_text,strlen(safe_text));fputc('}',stream);}
            free(title);free(link);free(text);free(safe_title);free(safe_text);
        }
        if(!error)fputs("]}",stream);
    }
    if(stream&&fclose(stream))error="resource_limit";
    render_close(&r);free(owned);free(bytes);free(url);free(headers);free(post);free(query);
    char *admission=NULL,*response=NULL;size_t admitted=0,response_length=0;int transaction=0;
    if(!error){if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)error="store_busy";else transaction=1;}
    if(!error){FILE *f=open_memstream(&admission,&admitted);if(!f)error="resource_limit";else{char locator[80];snprintf(locator,sizeof(locator),"search:%s",c->provider);int failed=source_snapshot_accept(cfg,db,f,(const unsigned char *)payload,size,locator,0);if(fclose(f))error="resource_limit";else if(failed){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);fputs(admission,out);free(admission);free(payload);return 1;}}}
    if(!error){
        Json a;if(!json_parse(&a,admission,admitted))error="invalid_response";
        else{
            char source[65],material[100],evidence[65];hash_bytes(payload,size,source);snprintf(material,sizeof(material),"search-metadata-v1:%s",source);hash_bytes(material,strlen(material),evidence);
            FILE *f=open_memstream(&response,&response_length);if(!f)error="resource_limit";
            else{fwrite(admission,1,a.tokens[0].end-1,f);fprintf(f,",\"evidence_id\":\"%s\",\"search\":",evidence);fwrite(payload,1,size,f);fputs("}\n",f);if(fclose(f))error="resource_limit";}
        }
    }
    if(!error)error=sources_interrupted(cfg);
    if(transaction){if(error)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);error="store_failed";}}
    if(!error)fwrite(response,1,response_length,out);
    free(response);free(admission);free(payload);return error?failure(out,error):0;
}
int sources_search_preset(const char *name,SourcesSearchConfig *c){
    if(!name||!c)return 1;
    *c=(SourcesSearchConfig){.provider=name,.title_path="title",.url_path="url",.description_path="description",.headers="{\"Accept\":\"application/json\"}"};
    if(!strcmp(name,"parallel")){c->origin="https://search.parallel.ai";c->url_template="https://search.parallel.ai/mcp";c->headers="{\"Content-Type\":\"application/json\",\"Accept\":\"application/json, text/event-stream\"}";c->body="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"web_search\",\"arguments\":{\"objective\":\"{query}\",\"search_queries\":[\"{query}\"],\"session_id\":\"{session_id}\"}}}";c->results_path="result.structuredContent.results";c->description_path="excerpts";}
    else if(!strcmp(name,"brave")){c->origin="https://api.search.brave.com";c->url_template="https://api.search.brave.com/res/v1/web/search?q={query}&count=8";c->headers="{\"Accept\":\"application/json\",\"X-Subscription-Token\":\"{api_key}\"}";c->results_path="web.results";}
    else if(!strcmp(name,"tavily")){c->origin="https://api.tavily.com";c->url_template="https://api.tavily.com/search";c->headers="{\"Content-Type\":\"application/json\"}";c->body="{\"api_key\":\"{api_key}\",\"query\":\"{query}\",\"max_results\":8}";c->results_path="results";c->description_path="content";}
    else if(!strcmp(name,"serpapi")){c->origin="https://serpapi.com";c->url_template="https://serpapi.com/search.json?engine=google&q={query}&api_key={api_key}";c->results_path="organic_results";c->url_path="link";c->description_path="snippet";}
    else if(!strcmp(name,"google")){c->origin="https://www.googleapis.com";c->url_template="https://www.googleapis.com/customsearch/v1?key={api_key}&cx={cx}&q={query}";c->results_path="items";c->url_path="link";c->description_path="snippet";}
    else if(!strcmp(name,"searxng")){c->url_template="{base_url}/search?q={query}&format=json";c->results_path="results";c->description_path="content";}
    else{*c=(SourcesSearchConfig){0};return 1;}
    return 0;
}
