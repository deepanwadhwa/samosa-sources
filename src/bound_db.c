#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "bound_db.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define BOUND_MAX 64
struct SourceBoundDb {int directory;uint64_t token;char prefix[64],name[32];};
/* SQLite file storage guarantees its native eight-byte alignment, not the
 * stronger alignment of max_align_t on every supported ABI. */
typedef struct {sqlite3_file file;SourceBoundDb *binding;sqlite3_file *real;char leaf[64];struct stat opened;sqlite3_int64 alignment;} BoundFile;
static sqlite3_vfs *parent_vfs,bound_vfs;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static SourceBoundDb *bindings[BOUND_MAX];
static uint64_t serial;
static int ready;
static int (*parent_open)(const char *,int,int);
static int (*parent_stat)(const char *,struct stat *);
static int (*parent_lstat)(const char *,struct stat *);
static int (*parent_fstat)(int,struct stat *);
static _Thread_local SourceBoundDb *active;
static _Thread_local int observed;
static _Thread_local struct stat opened_info;
#ifdef SAMOSA_SOURCES_TEST_FAULTS
void (*source_bound_test_admitted)(void);
#endif
static int private_file(const struct stat *s){return S_ISREG(s->st_mode)&&s->st_uid==geteuid()&&!(s->st_mode&077)&&s->st_nlink==1;}
static int bound_fstat(int fd,struct stat *out){
    int rc=parent_fstat(fd,out);
    /* Native Unix may reuse a deferred descriptor while another connection
     * holds a lock. Its actual descriptor must still be validated. */
    if(!rc&&active&&private_file(out)){opened_info=*out;observed=1;}
    return rc;
}
static const char *leaf_name(SourceBoundDb *b,const char *path){
    size_t n=strlen(b->prefix);if(!path||strncmp(path,b->prefix,n)||path[n]!='/')return NULL;
    const char *leaf=path+n+1;size_t length=strlen(b->name);
    return !strcmp(leaf,b->name)||(!strncmp(leaf,b->name,length)&&
        (!strcmp(leaf+length,"-journal")||!strcmp(leaf+length,"-wal")))?leaf:NULL;
}
static SourceBoundDb *lookup(const char *path){
    SourceBoundDb *result=NULL;pthread_mutex_lock(&mutex);
    for(size_t i=0;i<BOUND_MAX;i++)if(bindings[i]&&leaf_name(bindings[i],path)){result=bindings[i];break;}
    pthread_mutex_unlock(&mutex);return result;
}
static int bound_metadata(const char *path,struct stat *out,int follow){
    if(!active)return follow?parent_stat(path,out):parent_lstat(path,out);
    if(!strcmp(path,active->prefix))return fstat(active->directory,out);
    const char *leaf=leaf_name(active,path);
    if(!leaf){errno=EACCES;return -1;}
    return fstatat(active->directory,leaf,out,AT_SYMLINK_NOFOLLOW);
}
static int bound_stat(const char *path,struct stat *out){return bound_metadata(path,out,1);}
static int bound_lstat(const char *path,struct stat *out){return bound_metadata(path,out,0);}
static int bound_os_open(const char *path,int flags,int mode){
    if(!active)return parent_open(path,flags,mode);
    if(!strcmp(path,active->prefix))return fcntl(active->directory,F_DUPFD_CLOEXEC,0);
    const char *leaf=leaf_name(active,path);if(!leaf){errno=EACCES;return -1;}
    struct stat named;
    if(!fstatat(active->directory,leaf,&named,AT_SYMLINK_NOFOLLOW)){if(!private_file(&named)){errno=EACCES;return -1;}}
    else if(errno!=ENOENT)return -1;
    int fd=openat(active->directory,leaf,flags|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0)return -1;
    struct stat held;
    if(fstat(fd,&held)||!private_file(&held)){close(fd);errno=EACCES;return -1;}
    opened_info=held;observed=1;return fd;
}
static int binding_live(BoundFile *f){struct stat named;return !fstatat(f->binding->directory,f->leaf,&named,AT_SYMLINK_NOFOLLOW)&&private_file(&named)&&named.st_dev==f->opened.st_dev&&named.st_ino==f->opened.st_ino;}
#define ENTER(f) SourceBoundDb *saved=active;active=(f)->binding
#define LEAVE() active=saved
static int io_close(sqlite3_file *file){BoundFile *f=(BoundFile*)file;ENTER(f);int rc=f->real->pMethods->xClose(f->real);LEAVE();f->file.pMethods=NULL;return rc;}
static int io_read(sqlite3_file *file,void *p,int n,sqlite3_int64 offset){BoundFile *f=(BoundFile*)file;ENTER(f);int rc=f->real->pMethods->xRead(f->real,p,n,offset);LEAVE();return rc;}
static int io_write(sqlite3_file *file,const void *p,int n,sqlite3_int64 offset){BoundFile *f=(BoundFile*)file;if(!binding_live(f))return SQLITE_READONLY_DBMOVED;ENTER(f);int rc=f->real->pMethods->xWrite(f->real,p,n,offset);LEAVE();return rc;}
static int io_truncate(sqlite3_file *file,sqlite3_int64 n){BoundFile *f=(BoundFile*)file;if(!binding_live(f))return SQLITE_READONLY_DBMOVED;ENTER(f);int rc=f->real->pMethods->xTruncate(f->real,n);LEAVE();return rc;}
static int io_sync(sqlite3_file *file,int flags){BoundFile *f=(BoundFile*)file;ENTER(f);int rc=f->real->pMethods->xSync(f->real,flags);LEAVE();return rc;}
static int io_size(sqlite3_file *file,sqlite3_int64 *n){BoundFile *f=(BoundFile*)file;ENTER(f);int rc=f->real->pMethods->xFileSize(f->real,n);LEAVE();return rc;}
static int io_lock(sqlite3_file *file,int lock){BoundFile *f=(BoundFile*)file;ENTER(f);int rc=f->real->pMethods->xLock(f->real,lock);LEAVE();return rc;}
static int io_unlock(sqlite3_file *file,int lock){BoundFile *f=(BoundFile*)file;ENTER(f);int rc=f->real->pMethods->xUnlock(f->real,lock);LEAVE();return rc;}
static int io_reserved(sqlite3_file *file,int *result){BoundFile *f=(BoundFile*)file;ENTER(f);int rc=f->real->pMethods->xCheckReservedLock(f->real,result);LEAVE();return rc;}
static int io_control(sqlite3_file *file,int op,void *arg){BoundFile *f=(BoundFile*)file;if(op==SQLITE_FCNTL_HAS_MOVED){*(int*)arg=!binding_live(f);return SQLITE_OK;}ENTER(f);int rc=f->real->pMethods->xFileControl(f->real,op,arg);LEAVE();return rc;}
static int io_sector(sqlite3_file *file){BoundFile *f=(BoundFile*)file;return f->real->pMethods->xSectorSize(f->real);}
static int io_characteristics(sqlite3_file *file){BoundFile *f=(BoundFile*)file;return f->real->pMethods->xDeviceCharacteristics(f->real);}
static const sqlite3_io_methods methods={.iVersion=1,.xClose=io_close,.xRead=io_read,.xWrite=io_write,.xTruncate=io_truncate,.xSync=io_sync,.xFileSize=io_size,.xLock=io_lock,.xUnlock=io_unlock,.xCheckReservedLock=io_reserved,.xFileControl=io_control,.xSectorSize=io_sector,.xDeviceCharacteristics=io_characteristics};
static int vfs_open(sqlite3_vfs *v,const char *path,sqlite3_file *file,int flags,int *outflags){
    (void)v;SourceBoundDb *b=lookup(path);if(!b||!(flags&(SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_MAIN_JOURNAL)))return SQLITE_CANTOPEN;
    BoundFile *f=(BoundFile*)file;memset(f,0,sizeof(*f));f->binding=b;f->real=(sqlite3_file*)((char*)file+sizeof(*f));snprintf(f->leaf,sizeof(f->leaf),"%s",leaf_name(b,path));
    SourceBoundDb *saved=active;active=b;observed=0;int rc=parent_vfs->xOpen(parent_vfs,path,f->real,flags,outflags);struct stat opened=opened_info;int checked=observed;active=saved;
    if(rc!=SQLITE_OK)return rc;
    f->opened=opened;
    if(!checked||!binding_live(f)){active=b;f->real->pMethods->xClose(f->real);active=saved;return SQLITE_CANTOPEN;}
    f->file.pMethods=&methods;return SQLITE_OK;
}
static int vfs_delete(sqlite3_vfs *v,const char *path,int syncdir){
    (void)v;SourceBoundDb *b=lookup(path);const char *leaf=b?leaf_name(b,path):NULL;if(!leaf||!strcmp(leaf,b->name))return SQLITE_IOERR_DELETE;
    struct stat info;if(fstatat(b->directory,leaf,&info,AT_SYMLINK_NOFOLLOW)){return errno==ENOENT?SQLITE_OK:SQLITE_IOERR_DELETE;}
    if(!private_file(&info)||unlinkat(b->directory,leaf,0))return SQLITE_IOERR_DELETE;
    return syncdir&&fsync(b->directory)?SQLITE_IOERR_DIR_FSYNC:SQLITE_OK;
}
static int vfs_access(sqlite3_vfs *v,const char *path,int flags,int *result){
    (void)v;(void)flags;SourceBoundDb *b=lookup(path);const char *leaf=b?leaf_name(b,path):NULL;*result=0;if(!leaf)return SQLITE_IOERR_ACCESS;
    struct stat info;if(fstatat(b->directory,leaf,&info,AT_SYMLINK_NOFOLLOW))return errno==ENOENT?SQLITE_OK:SQLITE_IOERR_ACCESS;
    if(!private_file(&info))return SQLITE_IOERR_ACCESS;
    *result=1;return SQLITE_OK;
}
static int vfs_full(sqlite3_vfs *v,const char *path,int n,char *out){(void)v;if(!lookup(path)||strlen(path)>=(size_t)n)return SQLITE_CANTOPEN;memcpy(out,path,strlen(path)+1);return SQLITE_OK;}
static void *vfs_dl_open(sqlite3_vfs *v,const char *path){(void)v;return parent_vfs->xDlOpen(parent_vfs,path);}
static void vfs_dl_error(sqlite3_vfs *v,int n,char *out){(void)v;parent_vfs->xDlError(parent_vfs,n,out);}
static void (*vfs_dl_sym(sqlite3_vfs *v,void *handle,const char *name))(void){(void)v;return parent_vfs->xDlSym(parent_vfs,handle,name);}
static void vfs_dl_close(sqlite3_vfs *v,void *handle){(void)v;parent_vfs->xDlClose(parent_vfs,handle);}
static int vfs_random(sqlite3_vfs *v,int n,char *out){(void)v;return parent_vfs->xRandomness(parent_vfs,n,out);}
static int vfs_sleep(sqlite3_vfs *v,int us){(void)v;return parent_vfs->xSleep(parent_vfs,us);}
static int vfs_time(sqlite3_vfs *v,double *out){(void)v;return parent_vfs->xCurrentTime(parent_vfs,out);}
static int vfs_error(sqlite3_vfs *v,int n,char *out){(void)v;return parent_vfs->xGetLastError?parent_vfs->xGetLastError(parent_vfs,n,out):0;}
static void initialize(void){
    parent_vfs=sqlite3_vfs_find(NULL);if(!parent_vfs||parent_vfs->iVersion<3||!parent_vfs->xGetSystemCall||!parent_vfs->xSetSystemCall)return;
    parent_open=(int(*)(const char*,int,int))parent_vfs->xGetSystemCall(parent_vfs,"open");if(!parent_open)return;
    parent_stat=(int(*)(const char*,struct stat*))parent_vfs->xGetSystemCall(parent_vfs,"stat");
    parent_lstat=(int(*)(const char*,struct stat*))parent_vfs->xGetSystemCall(parent_vfs,"lstat");
    parent_fstat=(int(*)(int,struct stat*))parent_vfs->xGetSystemCall(parent_vfs,"fstat");
    if(!parent_stat||!parent_lstat||!parent_fstat)return;
    if(parent_vfs->xSetSystemCall(parent_vfs,"open",(sqlite3_syscall_ptr)bound_os_open)!=SQLITE_OK)return;
    if(parent_vfs->xSetSystemCall(parent_vfs,"stat",(sqlite3_syscall_ptr)bound_stat)!=SQLITE_OK)return;
    if(parent_vfs->xSetSystemCall(parent_vfs,"lstat",(sqlite3_syscall_ptr)bound_lstat)!=SQLITE_OK)return;
    if(parent_vfs->xSetSystemCall(parent_vfs,"fstat",(sqlite3_syscall_ptr)bound_fstat)!=SQLITE_OK)return;
    bound_vfs=(sqlite3_vfs){.iVersion=1,.szOsFile=(int)sizeof(BoundFile)+parent_vfs->szOsFile,.mxPathname=256,.zName="samosa-bound-v1",.xOpen=vfs_open,.xDelete=vfs_delete,.xAccess=vfs_access,.xFullPathname=vfs_full,.xDlOpen=vfs_dl_open,.xDlError=vfs_dl_error,.xDlSym=vfs_dl_sym,.xDlClose=vfs_dl_close,.xRandomness=vfs_random,.xSleep=vfs_sleep,.xCurrentTime=vfs_time,.xGetLastError=vfs_error};
    ready=sqlite3_vfs_register(&bound_vfs,0)==SQLITE_OK;
}
static void release(SourceBoundDb *b){pthread_mutex_lock(&mutex);for(size_t i=0;i<BOUND_MAX;i++)if(bindings[i]==b)bindings[i]=NULL;pthread_mutex_unlock(&mutex);close(b->directory);free(b);}
int source_bound_open(int directory,const char *name,sqlite3 **db,SourceBoundDb **binding){
    *db=NULL;*binding=NULL;if(!name||(strcmp(name,"sources.sqlite3")&&strcmp(name,"read-operations.sqlite3")))return SQLITE_CANTOPEN;
    pthread_once(&once,initialize);if(!ready)return SQLITE_CANTOPEN;
    SourceBoundDb *b=calloc(1,sizeof(*b));if(!b)return SQLITE_NOMEM;b->directory=fcntl(directory,F_DUPFD_CLOEXEC,0);struct stat info;
    if(b->directory<0||fstat(b->directory,&info)||!S_ISDIR(info.st_mode)||info.st_uid!=geteuid()||(info.st_mode&077)){if(b->directory>=0)close(b->directory);free(b);return SQLITE_CANTOPEN;}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if(source_bound_test_admitted)source_bound_test_admitted();
#endif
    strcpy(b->name,name);size_t slot=BOUND_MAX;pthread_mutex_lock(&mutex);for(size_t i=0;i<BOUND_MAX;i++)if(!bindings[i]){slot=i;break;}
    if(slot<BOUND_MAX&&serial!=UINT64_MAX){b->token=++serial;snprintf(b->prefix,sizeof(b->prefix),"/samosa-bound-v1/%016llx",(unsigned long long)b->token);bindings[slot]=b;}else slot=BOUND_MAX;pthread_mutex_unlock(&mutex);
    if(slot==BOUND_MAX){close(b->directory);free(b);return SQLITE_BUSY;}
    char path[128];snprintf(path,sizeof(path),"%s/%s",b->prefix,b->name);int rc=sqlite3_open_v2(path,db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX|SQLITE_OPEN_PRIVATECACHE,"samosa-bound-v1");
    /* The store sets journal mode after configuring its busy handler. Doing
     * that here can turn an ordinary concurrent writer into admission failure. */
    if(rc==SQLITE_OK)rc=sqlite3_exec(*db,"PRAGMA temp_store=MEMORY;",NULL,NULL,NULL);
    if(rc!=SQLITE_OK){if(*db)sqlite3_close(*db);*db=NULL;release(b);return rc;}
    *binding=b;return SQLITE_OK;
}
int source_bound_close(sqlite3 *db,SourceBoundDb *binding){int rc=sqlite3_close(db);if(rc==SQLITE_OK)release(binding);return rc;}
