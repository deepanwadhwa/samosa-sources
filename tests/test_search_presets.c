#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "search.h"
#include "json.h"
#include <assert.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sqlite3.h>
static const char *names[]={"parallel","brave","tavily","serpapi","google","searxng"};
static const char query[]="how \"quoted\"\n{api_key}&x=1";
static const char encoded[]="how%20%22quoted%22%0A%7Bapi_key%7D%26x%3D1";
static void send_all(int fd,const char *bytes,size_t n){while(n){ssize_t sent=send(fd,bytes,n,0);assert(sent>0);bytes+=sent;n-=(size_t)sent;}}
static void text(const Json *j,int object,const char *key,const char *expected){char *value=json_string(j,json_get(j,object,key));assert(value&&!strcmp(value,expected));free(value);}
static pid_t fixture(unsigned provider,unsigned mode,unsigned short *port){
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    assert(!bind(fd,(struct sockaddr*)&address,sizeof(address))&&!listen(fd,1));socklen_t size=sizeof(address);assert(!getsockname(fd,(struct sockaddr*)&address,&size));*port=ntohs(address.sin_port);
    pid_t child=fork();assert(child>=0);if(child){close(fd);return child;}
    alarm(15);signal(SIGPIPE,SIG_IGN);int peer=accept(fd,NULL,NULL);assert(peer>=0);close(fd);
    char request[16384]={0};size_t n=0;char *end=NULL;
    while(!(end=strstr(request,"\r\n\r\n"))){ssize_t got=recv(peer,request+n,sizeof(request)-1-n,0);assert(got>0);n+=(size_t)got;assert(n<sizeof(request)-1);}
    char target[2048];
    if(provider==0)snprintf(target,sizeof(target),"POST /mcp HTTP/");
    else if(provider==1)snprintf(target,sizeof(target),"GET /res/v1/web/search?q=%s&count=8 HTTP/",encoded);
    else if(provider==2)snprintf(target,sizeof(target),"POST /search HTTP/");
    else if(provider==3)snprintf(target,sizeof(target),"GET /search.json?engine=google&q=%s&api_key=credential%%2Fsecret HTTP/",encoded);
    else if(provider==4)snprintf(target,sizeof(target),"GET /customsearch/v1?key=credential%%2Fsecret&cx=engine%%2Fid&q=%s HTTP/",encoded);
    else snprintf(target,sizeof(target),"GET /search?q=%s&format=json HTTP/",encoded);
    assert(!strncmp(request,target,strlen(target)));assert(!strstr(request,"Authorization:"));
    if(provider==1)assert(strstr(request,"X-Subscription-Token: credential/secret\r\n"));else assert(!strstr(request,"X-Subscription-Token:"));
    if(provider==0||provider==2){
        assert(strstr(request,"Content-Type: application/json\r\n"));char *length=strstr(request,"Content-Length: ");assert(length);size_t declared=strtoul(length+16,NULL,10),header=(size_t)(end-request)+4;assert(declared<sizeof(request)-header);
        while(n<header+declared){ssize_t got=recv(peer,request+n,sizeof(request)-1-n,0);assert(got>0);n+=(size_t)got;}
        Json body;assert(json_parse(&body,request+header,declared));
        if(provider==2){assert(body.tokens[0].count==3);text(&body,0,"query",query);text(&body,0,"api_key","credential/secret");size_t maximum=0;assert(json_uint(&body,json_get(&body,0,"max_results"),&maximum)&&maximum==8);}
        else{
            assert(body.tokens[0].count==4);text(&body,0,"jsonrpc","2.0");text(&body,0,"method","tools/call");size_t id=0;assert(json_uint(&body,json_get(&body,0,"id"),&id)&&id==1);
            int params=json_get(&body,0,"params");text(&body,params,"name","web_search");int arguments=json_get(&body,params,"arguments");assert(body.tokens[arguments].count==3);text(&body,arguments,"objective",query);
            int queries=json_get(&body,arguments,"search_queries");assert(body.tokens[queries].type==JT_ARRAY&&body.tokens[queries].count==1);char *q=json_string(&body,queries+1);assert(q&&!strcmp(q,query));free(q);
            char *session=json_string(&body,json_get(&body,arguments,"session_id"));assert(session&&strlen(session)==32&&strspn(session,"0123456789abcdef")==32);free(session);
            assert(strstr(request,"Accept: application/json, text/event-stream\r\n"));
        }
    }else{assert(!strstr(request,"Content-Length:"));assert(strstr(request,"Accept: application/json\r\n"));}
    const char *replies[]={
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"isError\":false,\"structuredContent\":{\"results\":[{\"title\":\"Parallel title\",\"url\":\"https://example.test/source\",\"excerpts\":[\"first\",\"second\"]}]}}}",
        "{\"web\":{\"results\":[{\"title\":\"Brave title\",\"url\":\"https://example.test/source\",\"description\":\"credential/secret Brave description\"}]}}",
        "{\"results\":[{\"title\":\"Tavily title\",\"url\":\"https://example.test/source\",\"content\":\"credential/secret Tavily description\"}]}",
        "{\"organic_results\":[{\"title\":\"SerpAPI title\",\"link\":\"https://example.test/source\",\"snippet\":\"credential/secret SerpAPI description\"}]}",
        "{\"items\":[{\"title\":\"Google title\",\"link\":\"https://example.test/source\",\"snippet\":\"credential/secret Google description\"}]}",
        "{\"results\":[{\"title\":\"SearxNG title\",\"url\":\"https://example.test/source\",\"content\":\"SearxNG description\"}]}"
    };
    const char *body=mode==1?"{\"results\":\"malformed\"}":mode==3?"{\"error\":{\"message\":\"credential/secret\"}}":replies[provider];
    char response[256];int count=snprintf(response,sizeof(response),"HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",mode==2?"302 Found":"200 OK",strlen(body),mode==2?"Location: http://127.0.0.1:1/credential-leak\r\n":"");
    send_all(peer,response,(size_t)count);send_all(peer,body,strlen(body));close(peer);_exit(0);
}
int main(int argc,char **argv){
    assert(argc==2);signal(SIGPIPE,SIG_IGN);char state[]="/tmp/samosa-search-presets-XXXXXX";assert(mkdtemp(state));SourcesConfig cfg={.state=state,.owner="preset-owner",.fetch_helper=argv[1]};
    char *request=NULL;size_t request_length=0;FILE *out=open_memstream(&request,&request_length);assert(out);fputs("{\"contract_version\":1,\"op\":\"search\",\"query\":",out);json_quote(out,query,strlen(query));fputc('}',out);assert(!fclose(out));
    for(unsigned provider=0;provider<6;provider++)for(unsigned mode=0;mode<4;mode++){
        unsigned short port;pid_t child=fixture(provider,mode,&port);SourcesSearchConfig preset;assert(!sources_search_preset(names[provider],&preset));
        char origin[128],url[2048],values[512];snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);
        if(provider==5){preset.origin=origin;snprintf(values,sizeof(values),"{\"base_url\":\"%s\"}",origin);}
        else{snprintf(url,sizeof(url),"%s%s",origin,preset.url_template+strlen(preset.origin));preset.origin=origin;preset.url_template=url;snprintf(values,sizeof(values),provider==0?"{}":"{\"api_key\":\"credential/secret\",\"cx\":\"engine/id\"}");}
        preset.values=values;cfg.search=&preset;char *body=NULL;size_t n=0;out=open_memstream(&body,&n);assert(out);int result=sources_dispatch(&cfg,request,request_length,out);assert(!fclose(out));
        if(result!=(mode?1:0)){fprintf(stderr,"provider %s mode %u: %s\n",names[provider],mode,body);}
        assert(result==(mode?1:0));assert(!strstr(body,"credential/secret")&&!strstr(body,"credential%2Fsecret"));
        if(mode)assert(strstr(body,mode==2?"fetch_http_status":"search_response_invalid"));
        else{
            Json reply;assert(json_parse(&reply,body,n));int search=json_get(&reply,0,"search"),items=json_get(&reply,search,"items");text(&reply,search,"provider",names[provider]);assert(reply.tokens[items].type==JT_ARRAY&&reply.tokens[items].count==1);text(&reply,items+1,"url","https://example.test/source");
            if(provider==0)text(&reply,items+1,"description","first\nsecond");else if(provider!=5)assert(strstr(body,"<redacted>"));
        }
        free(body);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    free(request);char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;sqlite3_stmt *stmt=NULL;assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT count(*) FROM snapshots",-1,&stmt,NULL)==SQLITE_OK);assert(sqlite3_step(stmt)==SQLITE_ROW&&sqlite3_column_int(stmt,0)==6);assert(sqlite3_finalize(stmt)==SQLITE_OK&&sqlite3_close(db)==SQLITE_OK);
    puts("PASS six search presets: actual exact GET/POST queries, provider credentials/headers/body/session, provider-specific reply mappings, redaction, malformed/provider-error/redirect refusal and immutable admission bounds");return 0;
}
