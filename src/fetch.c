#define _POSIX_C_SOURCE 200809L
#include "fetch.h"
#include "json.h"
#include "sha256.h"
#include <strings.h>
#include <arpa/inet.h>
#include "fetch_public.h"
/* Intentionally narrow URL grammar, identical in parent and worker. No parser
 * normalization can expand an exact host-granted authority. */
const char *sources_fetch_authorize(const SourcesConfig *cfg,const char *url){
    if(cfg&&(cfg->fetch_offline<0||cfg->fetch_offline>1))return "invalid_fetch_configuration";
    if(cfg&&cfg->fetch_offline)return "fetch_offline";
    if(!cfg||!cfg->fetch_origin||!cfg->fetch_helper||!*cfg->fetch_helper)return "fetch_unavailable";
    if(cfg->fetch_ca_pem&&(!*cfg->fetch_ca_pem||strlen(cfg->fetch_ca_pem)>8192))return "invalid_fetch_configuration";
    const char *origin=cfg->fetch_origin;
    if(cfg->fetch_public_only<0||cfg->fetch_public_only>1)return "invalid_fetch_configuration";
    int https=!strncmp(origin,"https://",8);
    size_t scheme=https?8:7,n=strlen(origin);
    if((!https&&strncmp(origin,"http://",7))||n<=scheme||n>256)return "invalid_fetch_configuration";
    const char *host=origin+scheme,*colon=NULL;size_t host_length=0;int loopback=0;
    if(*host=='['){
        const char *end=strchr(host,']');char literal[46];struct in6_addr address;
        if(!end||end==host+1||(size_t)(end-host-1)>=sizeof(literal)||(end[1]&&end[1]!=':'))return "invalid_fetch_configuration";
        host_length=(size_t)(end-host-1);memcpy(literal,host+1,host_length);literal[host_length]=0;
        if(strspn(literal,"0123456789abcdefABCDEF:.")!=host_length||inet_pton(AF_INET6,literal,&address)!=1)return "invalid_fetch_configuration";
        static const unsigned char local[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};loopback=!memcmp(address.s6_addr,local,sizeof(local));
        if(end[1])colon=end+1;
    }else{
        colon=strchr(host,':');host_length=colon?(size_t)(colon-host):strlen(host);
        if(!host_length||host[0]=='.'||host[host_length-1]=='.')return "invalid_fetch_configuration";
        for(size_t i=0;i<host_length;i++)if(!strchr("abcdefghijklmnopqrstuvwxyz0123456789.-",host[i]))return "invalid_fetch_configuration";
        loopback=host_length==9&&!memcmp(host,"127.0.0.1",9);
    }
    if(!https&&!cfg->fetch_public_only&&!loopback)return "invalid_fetch_configuration";
    if(colon){
        size_t port=0,digits=strlen(colon+1);
        if(!digits||digits>5)return "invalid_fetch_configuration";
        for(size_t i=1;colon[i];i++){
            if(colon[i]<'0'||colon[i]>'9')return "invalid_fetch_configuration";
            port=port*10+(size_t)(colon[i]-'0');
        }
        if(!port||port>65535)return "invalid_fetch_configuration";
    }
    if(!url||strlen(url)>2048||strncmp(url,origin,n)||
       (url[n]&&url[n]!='/'&&url[n]!='?'))return "unauthorized_url";
    for(size_t i=0;url[i];i++){
        unsigned c=(unsigned char)url[i];
        if(c<=32||c>=127||c=='#'||c=='\\')return "unauthorized_url";
        if(c=='%'&&(!url[i+1]||!url[i+2]||json_hex(url[i+1])<0||json_hex(url[i+2])<0))return "unauthorized_url";
    }
    return NULL;
}
static int digit(unsigned char c){
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *p=c?strchr(alphabet,c):NULL;return p?(int)(p-alphabet):-1;
}
const char *sources_fetch_validate(const char *url,const char *body,size_t n,unsigned char **bytes,size_t *length){
    *bytes=NULL;*length=0;
    Json j;size_t declared=0,status=0;
    if(!url||!body||n>SOURCES_HTTP_RESPONSE_MAX||!json_parse(&j,body,n)||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count!=5)return "native_helper_protocol";
    char *actual=json_string(&j,json_get(&j,0,"url")),*digest=json_string(&j,json_get(&j,0,"digest")),*data=json_string(&j,json_get(&j,0,"data"));
    const char *error=NULL;
    if(!actual||strcmp(actual,url)||!digest||strlen(digest)!=64||!data||
       !json_uint(&j,json_get(&j,0,"bytes"),&declared)||declared>SOURCES_FETCH_MAX||
       !json_uint(&j,json_get(&j,0,"status"),&status)||status!=200)error="native_helper_protocol";
    size_t encoded=data?strlen(data):0,padding=declared%3?3-declared%3:0;
    if(!error&&encoded!=((declared+2)/3)*4)error="native_helper_protocol";
    unsigned char *output=error?NULL:malloc(declared+1);
    if(!error&&!output)error="resource_limit";
    size_t offset=0;
    for(size_t i=0;!error&&i<encoded;i+=4){
        int a=digit((unsigned char)data[i]),b=digit((unsigned char)data[i+1]);
        int last=i+4==encoded, pad=last?(int)padding:0;
        int c=pad==2?0:digit((unsigned char)data[i+2]),d=pad?0:digit((unsigned char)data[i+3]);
        if(a<0||b<0||c<0||d<0||(pad==2&&(data[i+2]!='='||(b&15)))||(pad&&(data[i+3]!='='||(pad==1&&(c&3))))){error="native_helper_protocol";break;}
        output[offset++]=(unsigned char)((a<<2)|(b>>4));
        if(pad!=2)output[offset++]=(unsigned char)((b<<4)|(c>>2));
        if(!pad)output[offset++]=(unsigned char)((c<<6)|d);
    }
    if(!error){char hash[65];hash_bytes(output,declared,hash);if(offset!=declared||strcmp(hash,digest))error="native_helper_protocol";}
    free(actual);free(digest);free(data);
    if(error)free(output);else{*bytes=output;*length=declared;}
    return error;
}

const char *sources_http_options_validate(const char *method,const char *headers,const char *post){
    if(!method||(strcmp(method,"GET")&&strcmp(method,"POST"))||(!strcmp(method,"GET")&&post)||(!strcmp(method,"POST")&&!post)|| (post&&strlen(post)>8192))return "invalid_fetch_configuration";
    if(!headers)return NULL;
    Json j;
    if(strlen(headers)>4096||!json_parse(&j,headers,strlen(headers))||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count>8)return "invalid_fetch_configuration";
    for(int t=1;t<j.tokens[0].next;t=j.tokens[j.tokens[t].next].next){
        char *name=json_string(&j,t),*value=json_string(&j,j.tokens[t].next);int bad=!name||!*name||strlen(name)>64||!value||strlen(value)>1024;
        if(!bad&&(!strcasecmp(name,"Host")||!strcasecmp(name,"Content-Length")||!strcasecmp(name,"Transfer-Encoding")||!strcasecmp(name,"Connection")||!strcasecmp(name,"Proxy-Authorization")||!strcasecmp(name,"Accept-Encoding")))bad=1;
        if(name)for(size_t i=0;name[i];i++)if((unsigned char)name[i]>=127||(!isalnum((unsigned char)name[i])&&!strchr("!#$%&'*+-.^_`|~",name[i])))bad=1;
        if(value)for(size_t i=0;value[i];i++)if((unsigned char)value[i]<32||(unsigned char)value[i]>=127)bad=1;
        free(name);free(value);if(bad)return "invalid_fetch_configuration";
    }
    return NULL;
}
const char *sources_fetch_validate_route(const SourcesConfig *cfg,const char *url,const char *body,size_t n,unsigned char **bytes,size_t *length,char **final_url,char **urls){
    *bytes=NULL;*length=0;*final_url=NULL;*urls=NULL;
    Json j;
    if(!body||n>SOURCES_HTTP_RESPONSE_MAX||!json_parse(&j,body,n)||j.tokens[0].type!=JT_OBJECT||j.tokens[0].count!=6)return "native_helper_protocol";
    int array=json_get(&j,0,"urls");
    if(array<0||j.tokens[array].type!=JT_ARRAY||!j.tokens[array].count||j.tokens[array].count>(int)SOURCES_FETCH_MAX_HOPS)return "native_helper_protocol";
    char *seen[SOURCES_FETCH_MAX_HOPS]={0};size_t count=0;const char *error=NULL;
    for(int token=array+1;!error&&token<j.tokens[array].next;token=j.tokens[token].next){
        char origin[257];seen[count]=json_string(&j,token);
        if(!seen[count]||sources_fetch_url_origin(seen[count],origin)||(!count&&(strcmp(seen[count],url)||sources_fetch_authorize(cfg,seen[count]))))error="native_helper_protocol";
        for(size_t i=0;!error&&i<count;i++)if(!strcmp(seen[i],seen[count]))error="native_helper_protocol";
        count++;
    }
    char *plain=NULL;size_t plain_length=0;
    if(!error){
        FILE *out=open_memstream(&plain,&plain_length);if(!out)error="resource_limit";
        else{
            fputc('{',out);const char *fields[]={"url","status","bytes","digest","data"};
            for(size_t i=0;i<5;i++){int token=json_get(&j,0,fields[i]);if(token<0){error="native_helper_protocol";break;}if(i)fputc(',',out);fprintf(out,"\"%s\":",fields[i]);fwrite(body+j.tokens[token].begin,1,j.tokens[token].end-j.tokens[token].begin,out);}
            fputc('}',out);if(fclose(out)&&!error)error="resource_limit";
        }
    }
    if(!error)error=sources_fetch_validate(url,plain,plain_length,bytes,length);
    if(!error){
        *final_url=strdup(seen[count-1]);size_t size=j.tokens[array].end-j.tokens[array].begin;
        *urls=malloc(size+1);if(*urls){memcpy(*urls,body+j.tokens[array].begin,size);(*urls)[size]=0;}
        if(!*final_url||!*urls)error="resource_limit";
    }
    for(size_t i=0;i<count;i++)free(seen[i]);
    free(plain);
    if(error){free(*bytes);free(*final_url);free(*urls);*bytes=NULL;*length=0;*final_url=NULL;*urls=NULL;}
    return error;
}
