#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "bound_db.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void run(sqlite3 *db,const char *sql){int rc=sqlite3_exec(db,sql,NULL,NULL,NULL);if(rc)fprintf(stderr,"SQL failed %d: %s\n",rc,sqlite3_errmsg(db));assert(rc==SQLITE_OK);}
static sqlite3 *open_bound(int fd,SourceBoundDb **binding){sqlite3 *db=NULL;int rc=source_bound_open(fd,"sources.sqlite3",&db,binding);if(rc)fprintf(stderr,"bound open failed: %d\n",rc);assert(rc==SQLITE_OK);return db;}
static int count(sqlite3 *db){sqlite3_stmt *s=NULL;assert(sqlite3_prepare_v2(db,"SELECT count(*) FROM x",-1,&s,NULL)==SQLITE_OK);assert(sqlite3_step(s)==SQLITE_ROW);int n=sqlite3_column_int(s,0);assert(sqlite3_finalize(s)==SQLITE_OK);return n;}
static void external_busy(const char *program,const char *path){
    pid_t child=fork();assert(child>=0);
    if(!child){execl(program,program,"--expect-busy",path,(char*)NULL);_exit(127);}
    int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
static const char *swap_path;
static char swap_held[128];
static void swap_admitted(void){assert(!rename(swap_path,swap_held));assert(!mkdir(swap_path,0700));source_bound_test_admitted=NULL;}
#endif
int main(int argc,char **argv){
    if(argc==3&&!strcmp(argv[1],"--expect-busy")){
        sqlite3 *db=NULL;int rc=sqlite3_open(argv[2],&db);
        if(rc==SQLITE_OK)rc=sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL);
        if(rc!=SQLITE_BUSY)fprintf(stderr,"external lock expected BUSY, got %d\n",rc);
        sqlite3_close(db);return rc==SQLITE_BUSY?0:1;
    }
    char dir[]="/tmp/samosa-bound-db-XXXXXX",held[128],path[160];assert(mkdtemp(dir));int fd=open(dir,O_RDONLY|O_DIRECTORY);assert(fd>=0);
    SourceBoundDb *first=NULL,*second=NULL;
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    swap_path=dir;snprintf(swap_held,sizeof(swap_held),"%s-before-open",dir);source_bound_test_admitted=swap_admitted;
#endif
    sqlite3 *a=open_bound(fd,&first);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    snprintf(path,sizeof(path),"%s/sources.sqlite3",dir);assert(access(path,F_OK)<0&&errno==ENOENT);assert(!rmdir(dir));assert(!rename(swap_held,dir));
#endif
    run(a,"CREATE TABLE x(a);INSERT INTO x VALUES(1)");
    sqlite3 *b=open_bound(fd,&second);run(a,"BEGIN IMMEDIATE");assert(sqlite3_exec(b,"BEGIN IMMEDIATE",NULL,NULL,NULL)==SQLITE_BUSY);
    assert(source_bound_close(b,second)==SQLITE_OK);
    snprintf(path,sizeof(path),"%s/sources.sqlite3",dir);
    external_busy(argv[0],path);
    b=open_bound(fd,&second);external_busy(argv[0],path);
    assert(source_bound_close(b,second)==SQLITE_OK);external_busy(argv[0],path);
    snprintf(path,sizeof(path),"%s/sources.sqlite3",dir);sqlite3 *ordinary=NULL;assert(sqlite3_open(path,&ordinary)==SQLITE_OK);
    assert(sqlite3_exec(ordinary,"BEGIN IMMEDIATE",NULL,NULL,NULL)==SQLITE_BUSY);run(a,"ROLLBACK");run(ordinary,"BEGIN IMMEDIATE;ROLLBACK");assert(sqlite3_close(ordinary)==SQLITE_OK);
    snprintf(held,sizeof(held),"%s-held",dir);assert(!rename(dir,held));assert(!mkdir(dir,0700));run(a,"INSERT INTO x VALUES(2)");struct stat st;assert(lstat(path,&st)<0&&errno==ENOENT);assert(count(a)==2);assert(source_bound_close(a,first)==SQLITE_OK);
    int pipefd[2];assert(!pipe(pipefd));pid_t child=fork();assert(child>=0);
    if(!child){close(pipefd[0]);SourceBoundDb *binding=NULL;sqlite3 *db=open_bound(fd,&binding);run(db,"BEGIN IMMEDIATE;INSERT INTO x VALUES(3)");assert(sqlite3_db_cacheflush(db)==SQLITE_OK);assert(write(pipefd[1],"x",1)==1);for(;;)pause();}
    close(pipefd[1]);char ready;assert(read(pipefd[0],&ready,1)==1);close(pipefd[0]);assert(!kill(child,SIGKILL));int status;assert(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    a=open_bound(fd,&first);assert(count(a)==2);assert(source_bound_close(a,first)==SQLITE_OK);assert(lstat(path,&st)<0&&errno==ENOENT);close(fd);
    fd=open(held,O_RDONLY|O_DIRECTORY);assert(fd>=0);
    sqlite3 *connections[64];SourceBoundDb *bindings[64];
    for(size_t i=0;i<64;i++)connections[i]=open_bound(fd,&bindings[i]);
    sqlite3 *extra=NULL;SourceBoundDb *extra_binding=NULL;
    assert(source_bound_open(fd,"sources.sqlite3",&extra,&extra_binding)==SQLITE_BUSY);
    assert(!extra&&!extra_binding);
    for(size_t i=0;i<64;i++)assert(source_bound_close(connections[i],bindings[i])==SQLITE_OK);
    a=open_bound(fd,&first);assert(count(a)==2);assert(source_bound_close(a,first)==SQLITE_OK);
    assert(source_bound_open(fd,"unapproved.sqlite3",&extra,&extra_binding)==SQLITE_CANTOPEN);
    snprintf(path,sizeof(path),"%s/read-operations.sqlite3",held);
    assert(!symlink("sources.sqlite3",path));
    assert(source_bound_open(fd,"read-operations.sqlite3",&extra,&extra_binding)!=SQLITE_OK);assert(!extra&&!extra_binding);assert(!unlink(path));
    assert(!linkat(fd,"sources.sqlite3",fd,"read-operations.sqlite3",0));
    assert(source_bound_open(fd,"read-operations.sqlite3",&extra,&extra_binding)!=SQLITE_OK);assert(!extra&&!extra_binding);assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/sources.sqlite3",held);assert(!chmod(path,0644));
    assert(source_bound_open(fd,"sources.sqlite3",&extra,&extra_binding)!=SQLITE_OK);assert(!extra&&!extra_binding);
    assert(!chmod(path,0600));a=open_bound(fd,&first);assert(count(a)==2);
    char original[160];snprintf(original,sizeof(original),"%s/original.sqlite3",held);assert(!rename(path,original));
    b=open_bound(fd,&second);run(b,"CREATE TABLE x(a);INSERT INTO x VALUES(99)");assert(source_bound_close(b,second)==SQLITE_OK);
    int moved=sqlite3_exec(a,"INSERT INTO x VALUES(3)",NULL,NULL,NULL);
    assert((moved&255)==SQLITE_READONLY);assert(source_bound_close(a,first)==SQLITE_OK);
    b=open_bound(fd,&second);assert(count(b)==1);assert(source_bound_close(b,second)==SQLITE_OK);
    assert(!unlink(path));assert(!rename(original,path));a=open_bound(fd,&first);assert(count(a)==2);assert(source_bound_close(a,first)==SQLITE_OK);close(fd);
    snprintf(path,sizeof(path),"%s/sources.sqlite3",held);assert(!unlink(path));assert(!rmdir(held));assert(!rmdir(dir));
    puts("PASS bound DB: external locks, native interoperability, directory replacement, killed transaction rollback, capacity recovery and private-file admission");return 0;
}
