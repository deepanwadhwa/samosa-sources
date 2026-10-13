#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "service_lease.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
static int private_file(const struct stat *s) {
    return S_ISREG(s->st_mode) && s->st_uid == geteuid() &&
           !(s->st_mode & 0077) && s->st_nlink == 1;
}
static int bound_file(int directory, int fd, const char *name) {
    struct stat held, named;
    return fd >= 0 && !fstat(fd, &held) && private_file(&held) &&
           !fstatat(directory, name, &named, AT_SYMLINK_NOFOLLOW) &&
           private_file(&named) && held.st_dev == named.st_dev &&
           held.st_ino == named.st_ino;
}
static const char *lock_file(int directory, const char *name, int *fd) {
    *fd = openat(directory, name,
                 O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (!bound_file(directory, *fd, name))
        return "state_unavailable";
    if (flock(*fd, LOCK_EX | LOCK_NB))
        return errno == EAGAIN || errno == EWOULDBLOCK ? "service_busy"
                                                       : "state_unavailable";
    return bound_file(directory, *fd, name) ? NULL : "state_unavailable";
}
void sources_service_lease_close(SourcesServiceLease *lease) {
    if (lease->queue >= 0)
        close(lease->queue);
    if (lease->service >= 0)
        close(lease->service);
    if (lease->directory >= 0)
        close(lease->directory);
    *lease = (SourcesServiceLease){-1, -1, -1};
}
int sources_service_lease_valid(const SourcesConfig *cfg,
                                const SourcesServiceLease *lease) {
    struct stat held, named;
    return cfg && cfg->state && lease && lease->directory >= 0 &&
           !fstat(lease->directory, &held) && S_ISDIR(held.st_mode) &&
           held.st_uid == geteuid() && !(held.st_mode & 0077) &&
           !lstat(cfg->state, &named) && S_ISDIR(named.st_mode) &&
           named.st_dev == held.st_dev && named.st_ino == held.st_ino &&
           (!cfg->state_pinned || ((size_t)held.st_dev == cfg->state_device &&
                                   (size_t)held.st_ino == cfg->state_inode)) &&
           bound_file(lease->directory, lease->service,
                      "service-runtime.lock") &&
           bound_file(lease->directory, lease->queue, "reindex-runtime.lock");
}
const char *sources_service_lease_open(const SourcesConfig *cfg,
                                       SourcesServiceLease *lease) {
    *lease = (SourcesServiceLease){-1, -1, -1};
    if (!cfg || !cfg->state || cfg->state[0] != '/')
        return "invalid_configuration";
    lease->directory =
        open(cfg->state, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat s;
    const char *error = NULL;
    if (lease->directory < 0 || fstat(lease->directory, &s) ||
        s.st_uid != geteuid() || (s.st_mode & 0077) ||
        (cfg->state_pinned && ((size_t)s.st_dev != cfg->state_device ||
                               (size_t)s.st_ino != cfg->state_inode)))
        error = "state_unavailable";
    if (!error)
        error = lock_file(lease->directory, "service-runtime.lock",
                          &lease->service);
    if (!error)
        error =
            lock_file(lease->directory, "reindex-runtime.lock", &lease->queue);
    if (!error && !sources_service_lease_valid(cfg, lease))
        error = "state_unavailable";
    if (error)
        sources_service_lease_close(lease);
    return error;
}
