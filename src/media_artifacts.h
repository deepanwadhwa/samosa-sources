#ifndef SOURCES_MEDIA_ARTIFACTS_H
#define SOURCES_MEDIA_ARTIFACTS_H
#include "sources.h"
#include <sys/stat.h>
typedef struct {
    int fd;
    struct stat before;
    char path[4096];
    void *address;
} SourcesMediaArtifact;
enum {SOURCES_MEDIA_ARTIFACTS_COUNT=8};
typedef struct {SourcesMediaArtifact files[SOURCES_MEDIA_ARTIFACTS_COUNT];} SourcesMediaArtifacts;
const char *sources_media_artifacts_open(const SourcesConfig *,SourcesMediaArtifacts *,char[65]);
const char *sources_media_artifacts_check(const SourcesConfig *,const SourcesMediaArtifacts *);
void sources_media_artifacts_close(SourcesMediaArtifacts *);
#endif
