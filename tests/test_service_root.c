#define main service_socket_corpus
#include "test_service.c"
#undef main
int main(int argc,char **argv) {
    CHECK(argc==2); alarm(60); signal(SIGPIPE,SIG_IGN); umask(077);
    char base[]="/tmp/samosa-service-root-XXXXXX"; CHECK(mkdtemp(base));
    char root[512],state[512],auth[512],saved[512];
    snprintf(root,sizeof(root),"%s/content",base); snprintf(state,sizeof(state),"%s/state",base);
    snprintf(auth,sizeof(auth),"%s/auth.json",base); snprintf(saved,sizeof(saved),"%s/content-held",base);
    CHECK(!mkdir(root,0700)); FILE *f=fopen(auth,"w"); CHECK(f);
    fprintf(f,"[{\"owner\":\"alice\",\"token\":\"%s\"},{\"owner\":\"bob\",\"token\":\"%s\"}]",tokens[0],tokens[1]); CHECK(!fclose(f));
    pid_t pid; unsigned port=start(argv[1],root,state,auth,&pid);
    int fd=request_head(port,0,"/v1/uploads",4096,"X-Sources-Format: text\r\nExpect: 100-continue\r\n"); interim(fd);
    CHECK(!rename(root,saved)); CHECK(!mkdir(root,0700)); exited(pid,1); close(fd); await_admission(state);
    CHECK(!rmdir(root)); CHECK(!rename(saved,root));
    port=start(argv[1],root,state,auth,&pid);
    char *response=call(port,0,"{\"contract_version\":1,\"op\":\"describe\"}"); CHECK(strstr(response,"200")); free(response); stop(pid);
    printf("PASS native socket root scope: %u checks; replacement independent of retained state, active upload stop and bound-scope restart; %s\n",checks,base);
    return 0;
}
