#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sources.h"
#include "json.h"
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sqlite3.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#x);abort();}}while(0)
static EVP_PKEY *key(void){EVP_PKEY *p=EVP_RSA_gen(2048);CHECK(p);return p;}
static void extension(X509 *certificate,X509 *issuer,int nid,const char *value){
    X509V3_CTX context;X509V3_set_ctx(&context,issuer,certificate,NULL,NULL,0);
    X509_EXTENSION *ext=X509V3_EXT_conf_nid(NULL,&context,nid,value);CHECK(ext);CHECK(X509_add_ext(certificate,ext,-1));X509_EXTENSION_free(ext);
}
static X509 *certificate(EVP_PKEY *subject_key,EVP_PKEY *issuer_key,X509 *issuer,int wrong_name,int expired){
    X509 *c=X509_new();CHECK(c);CHECK(X509_set_version(c,2));CHECK(ASN1_INTEGER_set(X509_get_serialNumber(c),issuer?2:1));
    CHECK(X509_gmtime_adj(X509_getm_notBefore(c),expired?-3600:-60));CHECK(X509_gmtime_adj(X509_getm_notAfter(c),expired?-60:3600));CHECK(X509_set_pubkey(c,subject_key));
    X509_NAME *name=X509_get_subject_name(c);const char *cn=issuer?(wrong_name?"wrong.example.test":"127.0.0.1"):"Sources fixture CA";
    CHECK(X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,(const unsigned char *)cn,-1,-1,0));
    CHECK(X509_set_issuer_name(c,issuer?X509_get_subject_name(issuer):name));
    extension(c,issuer?issuer:c,NID_basic_constraints,issuer?"critical,CA:FALSE":"critical,CA:TRUE");
    extension(c,issuer?issuer:c,NID_key_usage,issuer?"critical,digitalSignature,keyEncipherment":"critical,keyCertSign,cRLSign");
    if(issuer){extension(c,issuer,NID_ext_key_usage,"serverAuth");extension(c,issuer,NID_subject_alt_name,wrong_name?"DNS:wrong.example.test":"IP:127.0.0.1");}
    CHECK(X509_sign(c,issuer_key,EVP_sha256())>0);return c;
}
static char *pem(X509 *c){
    BIO *b=BIO_new(BIO_s_mem());CHECK(b);CHECK(PEM_write_bio_X509(b,c));char *data=NULL;long n=BIO_get_mem_data(b,&data);CHECK(n>0&&n<8192);char *copy=malloc((size_t)n+1);CHECK(copy);memcpy(copy,data,(size_t)n);copy[n]=0;BIO_free(b);return copy;
}
static pid_t fixture(SSL_CTX *context,unsigned short *port){
    int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    CHECK(!bind(fd,(struct sockaddr *)&address,sizeof(address)));CHECK(!listen(fd,1));socklen_t n=sizeof(address);CHECK(!getsockname(fd,(struct sockaddr *)&address,&n));*port=ntohs(address.sin_port);
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        signal(SIGPIPE,SIG_IGN);alarm(15);int peer=accept(fd,NULL,NULL);if(peer<0)_exit(1);close(fd);
        SSL *ssl=SSL_new(context);if(!ssl||!SSL_set_fd(ssl,peer))_exit(1);
        if(SSL_accept(ssl)==1){
            char request[4096]={0};size_t offset=0;
            while(!strstr(request,"\r\n\r\n")){int got=SSL_read(ssl,request+offset,(int)(sizeof(request)-1-offset));if(got<=0)break;offset+=(size_t)got;if(offset==sizeof(request)-1)break;}
            if(strstr(request,"\r\n\r\n")){
                const char *response="HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\ntls 7620 ok";
                size_t length=strlen(response);offset=0;
                while(offset<length){int got=SSL_write(ssl,response+offset,(int)(length-offset));if(got<=0)break;offset+=(size_t)got;}
                SSL_shutdown(ssl);
            }
        }
        SSL_free(ssl);close(peer);_exit(0);
    }
    close(fd);return child;
}
static char *fetch(SourcesConfig *cfg,const char *url,int expected){
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);int result=sources_fetch(cfg,SOURCES_UPLOAD_TEXT,url,out);CHECK(!fclose(out));
    if(result!=expected)fprintf(stderr,"TLS fetch: %s\n",body);
    CHECK(result==expected);return body;
}
static void cli(const char *binary,const char *worker,const char *origin,const char *ca_file,const char *state){
    int input[2],output[2];CHECK(!pipe(input));CHECK(!pipe(output));pid_t child=fork();CHECK(child>=0);
    if(!child){close(input[1]);close(output[0]);if(dup2(input[0],0)<0||dup2(output[1],1)<0)_exit(125);close(input[0]);close(output[1]);execl(binary,binary,"--state",state,"--owner","tls-owner","--fetch-helper",worker,"--fetch-origin",origin,"--fetch-ca-file",ca_file,(char *)NULL);_exit(125);}
    close(input[0]);close(output[1]);char request[1024];int n=snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"fetch\",\"acquisition_id\":\"tls-cli-capture\",\"url\":\"%s/source\",\"format\":\"text\",\"operation_id\":\"tls-cli-attempt\"}",origin);CHECK(n>0&&(size_t)n<sizeof(request));CHECK(write(input[1],request,(size_t)n)==n);close(input[1]);
    char body[4096]={0};size_t offset=0;for(;;){ssize_t got=read(output[0],body+offset,sizeof(body)-1-offset);CHECK(got>=0);if(!got)break;offset+=(size_t)got;CHECK(offset<sizeof(body)-1);}close(output[0]);int status;CHECK(waitpid(child,&status,0)==child);if(!WIFEXITED(status)||WEXITSTATUS(status))fprintf(stderr,"TLS CLI: %s\n",body);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));CHECK(strstr(body,"tls-cli-capture"));CHECK(strstr(body,"tls-cli-attempt"));
}
static void cli_refuse(const char *binary,const char *path){
    pid_t child=fork();CHECK(child>=0);
    if(!child){int fd=open("/dev/null",O_RDONLY);if(fd<0||dup2(fd,0)<0)_exit(125);close(fd);execl(binary,binary,"--fetch-ca-file",path,(char *)NULL);_exit(125);}
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==2);
}
int main(int argc,char **argv){
    CHECK(argc==3);signal(SIGPIPE,SIG_IGN);EVP_PKEY *authority_key=key(),*other_key=key(),*server_key=key();
    X509 *authority=certificate(authority_key,authority_key,NULL,0,0),*other=certificate(other_key,other_key,NULL,0,0);char *trusted=pem(authority),*wrong=pem(other);
    char state[]="/tmp/samosa-fetch-tls-XXXXXX";CHECK(mkdtemp(state));SourcesConfig cfg={.state=state,.owner="tls-owner",.fetch_helper=argv[1]};char origin[128],url[256],source[65]={0};
    for(int mode=0;mode<7;mode++){
        fprintf(stderr,"TLS fixture mode %d\n",mode);
        X509 *leaf=certificate(server_key,authority_key,authority,mode==3,mode==4);SSL_CTX *context=SSL_CTX_new(TLS_server_method());CHECK(context);CHECK(SSL_CTX_set_min_proto_version(context,TLS1_2_VERSION));CHECK(SSL_CTX_set_max_proto_version(context,TLS1_2_VERSION));CHECK(SSL_CTX_use_certificate(context,leaf));CHECK(SSL_CTX_use_PrivateKey(context,server_key));CHECK(SSL_CTX_check_private_key(context));
        unsigned short port;pid_t server=fixture(context,&port);snprintf(origin,sizeof(origin),"https://127.0.0.1:%u",port);snprintf(url,sizeof(url),"%s/source",origin);cfg.fetch_origin=origin;cfg.fetch_ca_pem=mode==1?NULL:mode==2?wrong:mode==5?"invalid pem":trusted;
        if(mode==6){char path[512];snprintf(path,sizeof(path),"%s/ca.pem",state);int fd=open(path,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(write(fd,trusted,strlen(trusted))==(ssize_t)strlen(trusted));CHECK(!close(fd));cli(argv[2],argv[1],origin,path,state);}
        else{
            char *body=fetch(&cfg,url,mode==0?0:1);
            if(mode)fprintf(stderr,"TLS refusal: %s\n",body);
            if(mode==0){Json j;CHECK(json_parse(&j,body,strlen(body)));char *id=json_string(&j,json_get(&j,0,"source_id"));CHECK(id&&strlen(id)==64);memcpy(source,id,65);free(id);}
            else if(mode==5)CHECK(strstr(body,"fetch_tls_trust_invalid"));
            else CHECK(strstr(body,"fetch_tls_verification_failed")||(mode==2&&strstr(body,"fetch_tls_failed")));
            free(body);
        }
        int status;CHECK(waitpid(server,&status,0)==server);CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));SSL_CTX_free(context);X509_free(leaf);
    }
    char link[512],target[512],large[512],nul[512];snprintf(target,sizeof(target),"%s/ca.pem",state);snprintf(link,sizeof(link),"%s/ca-link.pem",state);CHECK(!symlink(target,link));cli_refuse(argv[2],link);
    char too_large[8194];memset(too_large,'x',sizeof(too_large)-1);too_large[sizeof(too_large)-1]=0;
    snprintf(large,sizeof(large),"%s/ca-large.pem",state);int fd=open(large,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(write(fd,too_large,8193)==8193);CHECK(!close(fd));cli_refuse(argv[2],large);
    snprintf(nul,sizeof(nul),"%s/ca-nul.pem",state);fd=open(nul,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0);CHECK(write(fd,"a\0b",3)==3);CHECK(!close(fd));cli_refuse(argv[2],nul);
    cfg.fetch_ca_pem=too_large;char *body=fetch(&cfg,url,1);CHECK(strstr(body,"invalid_fetch_configuration"));free(body);cfg.fetch_ca_pem=NULL;
    char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":11}",source,source);size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);CHECK(!sources_dispatch(&cfg,request,strlen(request),out));CHECK(!fclose(out));CHECK(strstr(body,"tls 7620 ok"));free(body);
    char path[512];snprintf(path,sizeof(path),"%s/sources.sqlite3",state);sqlite3 *db=NULL;CHECK(sqlite3_open(path,&db)==SQLITE_OK);sqlite3_stmt *s=NULL;CHECK(sqlite3_prepare_v2(db,"SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM source_claims),(SELECT count(*) FROM fetch_receipts)",-1,&s,NULL)==SQLITE_OK);CHECK(sqlite3_step(s)==SQLITE_ROW);for(int i=0;i<3;i++)CHECK(sqlite3_column_int(s,i)==1);sqlite3_finalize(s);CHECK(sqlite3_close(db)==SQLITE_OK);
    free(trusted);free(wrong);X509_free(authority);X509_free(other);EVP_PKEY_free(authority_key);EVP_PKEY_free(other_key);EVP_PKEY_free(server_key);
    printf("PASS actual TLS trust/issuer/hostname/expiry/CLI/admission: %u checks\n",checks);return 0;
}
