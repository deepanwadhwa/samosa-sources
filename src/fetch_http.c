#define _POSIX_C_SOURCE 200809L
#include "fetch.h"
#include "fetch_public.h"
#include "control.h"
#include "json.h"
#include "sha256.h"
#include <curl/curl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <strings.h>
typedef struct {const SourcesConfig *cfg;unsigned char *bytes;size_t length,headers,limit;const char *error;char location[SOURCES_FETCH_URL_MAX+1],content_type[128];} Transfer;
static size_t receive(char *p,size_t size,size_t count,void *context){
    Transfer *t=context;
    if(!t->error)t->error=sources_interrupted(t->cfg);
    if(t->error)return 0;
    if(size&&count>SIZE_MAX/size){t->error="fetch_too_large";return 0;}
    size_t n=size*count;
    if(n>t->limit-t->length){t->error="fetch_too_large";return 0;}
    memcpy(t->bytes+t->length,p,n);t->length+=n;return n;
}
static size_t header(char *p,size_t size,size_t count,void *context){
    Transfer *t=context;
    if(!t->error)t->error=sources_interrupted(t->cfg);
    if(t->error)return 0;
    if(size&&count>SIZE_MAX/size){t->error="fetch_header_limit";return 0;}
    size_t n=size*count;
    if(n>65536u-t->headers){t->error="fetch_header_limit";return 0;}
    t->headers+=n;
    if(n>=5&&!memcmp(p,"HTTP/",5)){t->location[0]=0;t->content_type[0]=0;}
    if(n>=9&&!strncasecmp(p,"Location:",9)){
        const char *value=p+9;size_t count=n-9;
        while(count&&(*value==' '||*value=='\t')){value++;count--;}
        while(count&&(value[count-1]=='\r'||value[count-1]=='\n'||value[count-1]==' '||value[count-1]=='\t'))count--;
        if(!count||count>SOURCES_FETCH_URL_MAX||t->location[0]||memchr(value,0,count)){t->error="fetch_redirect_invalid";return 0;}
        memcpy(t->location,value,count);t->location[count]=0;
    }
    if(n>=13&&!strncasecmp(p,"Content-Type:",13)){
        const char *value=p+13;size_t count=n-13;
        while(count&&(*value==' '||*value=='\t')){value++;count--;}
        size_t end=0;while(end<count&&value[end]!=';'&&value[end]!='\r'&&value[end]!='\n'&&value[end]!=' ')end++;
        t->content_type[0]=0;
        if(end<sizeof(t->content_type)){for(size_t i=0;i<end;i++)t->content_type[i]=(char)tolower((unsigned char)value[i]);t->content_type[end]=0;}
    }
    return n;
}
static int progress(void *context,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d){
    (void)a;(void)b;(void)c;(void)d;Transfer *t=context;
    if(!t->error)t->error=sources_interrupted(t->cfg);
    return t->error!=NULL;
}
static const char *fetch_once(const SourcesConfig *cfg,const void *input,size_t length,FILE *out,int redirects,char content_type[128]){
    struct sigaction no_pipe={.sa_handler=SIG_IGN};sigemptyset(&no_pipe.sa_mask);
    if(sigaction(SIGPIPE,&no_pipe,NULL))return "fetch_unavailable";
    Json j;
    if(length>SOURCES_HTTP_REQUEST_MAX||!json_parse(&j,input,length)||j.tokens[0].type!=JT_OBJECT||(j.tokens[0].count<3||j.tokens[0].count>8))return "invalid_request";
    char *origin=json_string(&j,json_get(&j,0,"origin")),*url=json_string(&j,json_get(&j,0,"url"));
    char *ca=json_string(&j,json_get(&j,0,"ca_pem"));
    char *method=json_string(&j,json_get(&j,0,"method")),*headers=json_string(&j,json_get(&j,0,"headers")),*post=json_string(&j,json_get(&j,0,"post"));
    SourcesConfig grant=*cfg;grant.fetch_origin=origin;grant.fetch_helper="worker";grant.fetch_ca_pem=ca;
    int public_field=json_get(&j,0,"public_only");size_t public_only=0;
    const char *error=NULL;
    if(public_field>=0&&(!json_uint(&j,public_field,&public_only)||public_only>1))error="invalid_request";
    grant.fetch_public_only=(int)public_only;
    if(!error)error=sources_fetch_authorize(&grant,url);
    if(!error)error=sources_http_options_validate(method,headers,post);
    int limit_field=json_get(&j,0,"max_bytes");size_t limit=SOURCES_FETCH_MAX;
    if(!error&&limit_field>=0&&(!json_uint(&j,limit_field,&limit)||!limit||limit>SOURCES_FETCH_MAX))error="invalid_request";
    if(!error&&j.tokens[0].count!=3+(ca!=NULL)+(headers!=NULL)+(post!=NULL)+(limit_field>=0)+(public_field>=0))error="invalid_request";
    char hostname[256],address[SOURCES_FETCH_ADDRESS_MAX],resolution[384];unsigned port=0;
    struct curl_slist *resolved=NULL;
    if(!error&&public_only){
        error=sources_fetch_resolve(&grant,hostname,&port,address);
        if(!error){
            int n=strchr(address,':')?snprintf(resolution,sizeof(resolution),"%s:%u:[%s]",hostname,port,address):snprintf(resolution,sizeof(resolution),"%s:%u:%s",hostname,port,address);
            if(n<0||(size_t)n>=sizeof(resolution))error="invalid_fetch_configuration";
            /* An IPv6 literal already pins the URL to that exact address and
             * must not be rendered as a hostname:port curl resolver key. */
            else if(!strchr(hostname,':')&&!(resolved=curl_slist_append(NULL,resolution)))error="resource_limit";
        }
    }
    struct curl_slist *request_headers=NULL;
    if(!error&&headers){
        Json h;json_parse(&h,headers,strlen(headers));
        for(int key=1;!error&&key<h.tokens[0].next;key=h.tokens[h.tokens[key].next].next){
            char *name=json_string(&h,key),*value=json_string(&h,h.tokens[key].next);char line[1100];
            if(!name||!value){free(name);free(value);error="resource_limit";break;}
            snprintf(line,sizeof(line),"%s: %s",name,value);free(name);free(value);
            struct curl_slist *next=curl_slist_append(request_headers,line);
            if(!next)error="resource_limit";else request_headers=next;
        }
    }
    Transfer t={.cfg=cfg,.limit=limit};CURL *curl=NULL;int trust_fd=-1;char trust_path[64]={0};
    if(!error&&ca){
        char temporary[]="/tmp/samosa-fetch-trust-XXXXXX";
        trust_fd=mkstemp(temporary);
        if(trust_fd<0)error="fetch_tls_trust_invalid";
        else{
            if(unlink(temporary)||fcntl(trust_fd,F_SETFD,FD_CLOEXEC)<0)error="fetch_tls_trust_invalid";
            size_t n=strlen(ca),offset=0;
            while(!error&&offset<n){ssize_t sent=write(trust_fd,ca+offset,n-offset);if(sent<0&&errno==EINTR)continue;if(sent<=0)error="fetch_tls_trust_invalid";else offset+=(size_t)sent;}
            if(!error&&lseek(trust_fd,0,SEEK_SET)<0)error="fetch_tls_trust_invalid";
#ifdef __linux__
            snprintf(trust_path,sizeof(trust_path),"/proc/self/fd/%d",trust_fd);
#else
            snprintf(trust_path,sizeof(trust_path),"/dev/fd/%d",trust_fd);
#endif
        }
    }
    if(!error){t.bytes=malloc(limit);if(!t.bytes)error="resource_limit";}
    if(!error&&curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)error="fetch_unavailable";
    if(!error){curl=curl_easy_init();if(!curl)error="fetch_unavailable";}
#define OPTION(key,value) do{if(!error&&curl_easy_setopt(curl,key,value)!=CURLE_OK)error="fetch_unavailable";}while(0)
    OPTION(CURLOPT_URL,url);
    OPTION(CURLOPT_HTTPHEADER,request_headers);
    if(post){OPTION(CURLOPT_POST,1L);OPTION(CURLOPT_POSTFIELDS,post);OPTION(CURLOPT_POSTFIELDSIZE,(long)strlen(post));}
    OPTION(CURLOPT_PROXY,"");
    if(public_only){
        if(resolved)OPTION(CURLOPT_RESOLVE,resolved);
        OPTION(CURLOPT_IPRESOLVE,strchr(address,':')?CURL_IPRESOLVE_V6:CURL_IPRESOLVE_V4);
    }
    OPTION(CURLOPT_FOLLOWLOCATION,0L);
    OPTION(CURLOPT_SSL_VERIFYPEER,1L);
    OPTION(CURLOPT_SSL_VERIFYHOST,2L);
    if(ca){OPTION(CURLOPT_CAINFO,trust_path);OPTION(CURLOPT_CAPATH,(char *)NULL);}
    OPTION(CURLOPT_NOSIGNAL,1L);
    OPTION(CURLOPT_CONNECTTIMEOUT_MS,5000L);
    OPTION(CURLOPT_TIMEOUT_MS,30000L);
    OPTION(CURLOPT_WRITEFUNCTION,receive);
    OPTION(CURLOPT_WRITEDATA,&t);
    OPTION(CURLOPT_HEADERFUNCTION,header);
    OPTION(CURLOPT_HEADERDATA,&t);
    OPTION(CURLOPT_NOPROGRESS,0L);
    OPTION(CURLOPT_XFERINFOFUNCTION,progress);
    OPTION(CURLOPT_XFERINFODATA,&t);
#if LIBCURL_VERSION_NUM >= 0x075500
    OPTION(CURLOPT_PROTOCOLS_STR,"http,https");
#else
    OPTION(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#endif
#undef OPTION
    long status=0;
    if(!error){
        CURLcode code=curl_easy_perform(curl);
        error=t.error;
        if(!error&&code!=CURLE_OK)error=code==CURLE_OPERATION_TIMEDOUT?"deadline_exceeded":
            code==CURLE_PEER_FAILED_VERIFICATION?"fetch_tls_verification_failed":
            code==CURLE_SSL_CACERT_BADFILE?"fetch_tls_trust_invalid":
            code==CURLE_SSL_CONNECT_ERROR?"fetch_tls_failed":"fetch_failed";
        if(!error&&curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK)error="fetch_http_status";
        if(!error&&status!=200&&!(redirects&&(status==301||status==302||status==303||status==307||status==308)&&t.location[0]))error="fetch_http_status";
        if(!error&&public_only){
            char *peer=NULL;
            if(curl_easy_getinfo(curl,CURLINFO_PRIMARY_IP,&peer)!=CURLE_OK||!sources_fetch_address_equal(peer,address))error="fetch_address_changed";
        }
    }
    if(curl)curl_easy_cleanup(curl);
    curl_slist_free_all(request_headers);
    curl_slist_free_all(resolved);
    if(trust_fd>=0)close(trust_fd);
    if(!error)error=sources_interrupted(cfg);
    if(!error&&content_type)memcpy(content_type,t.content_type,sizeof(t.content_type));
    if(!error&&status!=200){
        fprintf(out,"{\"status\":%ld,\"location\":",status);json_quote(out,t.location,strlen(t.location));fputs("}\n",out);
    }else if(!error){
        char hash[65];hash_bytes(t.bytes,t.length,hash);
        fputs("{\"url\":",out);json_quote(out,url,strlen(url));
        fprintf(out,",\"status\":200,\"bytes\":%zu,\"digest\":\"%s\",\"data\":\"",t.length,hash);
        const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for(size_t i=0;i<t.length;i+=3){
            unsigned a=t.bytes[i],b=i+1<t.length?t.bytes[i+1]:0,c=i+2<t.length?t.bytes[i+2]:0;
            fputc(alphabet[a>>2],out);fputc(alphabet[((a&3)<<4)|(b>>4)],out);
            fputc(i+1<t.length?alphabet[((b&15)<<2)|(c>>6)]:'=',out);
            fputc(i+2<t.length?alphabet[c&63]:'=',out);
        }
        fputs("\"}\n",out);
    }
    free(t.bytes);free(origin);free(url);free(ca);free(method);free(headers);free(post);return error;
}
static const char *public_request(const char *origin,const char *url,const char *ca,size_t limit,char **request,size_t *length){
    FILE *stream=open_memstream(request,length);if(!stream)return "resource_limit";
    fputs("{\"origin\":",stream);json_quote(stream,origin,strlen(origin));fputs(",\"url\":",stream);json_quote(stream,url,strlen(url));
    fputs(",\"method\":\"GET\",\"public_only\":1,\"headers\":\"{\\\"User-Agent\\\":\\\"SamosaChat/1.0 (+local user-initiated fetch)\\\"}\"",stream);
    if(ca){fputs(",\"ca_pem\":",stream);json_quote(stream,ca,strlen(ca));}
    fprintf(stream,",\"max_bytes\":%zu}",limit);return fclose(stream)?"resource_limit":NULL;
}
static const char *rate_request(const SourcesConfig *cfg,const char *origin){
    return cfg->fetch_rate?cfg->fetch_rate(cfg->fetch_rate_context,origin):"invalid_fetch_configuration";
}
/* The monolith probes robots without recursively enforcing robots. Errors or
 * non-text replies allow the page, but caller cancellation/deadline still wins. */
static const char *robots_gate(const SourcesConfig *cfg,const char *origin,const char *page,const char *ca){
    char robots[280];snprintf(robots,sizeof(robots),"%s/robots.txt",origin);
    SourcesConfig grant=*cfg;grant.fetch_origin=origin;grant.fetch_helper="worker";grant.fetch_public_only=1;
    SourcesFetchRoute route;const char *error=sources_fetch_route_begin(&grant,robots,&route);
    while(!error){
        char authority[257],content_type[128]={0};const char *current=route.urls[route.count-1];error=sources_fetch_url_origin(current,authority);
        if(!error)error=rate_request(cfg,authority);
        char *request=NULL,*response=NULL;size_t request_length=0,response_length=0;
        if(!error)error=public_request(authority,current,ca,256u*1024u,&request,&request_length);
        FILE *stream=NULL;
        if(!error){stream=open_memstream(&response,&response_length);if(!stream)error="resource_limit";}
        if(!error)error=fetch_once(cfg,request,request_length,stream,1,content_type);
        if(stream&&fclose(stream)&&!error)error="resource_limit";
        free(request);
        if(error){free(response);break;}
        Json reply;size_t status=0;
        if(!json_parse(&reply,response,response_length)||!json_uint(&reply,json_get(&reply,0,"status"),&status)){free(response);break;}
        if(status!=200){char *location=json_string(&reply,json_get(&reply,0,"location"));error=sources_fetch_route_next(location,&route);free(location);free(response);continue;}
        unsigned char *bytes=NULL;size_t length=0;
        error=sources_fetch_validate(current,response,response_length,&bytes,&length);free(response);
        if(!error&&(!strcmp(content_type,"text/plain")||!strcmp(content_type,"text/html"))&&!memchr(bytes,0,length)){
            const char *path=page+strlen(origin);if(!*path||*path=='?')path="/";
            bytes[length]=0;int allowed=sources_fetch_robots_allowed((const char *)bytes,"samosachat",path);free(bytes);
            if(!allowed)return "fetch_robots_denied";
        }else free(bytes);
        break;
    }
    return sources_interrupted(cfg);
}
const char *sources_fetch_worker(const SourcesConfig *cfg,const void *input,size_t length,FILE *out){
    Json j;
    if(length>SOURCES_HTTP_REQUEST_MAX||!json_parse(&j,input,length)||j.tokens[0].type!=JT_OBJECT)return "invalid_request";
    int field=json_get(&j,0,"public_web");
    if(field<0)return fetch_once(cfg,input,length,out,0,NULL);
    size_t enabled=0,public_only=0,limit=SOURCES_FETCH_MAX;
    if(!json_uint(&j,field,&enabled)||enabled!=1||!json_uint(&j,json_get(&j,0,"public_only"),&public_only)||public_only!=1)return "invalid_request";
    char *origin=json_string(&j,json_get(&j,0,"origin")),*url=json_string(&j,json_get(&j,0,"url")),*method=json_string(&j,json_get(&j,0,"method")),*ca=json_string(&j,json_get(&j,0,"ca_pem"));
    int limit_field=json_get(&j,0,"max_bytes");const char *error=NULL;
    if(!method||strcmp(method,"GET")||j.tokens[0].count!=5+(ca!=NULL)+(limit_field>=0)||
       (limit_field>=0&&(!json_uint(&j,limit_field,&limit)||!limit||limit>SOURCES_FETCH_MAX)))error="invalid_request";
    SourcesConfig grant=*cfg;grant.fetch_origin=origin;grant.fetch_helper="worker";grant.fetch_ca_pem=ca;grant.fetch_public_only=1;
    SourcesFetchRoute route;if(!error)error=sources_fetch_route_begin(&grant,url,&route);
    char *response=NULL;size_t response_length=0;
    while(!error){
        char hop_origin[257];const char *current=route.urls[route.count-1];error=sources_fetch_url_origin(current,hop_origin);if(error)break;
        error=robots_gate(cfg,hop_origin,current,ca);if(!error)error=rate_request(cfg,hop_origin);if(error)break;
        char *request=NULL;size_t request_length=0;error=public_request(hop_origin,current,ca,limit,&request,&request_length);if(error){free(request);break;}
        FILE *stream=open_memstream(&response,&response_length);
        if(!stream){free(request);error="resource_limit";break;}
        error=fetch_once(cfg,request,request_length,stream,1,NULL);free(request);
        if(fclose(stream)&&!error)error="resource_limit";
        if(error)break;
        Json reply;size_t status=0;
        if(!json_parse(&reply,response,response_length)||!json_uint(&reply,json_get(&reply,0,"status"),&status)){error="native_helper_protocol";break;}
        if(status==200)break;
        char *location=json_string(&reply,json_get(&reply,0,"location"));error=sources_fetch_route_next(location,&route);free(location);free(response);response=NULL;response_length=0;
    }
    if(!error){
        Json reply;json_parse(&reply,response,response_length);
        fputs("{\"url\":",out);json_quote(out,url,strlen(url));
        const char *fields[]={"status","bytes","digest","data"};
        for(size_t i=0;i<4;i++){int token=json_get(&reply,0,fields[i]);fprintf(out,",\"%s\":",fields[i]);fwrite(response+reply.tokens[token].begin,1,reply.tokens[token].end-reply.tokens[token].begin,out);}
        fputs(",\"urls\":[",out);
        for(size_t i=0;i<route.count;i++){if(i)fputc(',',out);json_quote(out,route.urls[i],strlen(route.urls[i]));}fputs("]}\n",out);
    }
    free(response);free(origin);free(url);free(method);free(ca);return error;
}
