#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "search.h"
#include "fetch.h"
#include "json.h"
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sqlite3.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);abort();}}while(0)
static const char *query="how \"quoted\"\n{api_key}&x=1";
static void send_all(int fd,const void *bytes,size_t n){const char *p=bytes;while(n){ssize_t sent=send(fd,p,n,0);if(sent<=0)_exit(0);p+=sent;n-=(size_t)sent;}}
static pid_t fixture(unsigned short *port,int mode){
    int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};CHECK(!bind(fd,(struct sockaddr *)&address,sizeof(address)));CHECK(!listen(fd,1));socklen_t size=sizeof(address);CHECK(!getsockname(fd,(struct sockaddr *)&address,&size));*port=ntohs(address.sin_port);pid_t child=fork();CHECK(child>=0);
    if(!child){
        signal(SIGPIPE,SIG_IGN);alarm(15);int peer=accept(fd,NULL,NULL);CHECK(peer>=0);close(fd);char request[16384]={0};size_t offset=0;char *end=NULL;
        while(!(end=strstr(request,"\r\n\r\n"))){ssize_t got=recv(peer,request+offset,sizeof(request)-1-offset,0);CHECK(got>0);offset+=(size_t)got;CHECK(offset<sizeof(request)-1);}
        CHECK(!strncmp(request,"POST /search ",13));CHECK(strstr(request,"Authorization: Bearer credential/secret\r\n"));
        char *length=strstr(request,"Content-Length: ");CHECK(length);size_t n=strtoul(length+16,NULL,10);size_t header=(size_t)(end-request)+4;CHECK(n<sizeof(request)-header);
        while(offset<header+n){ssize_t got=recv(peer,request+offset,sizeof(request)-1-offset,0);CHECK(got>0);offset+=(size_t)got;}
        Json posted;CHECK(json_parse(&posted,request+header,n));char *received=json_string(&posted,json_get(&posted,0,"query"));CHECK(received&&!strcmp(received,query));free(received);
        char *body=NULL;size_t body_size=0;FILE *out=open_memstream(&body,&body_size);CHECK(out);
        if(mode==4)fputs("not json",out);
        else if(mode==10){fputs("{\"results\":[{\"title\":\"large\",\"url\":\"https://example.test/source\",\"description\":\"",out);for(size_t i=0;i<SOURCES_SEARCH_HTTP_MAX;i++)fputc('x',out);fputs("\"}]}",out);}
        else if(mode==1||mode==8){fprintf(out,"event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"isError\":false,\"structuredContent\":{\"results\":[{\"title\":\"SSE result\",\"url\":\"https://example.test/sse\",\"excerpts\":[\"first\",\"second\"]}]}}}\n\n",mode==8?2:1);}
        else{
            if(mode!=7)fputs(mode==9?"{\"error\":{\"message\":\"credential/secret\"},\"results\":[":"{\"results\":[",out);else fputc('[',out);
            for(int i=0;i<(mode==0?10:1);i++){
                if(i)fputc(',',out);
                fprintf(out,"{\"title\":\"credential/secret result %d\",\"url\":\"%s\",\"description\":\"",i,mode==3?"javascript:alert(1)":mode==5?"https://example.test/credential%2fsecret":"https://example.test/source");
                fputs("credential/secret ",out);for(int j=0;j<450;j++)fputs("é",out);fputs("\"}",out);
            }
            fputs(mode==7?"]":"]}",out);
        }
        CHECK(!fclose(out));char response[512];int count=snprintf(response,sizeof(response),"HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",mode==2?"302 Found":mode==6?"500 Failure":"200 OK",body_size,mode==2?"Location: http://127.0.0.1:1/leak\r\n":"");send_all(peer,response,(size_t)count);send_all(peer,body,body_size);free(body);close(peer);_exit(0);
    }
    close(fd);return child;
}
static char *dispatch(SourcesConfig *cfg,const char *request,int expected){char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);int result=sources_dispatch(cfg,request,strlen(request),out);CHECK(!fclose(out));if(result!=expected)fprintf(stderr,"search reply: %s\n",body);CHECK(result==expected);CHECK(!strstr(body,"credential/secret"));CHECK(!strstr(body,"credential%2fsecret"));return body;}
int main(int argc,char **argv){
    CHECK(argc==2);signal(SIGPIPE,SIG_IGN);char state[]="/tmp/samosa-search-XXXXXX";CHECK(mkdtemp(state));SourcesConfig cfg={.state=state,.owner="search-owner",.fetch_helper=argv[1]};char origin[128],url[256],*saved=NULL,source[65]={0};
    SourcesSearchConfig c={.provider="fixture",.headers="{\"Authorization\":\"Bearer {api_key}\",\"Content-Type\":\"application/json\"}",.body="{\"query\":\"{query}\"}",.values="{\"api_key\":\"credential/secret\"}",.results_path="results",.title_path="title",.url_path="url",.description_path="description"};cfg.search=&c;
    char *request=NULL;size_t request_size=0;FILE *out=open_memstream(&request,&request_size);CHECK(out);fputs("{\"contract_version\":1,\"op\":\"search\",\"query\":",out);json_quote(out,query,strlen(query));fputc('}',out);CHECK(!fclose(out));
    for(int mode=0;mode<11;mode++){
        fprintf(stderr,"search fixture mode %d\n",mode);unsigned short port;pid_t server=fixture(&port,mode);snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);snprintf(url,sizeof(url),"%s/search",origin);c.origin=origin;c.url_template=url;c.provider=mode==1||mode==8?"parallel":"fixture";c.results_path=mode==1||mode==8?"result.structuredContent.results":mode==7?"":"results";c.description_path=mode==1||mode==8?"excerpts":"description";
        char *body=dispatch(&cfg,request,mode==0||mode==1||mode==7?0:1);
        if(mode==0){Json j;CHECK(json_parse(&j,body,strlen(body)));int search=json_get(&j,0,"search"),items=json_get(&j,search,"items");CHECK(items>=0&&j.tokens[items].count==8);CHECK(strstr(body,"<redacted>"));CHECK(strstr(body,"provider_search_metadata"));char *id=json_string(&j,json_get(&j,0,"source_id"));CHECK(id&&strlen(id)==64);memcpy(source,id,65);free(id);}
        if(mode==1){Json j;CHECK(json_parse(&j,body,strlen(body)));int search=json_get(&j,0,"search"),items=json_get(&j,search,"items");CHECK(items>=0&&j.tokens[items].count==1);char *description=json_string(&j,json_get(&j,items+1,"description"));CHECK(description&&!strcmp(description,"first\nsecond"));free(description);}
        if(mode==2||mode==6)CHECK(strstr(body,"fetch_http_status"));
        if(mode==10)CHECK(strstr(body,"fetch_too_large"));
        if(mode==3||mode==4||mode==5||mode==8||mode==9)CHECK(strstr(body,"search_response_invalid"));
        free(body);int status;CHECK(waitpid(server,&status,0)==server);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    cfg.search=NULL;char read_request[512];snprintf(read_request,sizeof(read_request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":100}",source,source);char *body=dispatch(&cfg,read_request,0);CHECK(strstr(body,"search:fixture"));free(body);
    cfg.owner="other-owner";body=dispatch(&cfg,read_request,1);CHECK(strstr(body,"unknown_source"));free(body);cfg.owner="search-owner";cfg.search=&c;c.provider="fixture";c.results_path="results";c.description_path="description";
    c.values="{}";body=dispatch(&cfg,request,1);CHECK(strstr(body,"search_credentials_missing"));free(body);c.values="{\"api_key\":\"credential/secret\"}";
    c.headers="{\"Authorization\":\"Bearer bad\\r\\nInjected: bad\"}";body=dispatch(&cfg,request,1);CHECK(strstr(body,"invalid_fetch_configuration"));free(body);
    c.headers="{\"Host\":\"evil.test\"}";body=dispatch(&cfg,request,1);CHECK(strstr(body,"invalid_fetch_configuration"));free(body);
    body=dispatch(&cfg,"{\"contract_version\":1,\"op\":\"search\",\"query\":\"query\",\"values\":{\"api_key\":\"client\"}}",1);CHECK(strstr(body,"unknown_field"));free(body);
    const char *presets[]={"parallel","brave","tavily","serpapi","google","searxng"};for(size_t i=0;i<6;i++){SourcesSearchConfig preset;CHECK(!sources_search_preset(presets[i],&preset));CHECK(preset.url_template&&preset.results_path);}SourcesSearchConfig unknown;CHECK(sources_search_preset("unknown",&unknown));
    unsigned short port;pid_t server=fixture(&port,7);snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",port);snprintf(url,sizeof(url),"%s/search",origin);c.origin=origin;c.url_template=url;c.results_path="";c.headers="{\"Authorization\":\"Bearer {api_key}\",\"Content-Type\":\"application/json\"}";
    char *controlled=NULL;size_t length=0;out=open_memstream(&controlled,&length);CHECK(out);fwrite(request,1,request_size-1,out);fputs(",\"operation_id\":\"search-attempt\",\"parent_operation_id\":\"search-parent\",\"timeout_ms\":5000}",out);CHECK(!fclose(out));saved=dispatch(&cfg,controlled,0);CHECK(strstr(saved,"search-attempt"));CHECK(strstr(saved,"search-parent"));int status;CHECK(waitpid(server,&status,0)==server);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    cfg.search=NULL;cfg.fetch_helper=NULL;body=dispatch(&cfg,controlled,0);CHECK(!strcmp(saved,body));free(body);free(saved);free(controlled);free(request);
    char path[512];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);sqlite3_stmt *s=NULL;CHECK(sqlite3_prepare_v2(db,"SELECT count(*) FROM snapshots",-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_ROW);CHECK(sqlite3_column_int(s,0)==4);sqlite3_finalize(s);CHECK(sqlite3_close(db)==SQLITE_OK);
    printf("PASS search real POST/JSON/SSE/escaping/redaction/limits/ownership/offline operations: %u checks\n",checks);return 0;
}
