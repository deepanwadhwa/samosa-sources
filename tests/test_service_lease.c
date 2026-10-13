#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "service_lease.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x)                                                               \
    do {                                                                       \
        checks++;                                                              \
        if (!(x)) {                                                            \
            fprintf(stderr, "FAIL lease line%d: %s\n", __LINE__, #x);          \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
int main(void) {
    umask(077);
    char state[] = "/tmp/samosa-service-lease-XXXXXX";
    CHECK(mkdtemp(state));
    SourcesConfig cfg = {.state = state};
    SourcesServiceLease a, b;
    CHECK(!sources_service_lease_open(&cfg, &a));
    CHECK(sources_service_lease_valid(&cfg, &a));
    const char *error = sources_service_lease_open(&cfg, &b);
    CHECK(error && !strcmp(error, "service_busy"));
    CHECK(b.directory == -1 && b.service == -1 && b.queue == -1);
    char name[512], other[512];
    snprintf(name, sizeof(name), "%s/service-runtime.lock", state);
    snprintf(other, sizeof(other), "%s/service-held.lock", state);
    CHECK(!link(name, other));
    CHECK(!sources_service_lease_valid(&cfg, &a));
    CHECK(!unlink(other));
    CHECK(sources_service_lease_valid(&cfg, &a));
    CHECK(!chmod(name, 0640));
    CHECK(!sources_service_lease_valid(&cfg, &a));
    CHECK(!chmod(name, 0600));
    CHECK(sources_service_lease_valid(&cfg, &a));
    CHECK(!rename(name, other));
    int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0);
    CHECK(!close(fd));
    CHECK(!sources_service_lease_valid(&cfg, &a));
    error = sources_service_lease_open(&cfg, &b);
    CHECK(error && !strcmp(error, "service_busy"));
    CHECK(!unlink(name));
    CHECK(!rename(other, name));
    CHECK(sources_service_lease_valid(&cfg, &a));
    sources_service_lease_close(&a);
    CHECK(!sources_service_lease_open(&cfg, &a));
    sources_service_lease_close(&a);
    CHECK(!rename(name, other));
    CHECK(!symlink(other, name));
    error = sources_service_lease_open(&cfg, &a);
    CHECK(error && !strcmp(error, "state_unavailable"));
    CHECK(!unlink(name));
    CHECK(!rename(other, name));
    CHECK(!sources_service_lease_open(&cfg, &a));
    struct stat bound;
    CHECK(!stat(state, &bound));
    cfg.state_pinned = 1;
    cfg.state_device = (size_t)bound.st_dev;
    cfg.state_inode = (size_t)bound.st_ino;
    char moved[512];
    snprintf(moved, sizeof(moved), "%s-held", state);
    CHECK(!rename(state, moved));
    CHECK(!mkdir(state, 0700));
    CHECK(!sources_service_lease_valid(&cfg, &a));
    error = sources_service_lease_open(&cfg, &b);
    CHECK(error && !strcmp(error, "state_unavailable"));
    CHECK(!rmdir(state));
    CHECK(!rename(moved, state));
    CHECK(sources_service_lease_valid(&cfg, &a));
    sources_service_lease_close(&a);
    CHECK(!sources_service_lease_open(&cfg, &a));
    sources_service_lease_close(&a);
    printf("PASS native service lease: %u checks; shared admission, "
           "private/symlink/hardlink/permission refusal, named replacement and "
           "state identity; %s\n",
           checks, state);
    return 0;
}
