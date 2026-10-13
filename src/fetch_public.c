#define _POSIX_C_SOURCE 200809L
#include "fetch_public.h"
#include "fetch.h"
#include "control.h"
#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
/* Monolith public_rate_wait: one-second slots,64 host:port identities, reserve
 * under lock and sleep unlocked. The Sources parent owns this shared table;
 * isolated workers request reservations over their existing private channel. */
static pthread_mutex_t rate_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {char key[300];double last;} rates[64];
static double rate_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (double)t.tv_sec+(double)t.tv_nsec/1e9;}
const char *sources_fetch_rate_wait(const SourcesConfig *cfg,const char *url){
    char origin[257],key[300],literal[46];const char *error=sources_fetch_url_origin(url,origin);if(error)return error;
    size_t scheme=!strncmp(origin,"https://",8)?8:7;char *host=origin+scheme,*colon=NULL;
    unsigned port=scheme==8?443:80;
    if(*host=='['){char *end=strchr(host,']');if(end[1])colon=end+1;*end=0;host++;struct in6_addr address;
        if(inet_pton(AF_INET6,host,&address)!=1||!inet_ntop(AF_INET6,&address,literal,sizeof(literal)))return "invalid_fetch_configuration";
        host=literal;
    }else{colon=strchr(host,':');if(colon)*colon=0;}
    if(colon)port=(unsigned)strtoul(colon+1,NULL,10);
    snprintf(key,sizeof(key),"%s:%u",host,port);
    error=sources_interrupted(cfg);if(error)return error;double current=rate_now();if(current<0)return "state_unavailable";
    pthread_mutex_lock(&rate_lock);size_t slot=0,empty=64;int found=0;
    for(size_t i=0;i<64;i++){if(!rates[i].key[0]&&empty==64)empty=i;if(!strcmp(rates[i].key,key)){slot=i;found=1;break;}}
    if(!found){slot=empty<64?empty:0;rates[slot].last=0;}
    double ready=rates[slot].last+1.0;if(ready<current)ready=current;
    if(cfg&&cfg->deadline_monotonic&&ready>=cfg->deadline_monotonic){pthread_mutex_unlock(&rate_lock);return "deadline_exceeded";}
    memcpy(rates[slot].key,key,strlen(key)+1);rates[slot].last=ready;pthread_mutex_unlock(&rate_lock);
    while(current<ready){error=sources_interrupted(cfg);if(error)return error;struct timespec pause={0,10000000};nanosleep(&pause,NULL);current=rate_now();if(current<0)return "state_unavailable";}
    return sources_interrupted(cfg);
}
const char *sources_fetch_route_begin(const SourcesConfig *cfg,const char *url,SourcesFetchRoute *route){
    memset(route,0,sizeof(*route));
    const char *error=sources_fetch_authorize(cfg,url);
    if(!error&&cfg->fetch_public_only!=1)error="invalid_fetch_configuration";
    char origin[257];if(!error)error=sources_fetch_url_origin(url,origin);
    if(!error){memcpy(route->urls[0],url,strlen(url)+1);route->count=1;}
    return error;
}
const char *sources_fetch_route_next(const char *location,SourcesFetchRoute *route){
    if(!route->count||route->count>SOURCES_FETCH_MAX_HOPS)return "invalid_fetch_configuration";
    if(route->count==SOURCES_FETCH_MAX_HOPS)return "fetch_redirect_limit";
    char next[SOURCES_FETCH_URL_MAX+1];
    const char *error=sources_fetch_redirect_url(route->urls[route->count-1],location,next);
    if(error)return error;
    for(size_t i=0;i<route->count;i++)if(!strcmp(route->urls[i],next))return "fetch_redirect_loop";
    memcpy(route->urls[route->count],next,strlen(next)+1);route->count++;
    return NULL;
}
/* Adapted from the monolith url_parse/url_join. Keep exact authority spelling
 * and enforce the worker's bounded grammar before any destination is resolved. */
const char *sources_fetch_url_origin(const char *url,char origin[257]){
    origin[0]=0;
    if(!url||strlen(url)>SOURCES_FETCH_URL_MAX)return "unauthorized_url";
    size_t scheme=!strncmp(url,"https://",8)?8:!strncmp(url,"http://",7)?7:0;
    if(!scheme)return "unauthorized_url";
    size_t n=scheme+strcspn(url+scheme,"/?");
    if(n>256)return "unauthorized_url";
    memcpy(origin,url,n);origin[n]=0;
    SourcesConfig grant={.fetch_origin=origin,.fetch_helper="worker",.fetch_public_only=1};
    const char *error=sources_fetch_authorize(&grant,url);
    if(!error){
        const char *host=origin+scheme,*colon=*host=='['?strchr(host,']')+1:strchr(host,':');
        if(colon&&*colon==':'&&(unsigned)strtoul(colon+1,NULL,10)!=(scheme==8?443u:80u))error="fetch_public_port";
    }
    if(error)origin[0]=0;
    return error;
}
const char *sources_fetch_redirect_url(const char *base,const char *location,char out[SOURCES_FETCH_URL_MAX+1]){
    out[0]=0;char origin[257];const char *error=sources_fetch_url_origin(base,origin);
    if(error)return error;
    if(!location||!*location||strlen(location)>SOURCES_FETCH_URL_MAX)return "unauthorized_url";
    size_t prefix=0;const char *start=base;
    if(!strncmp(location,"http://",7)||!strncmp(location,"https://",8))start="";
    else if(!strncmp(location,"//",2))prefix=!strncmp(base,"https://",8)?6:5;
    else if(*location=='/')prefix=strlen(origin);
    else{
        size_t path_end=strcspn(base,"?");
        if(*location=='?')prefix=path_end;
        else{
            prefix=strlen(origin);
            for(size_t i=prefix;i<path_end;i++)if(base[i]=='/')prefix=i+1;
        }
    }
    int slash=start==base&&prefix==strlen(origin)&&*location!='/'&&*location!='?';
    size_t n=strlen(location);
    if(prefix+n+(size_t)slash>SOURCES_FETCH_URL_MAX)return "unauthorized_url";
    memcpy(out,start,prefix);if(slash)out[prefix++]='/';memcpy(out+prefix,location,n+1);
    char destination[257];error=sources_fetch_url_origin(out,destination);
    if(error)out[0]=0;
    return error;
}
/* Monolith robots_path_allowed: specific agent group, otherwise wildcard;
 * longest prefix wins and Allow wins equal-length matches. Preserve this
 * declared subset rather than introduce a different robots implementation. */
int sources_fetch_robots_allowed(const char *robots,const char *ua_token,const char *path){
    if(!robots||!ua_token||!*ua_token||!path)return 0;
    int have_specific=0;
    for(int pass=0;pass<2;pass++){
        const char *want=pass==0?ua_token:"*";
        int in_group=0,group_started=0,last_was_agent=0;
        long best_allow=-1,best_disallow=-1;
        for(const char *line=robots;line&&*line;){
            const char *eol=strchr(line,'\n');size_t len=eol?(size_t)(eol-line):strlen(line);
            while(len&&(line[len-1]=='\r'||line[len-1]==' '))len--;
            const char *p=line;size_t rest=len;
            while(rest&&(*p==' '||*p=='\t')){p++;rest--;}
            if(rest&&*p!='#'){
                if(rest>=11&&!strncasecmp(p,"user-agent:",11)){
                    const char *v=p+11;size_t vn=rest-11;
                    while(vn&&(*v==' '||*v=='\t')){v++;vn--;}
                    int matches=pass==0?(vn&&strlen(want)<=vn&&!strncasecmp(v,want,strlen(want))):(vn==1&&v[0]=='*');
                    if(!last_was_agent&&group_started)in_group=0;
                    if(matches){in_group=1;if(pass==0)have_specific=1;}
                    group_started=1;last_was_agent=1;
                }else{
                    last_was_agent=0;
                    if(in_group&&rest>=9&&!strncasecmp(p,"disallow:",9)){
                        const char *v=p+9;size_t vn=rest-9;
                        while(vn&&(*v==' '||*v=='\t')){v++;vn--;}
                        if(vn&&!strncmp(path,v,vn)&&(long)vn>best_disallow)best_disallow=(long)vn;
                    }else if(in_group&&rest>=6&&!strncasecmp(p,"allow:",6)){
                        const char *v=p+6;size_t vn=rest-6;
                        while(vn&&(*v==' '||*v=='\t')){v++;vn--;}
                        if(vn&&!strncmp(path,v,vn)&&(long)vn>best_allow)best_allow=(long)vn;
                    }
                }
            }
            line=eol?eol+1:NULL;
        }
        if(pass==0&&!have_specific)continue;
        return best_disallow<0||best_allow>=best_disallow;
    }
    return 1;
}
/* Extracted from samosa_gateway.c ipv4_blocked/ipv6_blocked/ip_blocked.
 * Preserve the monolith's address policy rather than substitute a new list. */
static int ipv4_blocked(uint32_t a){
    uint8_t o1=(uint8_t)(a>>24),o2=(uint8_t)(a>>16),o3=(uint8_t)(a>>8);
    if(o1==0||o1==10||o1==127)return 1;
    if(o1==100&&(o2&0xc0)==64)return 1;
    if(o1==169&&o2==254)return 1;
    if(o1==172&&(o2&0xf0)==16)return 1;
    if(o1==192&&o2==0&&o3==0)return 1;
    if(o1==192&&o2==168)return 1;
    if(o1==198&&(o2&0xfe)==18)return 1;
    return o1>=224;
}
static int ipv6_blocked(const uint8_t b[16]){
    int zero=1;for(int i=0;i<15;i++)if(b[i]){zero=0;break;}
    if(zero&&(b[15]==0||b[15]==1))return 1;
    if((b[0]&0xfe)==0xfc)return 1;
    if(b[0]==0xfe&&(b[1]&0xc0)==0x80)return 1;
    if(b[0]==0x20&&b[1]==0x02)return 1;
    zero=1;for(int i=0;i<10;i++)if(b[i]){zero=0;break;}
    if(zero&&b[10]==0xff&&b[11]==0xff)return 1;
    if(b[0]==0&&b[1]==0x64&&b[2]==0xff&&b[3]==0x9b){
        zero=1;for(int i=4;i<12;i++)if(b[i]){zero=0;break;}
        if(zero)return 1;
    }
    return 0;
}
int sources_fetch_address_blocked(const char *ip){
    struct in_addr v4;struct in6_addr v6;
    if(ip&&inet_pton(AF_INET,ip,&v4)==1)return ipv4_blocked(ntohl(v4.s_addr));
    if(ip&&inet_pton(AF_INET6,ip,&v6)==1)return ipv6_blocked(v6.s6_addr);
    return 1;
}
int sources_fetch_address_equal(const char *a,const char *b){
    struct in_addr a4,b4;struct in6_addr a6,b6;
    if(a&&b&&inet_pton(AF_INET,a,&a4)==1&&inet_pton(AF_INET,b,&b4)==1)return !memcmp(&a4,&b4,sizeof(a4));
    return a&&b&&inet_pton(AF_INET6,a,&a6)==1&&inet_pton(AF_INET6,b,&b6)==1&&!memcmp(&a6,&b6,sizeof(a6));
}
const char *sources_fetch_addresses(const struct addrinfo *rows,char out[SOURCES_FETCH_ADDRESS_MAX]){
    out[0]=0;
    for(const struct addrinfo *row=rows;row;row=row->ai_next){
        char ip[SOURCES_FETCH_ADDRESS_MAX];const void *address=NULL;
        if(row->ai_family==AF_INET&&row->ai_addr&&row->ai_addrlen>=sizeof(struct sockaddr_in))address=&((const struct sockaddr_in*)row->ai_addr)->sin_addr;
        else if(row->ai_family==AF_INET6&&row->ai_addr&&row->ai_addrlen>=sizeof(struct sockaddr_in6))address=&((const struct sockaddr_in6*)row->ai_addr)->sin6_addr;
        if(!address||!inet_ntop(row->ai_family,address,ip,sizeof(ip))){out[0]=0;return "fetch_dns_failed";}
        if(sources_fetch_address_blocked(ip)){out[0]=0;return "fetch_nonpublic_address";}
        if(!out[0])memcpy(out,ip,strlen(ip)+1);
    }
    return out[0]?NULL:"fetch_dns_failed";
}
const char *sources_fetch_resolve(const SourcesConfig *cfg,char host[256],unsigned *port,char ip[SOURCES_FETCH_ADDRESS_MAX]){
    host[0]=0;ip[0]=0;*port=0;
    const char *error=sources_interrupted(cfg);if(error)return error;
    error=sources_fetch_authorize(cfg,cfg?cfg->fetch_origin:NULL);if(error)return error;
    const char *origin=cfg->fetch_origin;size_t scheme=!strncmp(origin,"https://",8)?8:7;
    const char *name=origin+scheme,*colon=NULL;size_t n=0;
    if(*name=='['){const char *end=strchr(name,']');n=(size_t)(end-name-1);name++;if(end[1])colon=end+1;}
    else{colon=strchr(name,':');n=colon?(size_t)(colon-name):strlen(name);}
    if(!n||n>=256)return "invalid_fetch_configuration";
    memcpy(host,name,n);host[n]=0;*port=colon?(unsigned)strtoul(colon+1,NULL,10):scheme==8?443u:80u;
    if(*port!=(scheme==8?443u:80u))return "fetch_public_port";
    struct addrinfo hints={.ai_socktype=SOCK_STREAM},*rows=NULL;
    int rc=getaddrinfo(host,NULL,&hints,&rows);
    error=sources_interrupted(cfg);
    if(!error)error=rc||!rows?"fetch_dns_failed":sources_fetch_addresses(rows,ip);
    if(rows)freeaddrinfo(rows);
    if(error)ip[0]=0;
    return error;
}
