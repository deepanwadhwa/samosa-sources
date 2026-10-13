#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "inventory.h"
#include "control.h"
#include "sha256.h"
#include "json.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#define ENTRY_MAX 256u
#define DIRECTORY_MAX 32u
#define DEPTH_MAX 4u
#define LOCATOR_MAX 4096u
#define OUTPUT_MAX 524288u
typedef struct {char *path;struct stat info;const char *kind;char identity[65];} Entry;
typedef struct {Entry entries[ENTRY_MAX];size_t count,directories,excluded,invalid_names,inspected;int partial;const char *error;size_t encoded_bound;const SourcesConfig *cfg;} Inventory;
static int fail(FILE *out,const char *code){fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",code);return 1;}
static int same_directory(const struct stat *a,const struct stat *b) {
#ifdef __APPLE__
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mtimespec.tv_sec==b->st_mtimespec.tv_sec&&a->st_mtimespec.tv_nsec==b->st_mtimespec.tv_nsec&&a->st_ctimespec.tv_sec==b->st_ctimespec.tv_sec&&a->st_ctimespec.tv_nsec==b->st_ctimespec.tv_nsec;
#else
    return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
#endif
}
static long long mtime_seconds(const struct stat *s){return (long long)s->st_mtime;}
static long mtime_nanos(const struct stat *s){
#ifdef __APPLE__
    return s->st_mtimespec.tv_nsec;
#else
    return s->st_mtim.tv_nsec;
#endif
}
void inventory_file_identity(const struct stat *info,char identity[65]) {
    char material[256];int n=snprintf(material,sizeof(material),"file-identity-v1:%llu:%llu:%llu:%lld:%ld",(unsigned long long)info->st_dev,(unsigned long long)info->st_ino,(unsigned long long)info->st_size,mtime_seconds(info),mtime_nanos(info));hash_bytes(material,(size_t)n,identity);
}
static int selected_directory(const SourcesConfig *cfg,const char *path) {
    if(!cfg||!cfg->root||!path||strlen(path)>=LOCATOR_MAX||path[0]=='/'||(path[0]&&path[strlen(path)-1]=='/'))return -1;
    int fd=open(cfg->root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
    if(!*path)return fd;
    char *copy=strdup(path);if(!copy){close(fd);return -1;}char *part=copy;
    for(;;){char *next=strchr(part,'/');if(next)*next=0;
        if(!*part||!strcmp(part,".")||!strcmp(part,"..")){close(fd);fd=-1;break;}
        int opened=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(fd);fd=opened;if(fd<0||!next)break;part=next+1;
    }
    free(copy);return fd;
}
static const char *scope_hash(const SourcesConfig *cfg,const char *path,const struct stat *root,char identity[65]) {
    const char *host=cfg->host_id?cfg->host_id:"local-endpoint";
    if(!*host||strlen(host)>128||!utf8_valid((const unsigned char*)host,strlen(host)))return "invalid_configuration";
    char material[LOCATOR_MAX+256];int n=snprintf(material,sizeof(material),"directory-scope-v1:%s:%llu:%llu:%s",host,(unsigned long long)root->st_dev,(unsigned long long)root->st_ino,path);
    if(n<0||(size_t)n>=sizeof(material))return "resource_limit";
    hash_bytes(material,(size_t)n,identity);return NULL;
}
const char *inventory_scope_identity(const SourcesConfig *cfg,const char *path,char identity[65]) {
    int fd=selected_directory(cfg,path);if(fd<0)return "unauthorized_path";struct stat root;const char *error=fstat(fd,&root)?"inventory_failed":scope_hash(cfg,path,&root,identity);close(fd);if(!error&&!*path&&cfg->root_scope_pin&&strcmp(cfg->root_scope_pin,identity))error="scope_unavailable";return error;
}
static void walk(Inventory *inventory,int fd,const char *prefix,size_t depth) {
    struct stat before,after;if(fstat(fd,&before)){inventory->error="inventory_failed";close(fd);return;}
    DIR *directory=fdopendir(fd);if(!directory){inventory->error="inventory_failed";close(fd);return;}inventory->directories++;
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    const char *marker=getenv("SAMOSA_INVENTORY_TEST_MARKER"),*release=getenv("SAMOSA_INVENTORY_TEST_RELEASE");
    if(marker&&release){FILE *ready=fopen(marker,"w");if(!ready)_exit(98);fputs("ready",ready);fclose(ready);int released=0;for(int i=0;i<500;i++){if(access(release,F_OK)==0){released=1;break;}struct timespec delay={0,10000000};nanosleep(&delay,NULL);}if(!released)_exit(99);}
#endif
    for(;;){inventory->error=sources_interrupted(inventory->cfg);if(inventory->error)break;errno=0;struct dirent *item=readdir(directory);if(!item){if(errno)inventory->error="inventory_failed";break;}
        if(!strcmp(item->d_name,".")||!strcmp(item->d_name,".."))continue;
        if(inventory->inspected++>=512){inventory->partial=1;break;}
        if(!utf8_valid((const unsigned char*)item->d_name,strlen(item->d_name))){inventory->invalid_names++;inventory->partial=1;continue;}
        size_t length=strlen(prefix)+strlen(item->d_name)+(*prefix?1:0);if(length>=LOCATOR_MAX){inventory->partial=1;continue;}
        if(inventory->count>=ENTRY_MAX||inventory->encoded_bound+length*6+512>OUTPUT_MAX){inventory->partial=1;break;}
        Entry *entry=&inventory->entries[inventory->count];entry->path=malloc(length+1);if(!entry->path){inventory->error="resource_limit";break;}
        snprintf(entry->path,length+1,"%s%s%s",prefix,*prefix?"/":"",item->d_name);
        if(fstatat(dirfd(directory),item->d_name,&entry->info,AT_SYMLINK_NOFOLLOW)){free(entry->path);entry->path=NULL;inventory->error="source_changed";break;}
        entry->kind=S_ISREG(entry->info.st_mode)?"file":S_ISDIR(entry->info.st_mode)?"directory":S_ISLNK(entry->info.st_mode)?"symlink":"special";
        inventory_file_identity(&entry->info,entry->identity);
        inventory->count++;inventory->encoded_bound+=length*6+512;
        if(S_ISDIR(entry->info.st_mode)){
            if(depth>=DEPTH_MAX||inventory->directories>=DIRECTORY_MAX){inventory->partial=1;continue;}
            int child=openat(dirfd(directory),item->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat opened;
            if(child<0){inventory->error="inventory_unavailable";break;}
            if(fstat(child,&opened)||!same_directory(&entry->info,&opened)){close(child);inventory->error="source_changed";break;}
            walk(inventory,child,entry->path,depth+1);if(inventory->error)break;
        }else if(!S_ISREG(entry->info.st_mode))inventory->excluded++;
    }
    if(!inventory->error&&(fstat(dirfd(directory),&after)||!same_directory(&before,&after)))inventory->error="source_changed";
    closedir(directory);
}
static int compare_entries(const void *a,const void *b){return strcmp(((const Entry*)a)->path,((const Entry*)b)->path);}
int inventory_run(const SourcesConfig *cfg,const char *selected,FILE *out) {
    char *path=selected?strdup(selected):NULL;if(!path)return fail(out,"invalid_request");
    int fd=selected_directory(cfg,path);if(fd<0){free(path);return fail(out,"unauthorized_path");}
    struct stat root;if(fstat(fd,&root)){close(fd);free(path);return fail(out,"inventory_failed");}
    const char *host=cfg->host_id?cfg->host_id:"local-endpoint";char scope_identity[65];const char *scope_error=scope_hash(cfg,path,&root,scope_identity);
    if(scope_error){close(fd);free(path);return fail(out,scope_error);}
    Inventory *inventory=calloc(1,sizeof(*inventory));if(!inventory){close(fd);free(path);return fail(out,"resource_limit");}
    inventory->cfg=cfg;walk(inventory,fd,path,0);int rc=0;
    if(inventory->error)rc=fail(out,inventory->error);
    else {
        qsort(inventory->entries,inventory->count,sizeof(Entry),compare_entries);
        fprintf(out,"{\"contract_version\":1,\"host_id\":");json_quote(out,host,strlen(host));fprintf(out,",\"scope_identity\":\"%s\",\"scope\":",scope_identity);json_quote(out,path,strlen(path));
        fprintf(out,",\"inventory_coverage\":{\"status\":\"%s\",\"returned_entries\":%zu,\"total_entries\":",inventory->partial?"partial":"complete",inventory->count);
        if(inventory->partial)fputs("null",out);else fprintf(out,"%zu",inventory->count);
        fprintf(out,",\"excluded_nonregular\":%zu,\"invalid_names\":%zu,\"visited_directories\":%zu},\"content_coverage\":\"uninspected\",\"entries\":[",inventory->excluded,inventory->invalid_names,inventory->directories);
        for(size_t i=0;i<inventory->count;i++){Entry *entry=&inventory->entries[i];if(i)fputc(',',out);fputs("{\"locator\":",out);json_quote(out,entry->path,strlen(entry->path));fprintf(out,",\"kind\":\"%s\",\"eligible\":%s,\"identity\":\"%s\",\"byte_size\":%llu,\"content_version\":null,\"content_coverage\":\"uninspected\"}",entry->kind,S_ISREG(entry->info.st_mode)?"true":"false",entry->identity,(unsigned long long)entry->info.st_size);}
        fputs("]}\n",out);
    }
    for(size_t i=0;i<inventory->count;i++){free(inventory->entries[i].path);}free(inventory);free(path);return rc;
}
