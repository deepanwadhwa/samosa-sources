#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "fetch_public.h"
#include "fetch.h"
#include "native.h"
#include "json.h"
#include "sha256.h"
#include <arpa/inet.h>
#include <assert.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
static double monotonic(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
typedef struct {const char *url;double finished;const char *error;} RateAttempt;
static void *rate_thread(void *context){RateAttempt *attempt=context;SourcesConfig cfg={0};attempt->error=sources_fetch_rate_wait(&cfg,attempt->url);attempt->finished=monotonic();return NULL;}
static int cancelled_rate(void *context){(void)context;return 1;}
static void rate_controls(void){
    RateAttempt attempts[]={{.url="https://rate.example"},{.url="https://rate.example:443"},{.url="https://independent.example"}};pthread_t threads[3];
    for(size_t i=0;i<3;i++)assert(!pthread_create(&threads[i],NULL,rate_thread,&attempts[i]));
    for(size_t i=0;i<3;i++)assert(!pthread_join(threads[i],NULL)&&!attempts[i].error);
    double distance=attempts[0].finished-attempts[1].finished;if(distance<0)distance=-distance;assert(distance>=0.9);
    double later=attempts[0].finished>attempts[1].finished?attempts[0].finished:attempts[1].finished;assert(attempts[2].finished<later-0.5);
    SourcesConfig cfg={.deadline_monotonic=monotonic()+0.05};assert(!strcmp(sources_fetch_rate_wait(&cfg,"https://rate.example"),"deadline_exceeded"));
    cfg.deadline_monotonic=0;cfg.cancelled=cancelled_rate;assert(!strcmp(sources_fetch_rate_wait(&cfg,"https://rate.example"),"cancelled"));
    puts("PASS parent host politeness: concurrent same-host reservations, equivalent explicit/default ports, independent hosts and cancellation/deadline refusal");
}
static void public_web_rules(void){
    struct {const char *base,*location,*expected;} joins[]={
        {"https://example.com/a/page?old=1","next","https://example.com/a/next"},
        {"https://example.com/a/page","/root","https://example.com/root"},
        {"https://example.com/a/page","https://other.example/path","https://other.example/path"},
        {"https://example.com/a/page","//other.example/path","https://other.example/path"},
        {"http://example.com/a/page","//other.example/path","http://other.example/path"},
        {"https://example.com/a/page?old=1","?new=2","https://example.com/a/page?new=2"},
        {"https://example.com","next","https://example.com/next"},
        {"https://[2606:4700:4700::1111]/a/page","next","https://[2606:4700:4700::1111]/a/next"},
        {"https://example.com:443/a/page","next","https://example.com:443/a/next"},
        {"https://example.com/a/page","../next","https://example.com/a/../next"}
    };
    char out[SOURCES_FETCH_URL_MAX+1],origin[257];
    for(size_t i=0;i<sizeof(joins)/sizeof(*joins);i++){
        assert(!sources_fetch_redirect_url(joins[i].base,joins[i].location,out));assert(!strcmp(out,joins[i].expected));
    }
    const char *bad[]={"","https://user@other.example/path","https://other.example:8443/path","https://other.example/path#fragment","https://other.example/%xy","/bad\\path","/bad\r\nheader","https://[fe80::1%25lo]/",NULL};
    for(size_t i=0;bad[i];i++)assert(sources_fetch_redirect_url("https://example.com/",bad[i],out)&&!out[0]);
    assert(sources_fetch_redirect_url("https://example.com/",NULL,out)&&!out[0]);
    char long_location[SOURCES_FETCH_URL_MAX+2];memset(long_location,'a',sizeof(long_location)-1);long_location[sizeof(long_location)-1]=0;
    assert(sources_fetch_redirect_url("https://example.com/",long_location,out)&&!out[0]);
    assert(!sources_fetch_url_origin("https://[2606:4700:4700::1111]:443/a?x=1",origin)&&!strcmp(origin,"https://[2606:4700:4700::1111]:443"));
    const char *wild="User-agent: *\nDisallow: /private\nAllow: /private/public\n";
    assert(!sources_fetch_robots_allowed(wild,"samosa","/private/file"));
    assert(sources_fetch_robots_allowed(wild,"samosa","/private/public/file"));
    assert(sources_fetch_robots_allowed(wild,"samosa","/other"));
    assert(sources_fetch_robots_allowed("User-agent: *\nDisallow: /same\nAllow: /same\n","samosa","/same/file"));
    assert(sources_fetch_robots_allowed("User-agent: *\nDisallow:\n","samosa","/anything"));
    const char *specific="User-agent: *\nDisallow: /\nUser-agent: samosa\nDisallow: /blocked\nUser-agent: other\nDisallow: /elsewhere\n";
    assert(sources_fetch_robots_allowed(specific,"samosa","/normal"));
    assert(!sources_fetch_robots_allowed(specific,"samosa","/blocked"));
    assert(sources_fetch_robots_allowed(specific,"samosa","/elsewhere"));
    assert(!sources_fetch_robots_allowed(specific,"unknown","/normal"));
    assert(!sources_fetch_robots_allowed("# comment\r\n\tUser-Agent: samosa\r\nUser-Agent: alternate\r\n Disallow: /secret \r\n","samosa","/secret/page"));
    assert(sources_fetch_robots_allowed("User-agent: other\nDisallow: /\n","samosa","/normal"));
    assert(sources_fetch_robots_allowed("","samosa","/normal"));
    assert(!sources_fetch_robots_allowed(NULL,"samosa","/normal"));
    SourcesFetchRoute route;SourcesConfig grant={.fetch_origin="https://example.com",.fetch_helper="worker",.fetch_public_only=1};
    assert(!sources_fetch_route_begin(&grant,"https://example.com/start",&route));
    assert(!sources_fetch_route_next("https://other.example/second",&route));
    assert(route.count==2&&!strcmp(route.urls[0],"https://example.com/start")&&!strcmp(route.urls[1],"https://other.example/second"));
    assert(!strcmp(sources_fetch_route_next("https://example.com/start",&route),"fetch_redirect_loop")&&route.count==2);
    assert(sources_fetch_route_next("https://user@other.example/path",&route)&&route.count==2);
    for(size_t i=2;i<SOURCES_FETCH_MAX_HOPS;i++){char location[32];snprintf(location,sizeof(location),"/hop-%zu",i);assert(!sources_fetch_route_next(location,&route));}
    assert(route.count==SOURCES_FETCH_MAX_HOPS);
    assert(!strcmp(sources_fetch_route_next("/one-too-many",&route),"fetch_redirect_limit")&&route.count==SOURCES_FETCH_MAX_HOPS);
    grant.fetch_offline=1;assert(!strcmp(sources_fetch_route_begin(&grant,"https://example.com/start",&route),"fetch_offline")&&!route.count);
    grant.fetch_offline=0;grant.fetch_public_only=0;assert(sources_fetch_route_begin(&grant,"https://example.com/start",&route)&&!route.count);
    grant.fetch_public_only=1;assert(sources_fetch_route_begin(&grant,"https://ungranted.example/start",&route)&&!route.count);
    puts("PASS extracted public-web rules: bounded exact-origin/IPv6/relative redirect joins, malformed destination refusal, monolith specific/wildcard robots groups and longest allow/disallow matching");
}
static void addresses(void){
    const char *blocked[]={"0.0.0.0","0.255.255.255","10.0.0.0","10.255.255.255","100.64.0.0","100.127.255.255","127.0.0.1","169.254.0.0","172.16.0.0","172.31.255.255","192.0.0.0","192.0.0.255","192.168.1.2","198.18.0.0","198.19.255.255","224.0.0.0","255.255.255.255","::","::1","fc00::1","fdff::1","fe80::1","febf::1","::ffff:8.8.8.8","64:ff9b::808:808","2002:808:808::1","garbage",NULL};
    const char *allowed[]={"8.8.8.8","1.1.1.1","100.63.255.255","100.128.0.0","172.15.255.255","172.32.0.0","192.0.1.0","198.17.255.255","198.20.0.0","223.255.255.255","2606:4700:4700::1111",NULL};
    for(size_t i=0;blocked[i];i++)assert(sources_fetch_address_blocked(blocked[i]));
    for(size_t i=0;allowed[i];i++)assert(!sources_fetch_address_blocked(allowed[i]));
    assert(sources_fetch_address_blocked(NULL));
    assert(sources_fetch_address_equal("2606:4700:4700::1111","2606:4700:4700:0:0:0:0:1111"));
    assert(!sources_fetch_address_equal("8.8.8.8","8.8.4.4"));
    struct sockaddr_in public4={.sin_family=AF_INET},private4={.sin_family=AF_INET};
    struct sockaddr_in6 public6={.sin6_family=AF_INET6};
    assert(inet_pton(AF_INET,"8.8.8.8",&public4.sin_addr)==1&&inet_pton(AF_INET,"127.0.0.1",&private4.sin_addr)==1);
    assert(inet_pton(AF_INET6,"2606:4700:4700::1111",&public6.sin6_addr)==1);
    struct addrinfo third={.ai_family=AF_INET,.ai_addr=(struct sockaddr*)&private4,.ai_addrlen=sizeof(private4)};
    struct addrinfo second={.ai_family=AF_INET6,.ai_addr=(struct sockaddr*)&public6,.ai_addrlen=sizeof(public6)};
    struct addrinfo first={.ai_family=AF_INET,.ai_addr=(struct sockaddr*)&public4,.ai_addrlen=sizeof(public4),.ai_next=&second};
    char ip[SOURCES_FETCH_ADDRESS_MAX];assert(!sources_fetch_addresses(&first,ip)&&!strcmp(ip,"8.8.8.8"));
    second.ai_next=&third;assert(!strcmp(sources_fetch_addresses(&first,ip),"fetch_nonpublic_address")&&!ip[0]);
    third.ai_next=&first;second.ai_next=NULL;assert(!strcmp(sources_fetch_addresses(&third,ip),"fetch_nonpublic_address")&&!ip[0]);
    third.ai_next=NULL;third.ai_addrlen=1;second.ai_next=&third;
    assert(!strcmp(sources_fetch_addresses(&first,ip),"fetch_dns_failed")&&!ip[0]);
    assert(!strcmp(sources_fetch_addresses(NULL,ip),"fetch_dns_failed")&&!ip[0]);
}
static void route_responses(void){
    SourcesConfig cfg={.fetch_origin="https://example.com",.fetch_helper="worker",.fetch_public_only=1,.fetch_public_web=1};
    char digest[65];hash_bytes("x",1,digest);
    const char *histories[]={"[\"https://example.com/start\",\"https://other.example/final\"]","[]","[\"https://ungranted.example/start\"]","[\"https://example.com/start\",\"https://example.com/start\"]","[\"https://example.com/start\",\"https://user@other.example/final\"]","[\"https://example.com/start\",\"https://other.example:8443/final\"]","[\"https://example.com/start\",\"https://a.example/1\",\"https://a.example/2\",\"https://a.example/3\",\"https://a.example/4\",\"https://a.example/5\",\"https://a.example/6\"]"};
    for(size_t i=0;i<sizeof(histories)/sizeof(*histories);i++){
        char body[2048];snprintf(body,sizeof(body),"{\"url\":\"https://example.com/start\",\"status\":200,\"bytes\":1,\"digest\":\"%s\",\"data\":\"eA==\",\"urls\":%s}",digest,histories[i]);
        unsigned char *bytes=NULL;size_t n=0;char *final=NULL,*urls=NULL;
        const char *error=sources_fetch_validate_route(&cfg,"https://example.com/start",body,strlen(body),&bytes,&n,&final,&urls);
        if(!i){assert(!error&&n==1&&bytes[0]=='x'&&!strcmp(final,"https://other.example/final")&&!strcmp(urls,histories[i]));free(bytes);free(final);free(urls);}
        else assert(error&&!bytes&&!n&&!final&&!urls);
    }
    puts("PASS independent redirect response validation: exact initial grant, retained final URL/history, malformed/cyclic/oversize history refusal and original digest/base64 checks");
}
static void refusal(SourcesConfig *cfg,const char *url,const char *expected){
    unsigned char *bytes=(void*)1;size_t n=99;const char *error=sources_native_fetch(cfg,url,&bytes,&n);
    if(!error||strcmp(error,expected))fprintf(stderr,"expected %s, got %s\n",expected,error?error:"success");
    assert(error&&!strcmp(error,expected)&&!bytes&&!n);
}
static void local_ipv6(const char *worker){
    int fd=socket(AF_INET6,SOCK_STREAM,0);assert(fd>=0);
    struct sockaddr_in6 address={.sin6_family=AF_INET6,.sin6_addr=IN6ADDR_LOOPBACK_INIT};
    assert(!bind(fd,(struct sockaddr*)&address,sizeof(address))&&!listen(fd,1));socklen_t size=sizeof(address);
    assert(!getsockname(fd,(struct sockaddr*)&address,&size));pid_t server=fork();assert(server>=0);
    if(!server){
        alarm(15);int peer=accept(fd,NULL,NULL);assert(peer>=0);char request[4096]={0};size_t n=0;
        while(!strstr(request,"\r\n\r\n")){ssize_t got=read(peer,request+n,sizeof(request)-1-n);assert(got>0);n+=(size_t)got;assert(n<sizeof(request)-1);}
        assert(!strncmp(request,"GET /literal HTTP/",18));
        const char response[]="HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nliteral";
        size_t sent=0;while(sent<sizeof(response)-1){ssize_t wrote=write(peer,response+sent,sizeof(response)-1-sent);assert(wrote>0);sent+=(size_t)wrote;}
        close(peer);close(fd);_exit(0);
    }
    close(fd);char origin[128],url[160];snprintf(origin,sizeof(origin),"http://[::1]:%u",ntohs(address.sin6_port));snprintf(url,sizeof(url),"%s/literal",origin);
    SourcesConfig cfg={.fetch_origin=origin,.fetch_helper=worker};unsigned char *bytes=NULL;size_t n=0;
    const char *error=sources_native_fetch(&cfg,url,&bytes,&n);if(error)fprintf(stderr,"IPv6 literal: %s\n",error);
    assert(!error&&n==7&&!memcmp(bytes,"literal",7));free(bytes);int status;
    assert(waitpid(server,&status,0)==server&&WIFEXITED(status)&&!WEXITSTATUS(status));
    puts("PASS actual IPv6 literal: explicit local grant, native worker, exact response and independent parent validation");
}
int main(int argc,char **argv){
    assert(argc==2||(argc==3&&!strcmp(argv[2],"--live")));rate_controls();route_responses();public_web_rules();addresses();local_ipv6(argv[1]);SourcesConfig cfg={.fetch_helper=argv[1],.fetch_public_only=1};
    cfg.fetch_origin="http://127.0.0.1";refusal(&cfg,"http://127.0.0.1/source","fetch_nonpublic_address");
    cfg.fetch_offline=1;refusal(&cfg,"http://127.0.0.1/source","fetch_offline");cfg.fetch_offline=0;
    cfg.fetch_origin="https://localhost";refusal(&cfg,"https://localhost/source","fetch_nonpublic_address");
    cfg.fetch_origin="https://[::1]";refusal(&cfg,"https://[::1]/source","fetch_nonpublic_address");
    cfg.fetch_origin="https://[::ffff:8.8.8.8]";refusal(&cfg,"https://[::ffff:8.8.8.8]/source","fetch_nonpublic_address");
    cfg.fetch_origin="https://[2606:4700:4700::1111]";assert(!sources_fetch_authorize(&cfg,"https://[2606:4700:4700::1111]/source"));
    assert(sources_fetch_authorize(&cfg,"https://[2606:4700:4700::1111].evil/source"));
    const char *bad[]={"https://[::1","https://[::1]extra","https://[::1]:0","https://[not-v6]","https://[fe80::1%25lo0]"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++){cfg.fetch_origin=bad[i];const char *error=sources_fetch_authorize(&cfg,bad[i]);if(!error)fprintf(stderr,"accepted malformed IPv6 origin: %s\n",bad[i]);assert(error);}
    cfg.fetch_origin="https://localhost:8443";refusal(&cfg,"https://localhost:8443/source","fetch_public_port");
    cfg.fetch_origin="http://example.test";assert(!sources_fetch_authorize(&cfg,"http://example.test/source"));
    cfg.fetch_public_only=0;assert(sources_fetch_authorize(&cfg,"http://example.test/source"));
    cfg.fetch_public_only=2;assert(!strcmp(sources_fetch_authorize(&cfg,"http://example.test/source"),"invalid_fetch_configuration"));
    cfg.fetch_public_only=1;cfg.fetch_origin="http://127.0.0.1";
    char state[]="/tmp/samosa-public-fetch-XXXXXX";assert(mkdtemp(state));cfg.state=state;
    const char *request="{\"contract_version\":1,\"op\":\"fetch\",\"url\":\"http://127.0.0.1/source\",\"format\":\"text\",\"acquisition_id\":\"public-refusal\"}";
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);assert(out);
    assert(sources_dispatch(&cfg,request,strlen(request),out)==1&&!fclose(out));
    assert(strstr(body,"fetch_nonpublic_address")&&!strstr(body,"source_id"));free(body);
    const char *override="{\"contract_version\":1,\"op\":\"fetch\",\"url\":\"http://127.0.0.1/source\",\"format\":\"text\",\"acquisition_id\":\"override\",\"fetch_public_only\":0}";
    out=open_memstream(&body,&n);assert(out);assert(sources_dispatch(&cfg,override,strlen(override),out)==1&&!fclose(out));assert(strstr(body,"unknown_field"));free(body);
    char database[512];snprintf(database,sizeof(database),"%s/sources.sqlite3",state);sqlite3 *db=NULL;sqlite3_stmt *stmt=NULL;
    assert(sqlite3_open(database,&db)==SQLITE_OK);assert(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM fetch_receipts)",-1,&stmt,NULL)==SQLITE_OK);
    assert(sqlite3_step(stmt)==SQLITE_ROW&&!sqlite3_column_int(stmt,0)&&!sqlite3_column_int(stmt,1));assert(sqlite3_finalize(stmt)==SQLITE_OK&&sqlite3_close(db)==SQLITE_OK);
    puts("PASS public fetch: monolith IPv4/IPv6 ranges, mixed-answer refusal, exact address identity, actual isolated DNS/port refusal, no acquisition artifacts and client policy override refusal");
    if(argc==3){
        cfg.fetch_origin="https://example.com";unsigned char *bytes=NULL;size_t length=0;
        const char *error=sources_native_fetch(&cfg,"https://example.com/",&bytes,&length);
        if(error)fprintf(stderr,"live pinned fetch: %s\n",error);
        assert(!error&&bytes&&length);free(bytes);
        puts("PASS actual public HTTPS: validated complete DNS answer set, pinned transport, TLS hostname/trust and connected peer identity");
        cfg.fetch_origin="https://httpbingo.org";cfg.fetch_public_web=1;
        refusal(&cfg,"https://httpbingo.org/deny","fetch_robots_denied");
        char *final=NULL,*urls=NULL;
        error=sources_native_fetch_capture(&cfg,"https://httpbingo.org/redirect/2",&bytes,&length,&final,&urls);
        if(error)fprintf(stderr,"live redirect fetch: %s\n",error);
        assert(!error&&bytes&&length&&final&&!strcmp(final,"https://httpbingo.org/get"));Json route;assert(json_parse(&route,urls,strlen(urls))&&route.tokens[0].count==3);
        free(bytes);free(final);free(urls);
        const char *capture="{\"contract_version\":1,\"op\":\"fetch\",\"url\":\"https://httpbingo.org/redirect/2\",\"format\":\"text\",\"acquisition_id\":\"redirect-live\"}";
        out=open_memstream(&body,&n);assert(out);assert(!sources_dispatch(&cfg,capture,strlen(capture),out)&&!fclose(out));
        assert(strstr(body,"\"final_url\":\"https://httpbingo.org/get\"")&&strstr(body,"\"urls\":["));
        cfg.fetch_offline=1;char *replay=NULL;size_t replay_length=0;out=open_memstream(&replay,&replay_length);assert(out);
        assert(!sources_dispatch(&cfg,capture,strlen(capture),out)&&!fclose(out));assert(n==replay_length&&!memcmp(body,replay,n));free(body);free(replay);
        puts("PASS actual bounded public redirects: per-hop DNS/pinning/TLS/peer checks, three destinations, final-URL acquisition receipt and exact offline replay");
        puts("PASS actual robots gate and parent rate protocol: live Disallow refusal and rate-controlled robots/page requests through bounded redirects");
    }
    return 0;
}
