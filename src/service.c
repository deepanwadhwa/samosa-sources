#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "inventory.h"
#include "json.h"
#include "service.h"
#include "service_lease.h"
#include "service_upload.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define PEERS 16
#define WAITERS 8
#define RESPONSE_MAX (8u * 1048576u)
typedef struct {
    int fd, phase, control;
    pid_t pid;
    double deadline;
    unsigned long order;
    size_t used;
    char bytes[SOURCES_REQUEST_MAX + 1];
    SourcesHttpRequest request;
} Peer;
typedef struct {
    int fd;
    double deadline;
    pid_t parent;
} Transport;
static volatile sig_atomic_t stopping;
static pid_t server_pid;
static SourcesServiceLease runtime_lease = {-1, -1, -1};
static const SourcesConfig *runtime_config;
typedef struct {
    const char *op;
    unsigned budget;
} BackgroundPhase;
static const char *background_phase[SOURCES_HTTP_PRINCIPALS_MAX];
static int background_failed[SOURCES_HTTP_PRINCIPALS_MAX];
/* This adapter runs in the dedicated Sources daemon process. Every child is
 * either its foreground/background worker or an adopted descendant of one.
 * Preserve known worker statuses before draining adopted descendants. */
static int reap_adopted(Peer peers[PEERS], pid_t *background_pid,
                        const BackgroundPhase phases[7], size_t phase_count,
                        size_t *owner, size_t *phase, double *due,
                        size_t owners, size_t interval, double current) {
    int pending = 0;
#ifdef __linux__
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid == 0) {
            pending = 1;
            break;
        }
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pid == *background_pid) {
            background_phase[*owner] = phases[*phase].op;
            background_failed[*owner] =
                !WIFEXITED(status) || WEXITSTATUS(status) != 0;
            *background_pid = 0;
            if (++*owner == owners) {
                *owner = 0;
                *phase = (*phase + 1) % phase_count;
            }
            *due = current + (double)interval / 1000;
        } else
            for (int i = 0; i < PEERS; i++)
                if (peers[i].pid == pid) {
                    close(peers[i].fd);
                    peers[i] = (Peer){.fd = -1};
                    break;
                }
    }
#else
    (void)peers;
    (void)background_pid;
    (void)phases;
    (void)phase_count;
    (void)owner;
    (void)phase;
    (void)due;
    (void)owners;
    (void)interval;
    (void)current;
#endif
    return pending;
}
#ifdef __linux__
static void stop_adopted(void) {
    char path[96];
    int n = snprintf(path, sizeof(path), "/proc/self/task/%ld/children",
                     (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(path))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    long pid;
    while (fscanf(f, "%ld", &pid) == 1)
        if (pid > 1)
            kill((pid_t)pid, SIGKILL);
    fclose(f);
}
#endif
static void stop_signal(int sig) {
    (void)sig;
    stopping = 1;
}
static double clock_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int nonblocking(int fd) {
    return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(fd, F_SETFL, O_NONBLOCK) == 0;
}
static int wait_socket(Transport *t, short events) {
    while (!stopping && (!t->parent || getppid() == t->parent) &&
           clock_now() < t->deadline) {
        struct pollfd p = {.fd = t->fd, .events = events};
        int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return 0;
        if (rc && (p.revents & events))
            return 1;
        if (rc && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return 0;
    }
    return 0;
}
static int send_bytes(Transport *t, const char *bytes, size_t n) {
    while (n) {
        ssize_t got = send(t->fd, bytes, n, 0);
        if (got > 0) {
            bytes += got;
            n -= (size_t)got;
        } else if (got < 0 && errno == EINTR)
            continue;
        else if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_socket(t, POLLOUT))
                return 0;
        } else
            return 0;
    }
    return 1;
}
static int response(Transport *t, int status, const char *id, const char *body,
                    size_t n) {
    char header[512];
    int k = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d Result\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n%s%s%s\r\n",
                     status, n, id && *id ? "X-Request-ID: " : "",
                     id && *id ? id : "", id && *id ? "\r\n" : "");
    return k > 0 && (size_t)k < sizeof(header) &&
           send_bytes(t, header, (size_t)k) && send_bytes(t, body, n);
}
static void refusal(Peer *p, int status, const char *code) {
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",
                     code);
    Transport t = {.fd = p->fd, .deadline = clock_now() + .05};
    if (n > 0 && (size_t)n < sizeof(body))
        (void)response(&t, status, p->request.request_id, body, (size_t)n);
}
static int disconnected(void *context) {
    Transport *t = context;
    if (stopping || (t->parent && getppid() != t->parent) ||
        (t->parent &&
         !sources_service_lease_valid(runtime_config, &runtime_lease)))
        return 1;
    if (t->fd < 0)
        return 0;
    char c;
    ssize_t n = recv(t->fd, &c, 1, MSG_PEEK);
    return n == 0 ||
           (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}
static int control_request(const char *bytes, size_t n) {
    Json j;
    if (!json_parse(&j, bytes, n))
        return 0;
    char *op = json_string(&j, json_get(&j, 0, "op"));
    int control =
        op && (!strcmp(op, "operation_cancel") ||
               !strcmp(op, "operation_get") || !strcmp(op, "describe"));
    free(op);
    return control;
}
static int socket_read(void *context, void *bytes, size_t capacity,
                       size_t *got) {
    Transport *t = context;
    *got = 0;
    while (!disconnected(t)) {
        ssize_t n = recv(t->fd, bytes, capacity, 0);
        if (n > 0) {
            *got = (size_t)n;
            return 0;
        }
        if (!n)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return 1;
        if (!wait_socket(t, POLLIN))
            return 1;
    }
    return 1;
}
static void run_worker(const SourcesConfig *cfg, const SourcesHttpAuth *auth,
                       Peer *p) {
    Transport transport = {
        .fd = p->fd, .deadline = p->deadline, .parent = server_pid};
    SourcesConfig work = *cfg;
    work.owner = auth->principals[p->request.owner_index].owner;
    work.deadline_monotonic = p->deadline;
    work.cancelled = disconnected;
    work.cancellation = &transport;
    char *body = NULL;
    size_t n = 0;
    FILE *out = open_memstream(&body, &n);
    if (!out) {
        refusal(p, 503, "resource_limit");
        _exit(1);
    }
    int rc;
    if (p->request.health) {
        size_t owner = p->request.owner_index;
        fprintf(
            out,
            "{\"contract_version\":1,\"component\":\"samosa-sources\","
            "\"healthy\":true,\"background\":{\"enabled\":%s,\"last_phase\":",
            cfg->root ? "true" : "false");
        if (background_phase[owner])
            json_quote(out, background_phase[owner],
                       strlen(background_phase[owner]));
        else
            fputs("null", out);
        fprintf(out, ",\"last_attempt_failed\":%s}}\n",
                background_failed[owner] ? "true" : "false");
        rc = 0;
    } else if (p->request.upload) {
        if (p->request.expect_continue &&
            !send_bytes(&transport, "HTTP/1.1 100 Continue\r\n\r\n", 25))
            _exit(1);
        rc = sources_service_upload(&work, &p->request, socket_read, &transport,
                                    out);
    } else
        rc = sources_dispatch(&work, p->bytes, p->used, out);
    if (fclose(out) || n > RESPONSE_MAX) {
        free(body);
        refusal(p, 503, "resource_limit");
        _exit(1);
    }
    (void)response(&transport, rc ? 400 : 200, p->request.request_id, body, n);
    free(body);
    close(p->fd);
    _exit(0);
}
static void close_peer(Peer *p) {
    if (p->fd >= 0)
        close(p->fd);
    *p = (Peer){.fd = -1};
}
static void start_worker(const SourcesConfig *cfg, const SourcesHttpAuth *auth,
                         Peer peers[PEERS], int selected, int listener) {
    Peer *p = &peers[selected];
    pid_t child = fork();
    if (child == 0) {
        close(listener);
        for (int i = 0; i < PEERS; i++)
            if (i != selected && peers[i].fd >= 0)
                close(peers[i].fd);
        stopping = 0;
        run_worker(cfg, auth, p);
    }
    if (child < 0) {
        refusal(p, 503, "resource_limit");
        close_peer(p);
    } else {
        p->pid = child;
        p->phase = 3;
    }
}
static pid_t start_background(const SourcesConfig *cfg,
                              const SourcesHttpAuth *auth, Peer peers[PEERS],
                              int listener, size_t owner,
                              const BackgroundPhase *phase, double deadline) {
    pid_t child = fork();
    if (!child) {
        close(listener);
        for (int i = 0; i < PEERS; i++)
            if (peers[i].fd >= 0)
                close(peers[i].fd);
        stopping = 0;
        Transport transport = {
            .fd = -1, .deadline = deadline, .parent = server_pid};
        SourcesConfig work = *cfg;
        work.owner = auth->principals[owner].owner;
        work.cancelled = disconnected;
        work.cancellation = &transport;
        work.deadline_monotonic = deadline;
        char request[128];
        int k = snprintf(request, sizeof(request),
                         "{\"contract_version\":1,\"op\":\"%s\"}", phase->op);
        if (k < 0 || (size_t)k >= sizeof(request))
            _exit(2);
        char *body = NULL;
        size_t bytes;
        FILE *out = open_memstream(&body, &bytes);
        if (!out)
            _exit(2);
        int rc = sources_dispatch(&work, request, (size_t)k, out);
        if (fclose(out) || bytes > RESPONSE_MAX) {
            free(body);
            _exit(2);
        }
        Json reply;
        size_t contract;
        int valid = body && json_parse(&reply, body, bytes) &&
                    reply.tokens[0].type == JT_OBJECT &&
                    json_uint(&reply, json_get(&reply, 0, "contract_version"),
                              &contract) &&
                    contract == 1;
        free(body);
        _exit(valid ? (rc ? 1 : 0) : 2);
    }
    return child;
}
int sources_service_run(const SourcesConfig *cfg, const SourcesHttpAuth *auth,
                        unsigned port, size_t interval, FILE *ready) {
    if (!cfg || !auth || !auth->count ||
        auth->count > SOURCES_HTTP_PRINCIPALS_MAX || port > 65535 || !ready ||
        interval < 100 || interval > 60000)
        return 2;
    if (!cfg->state || cfg->state[0] != '/')
        return 2;
    SourcesConfig pinned = *cfg;
    pinned.owner = auth->principals[0].owner;
    char *probe = NULL;
    size_t probe_length = 0;
    FILE *probe_out = open_memstream(&probe, &probe_length);
    if (!probe_out)
        return 2;
    const char startup[] = "{\"contract_version\":1,\"op\":\"operation_get\","
                           "\"operation_id\":\"service-startup\"}";
    int probe_rc =
        sources_dispatch(&pinned, startup, sizeof(startup) - 1, probe_out);
    int probe_failed = fclose(probe_out);
    Json result;
    char *error = NULL;
    if (!probe_failed && probe && json_parse(&result, probe, probe_length))
        error = json_string(
            &result, json_get(&result, json_get(&result, 0, "error"), "code"));
    int valid_probe =
        !probe_failed && probe && json_parse(&result, probe, probe_length) &&
        result.tokens[0].type == JT_OBJECT &&
        (!probe_rc || (error && !strcmp(error, "unknown_operation")));
    free(error);
    free(probe);
    if (!valid_probe)
        return 2;
    int state =
        open(cfg->state, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat bound;
    if (state < 0 || fstat(state, &bound) || bound.st_uid != geteuid() ||
        (bound.st_mode & 0077)) {
        if (state >= 0)
            close(state);
        return 2;
    }
    close(state);
    pinned.state_pinned = 1;
    pinned.state_device = (size_t)bound.st_dev;
    pinned.state_inode = (size_t)bound.st_ino;
    char root_scope[65];
    if (cfg->root) {
        if (inventory_scope_identity(cfg, "", root_scope))
            return 2;
        pinned.root_scope_pin = root_scope;
    }
    cfg = &pinned;
    server_pid = getpid();
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || !nonblocking(listener)) {
        if (listener >= 0)
            close(listener);
        return 2;
    }
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(0x7f000001u),
                                  .sin_port = htons((unsigned short)port)};
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listener, PEERS)) {
        close(listener);
        return 2;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_size)) {
        close(listener);
        return 2;
    }
    struct sigaction action = {0}, old_term, old_int, old_pipe;
    sigemptyset(&action.sa_mask);
    action.sa_handler = stop_signal;
    if (sigaction(SIGTERM, &action, &old_term)) {
        close(listener);
        return 2;
    }
    if (sigaction(SIGINT, &action, &old_int)) {
        sigaction(SIGTERM, &old_term, NULL);
        close(listener);
        return 2;
    }
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, &old_pipe)) {
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGINT, &old_int, NULL);
        close(listener);
        return 2;
    }
    const char *lease_error = sources_service_lease_open(cfg, &runtime_lease);
    if (lease_error) {
        fprintf(ready, "{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",
                lease_error);
        close(listener);
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGPIPE, &old_pipe, NULL);
        return 2;
    }
    runtime_config = cfg;
#ifdef __linux__
    int previous_subreaper = 0;
    if (prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper) ||
        prctl(PR_SET_CHILD_SUBREAPER, 1)) {
        sources_service_lease_close(&runtime_lease);
        runtime_config = NULL;
        close(listener);
        sigaction(SIGTERM, &old_term, NULL);
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGPIPE, &old_pipe, NULL);
        return 2;
    }
#endif
    stopping = 0;
    fprintf(ready,
            "{\"contract_version\":1,\"service\":\"samosa-sources\",\"listen\":"
            "\"127.0.0.1\",\"port\":%u}\n",
            (unsigned)ntohs(address.sin_port));
    fflush(ready);
    Peer peers[PEERS];
    for (int i = 0; i < PEERS; i++)
        peers[i] = (Peer){.fd = -1};
    BackgroundPhase phases[7];
    size_t phase_count = 0;
    if (cfg->root) {
        if (cfg->discovery_interval_ms)
            phases[phase_count++] = (BackgroundPhase){"index_refresh_step", 5};
        phases[phase_count++] = (BackgroundPhase){"index_queue_step", 5};
        if (cfg->chutni_publish)
            phases[phase_count++] =
                (BackgroundPhase){"index_publish_queue_step", 5};
        if (cfg->inference)
            phases[phase_count++] =
                (BackgroundPhase){"audio_index_queue_step", 60};
        if (cfg->visual_inference)
            phases[phase_count++] =
                (BackgroundPhase){"visual_index_queue_step", 60};
        if (cfg->summary_inference)
            phases[phase_count++] =
                (BackgroundPhase){"index_enrich_queue_step", 60};
        if (cfg->embedding && cfg->chutni_publish)
            phases[phase_count++] =
                (BackgroundPhase){"index_embed_queue_step", 60};
    }
    memset(background_phase, 0, sizeof(background_phase));
    memset(background_failed, 0, sizeof(background_failed));
    pid_t background_pid = 0;
    size_t background_owner = 0, phase_index = 0;
    double background_due = clock_now(), background_deadline = 0;
    int last_background = 0;
    int terminal_failure = 0;
    unsigned long order = 0;
    size_t last_owner = auth->count - 1;
    double shutdown_at = 0;
    for (;;) {
        double current = clock_now();
        if (!sources_service_lease_valid(cfg, &runtime_lease)) {
            stopping = 1;
            terminal_failure = 1;
        }
        if (cfg->root) {
            char actual[65];
            if (inventory_scope_identity(cfg, "", actual) ||
                strcmp(actual, root_scope))
                stopping = terminal_failure = 1;
        }
        if (stopping && !shutdown_at) {
            shutdown_at = current + 3;
            close(listener);
            listener = -1;
            if (background_pid)
                kill(background_pid, SIGTERM);
            for (int i = 0; i < PEERS; i++) {
                if (peers[i].pid)
                    kill(peers[i].pid, SIGTERM);
                else if (peers[i].fd >= 0) {
                    refusal(&peers[i], 503, "service_stopping");
                    close_peer(&peers[i]);
                }
            }
        }
        if (background_pid) {
            int status;
            pid_t got = waitpid(background_pid, &status, WNOHANG);
            if (got == background_pid || (got < 0 && errno == ECHILD)) {
                background_phase[background_owner] = phases[phase_index].op;
                background_failed[background_owner] =
                    got < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
                background_pid = 0;
                if (++background_owner == auth->count) {
                    background_owner = 0;
                    phase_index = (phase_index + 1) % phase_count;
                }
                background_due = current + (double)interval / 1000;
            } else if (current >= background_deadline + 2 ||
                       (shutdown_at && current >= shutdown_at))
                kill(background_pid, SIGKILL);
        }
        int active = background_pid ? 1 : 0, controls = 0, queued = 0;
        for (int i = 0; i < PEERS; i++) {
            Peer *p = &peers[i];
            if (p->pid) {
                int status;
                pid_t got = waitpid(p->pid, &status, WNOHANG);
                if (got == p->pid || (got < 0 && errno == ECHILD)) {
                    close_peer(p);
                    continue;
                }
                if (current >= p->deadline + 3 ||
                    (shutdown_at && current >= shutdown_at))
                    kill(p->pid, SIGKILL);
                if (p->control)
                    controls++;
                else
                    active++;
            } else if (p->fd >= 0 && current >= p->deadline) {
                refusal(p, 408, "deadline_exceeded");
                close_peer(p);
            } else if (p->phase == 2) {
                char byte;
                ssize_t got = recv(p->fd, &byte, 1, MSG_PEEK);
                if (got == 0 || (got < 0 && errno != EAGAIN &&
                                 errno != EWOULDBLOCK && errno != EINTR))
                    close_peer(p);
                else if (!p->control)
                    queued++;
            }
        }
        int adopted_pending = reap_adopted(
            peers, &background_pid, phases, phase_count, &background_owner,
            &phase_index, &background_due, auth->count, interval, current);
#ifdef __linux__
        if (shutdown_at && current >= shutdown_at && adopted_pending)
            stop_adopted();
#endif
        if (shutdown_at && !active && !controls && !adopted_pending)
            break;
        if (!shutdown_at) {
            if (!controls) {
                int selected = -1;
                for (int i = 0; i < PEERS; i++)
                    if (peers[i].phase == 2 && peers[i].control &&
                        (selected < 0 ||
                         peers[i].order < peers[selected].order))
                        selected = i;
                if (selected >= 0)
                    start_worker(cfg, auth, peers, selected, listener);
            }
            while (active < 2) {
                if (phase_count && !background_pid &&
                    current >= background_due &&
                    (!queued || !last_background)) {
                    background_deadline = current + phases[phase_index].budget;
                    background_pid = start_background(
                        cfg, auth, peers, listener, background_owner,
                        &phases[phase_index], background_deadline);
                    if (background_pid < 0) {
                        background_pid = 0;
                        background_due = current + (double)interval / 1000;
                        background_failed[background_owner] = 1;
                    } else {
                        active++;
                        last_background = 1;
                    }
                    if (active == 2)
                        break;
                }
                int selected = -1;
                for (size_t k = 1; k <= auth->count && selected < 0; k++) {
                    size_t owner = (last_owner + k) % auth->count;
                    for (int i = 0; i < PEERS; i++)
                        if (peers[i].phase == 2 && !peers[i].control &&
                            peers[i].request.owner_index == owner &&
                            (selected < 0 ||
                             peers[i].order < peers[selected].order))
                            selected = i;
                }
                if (selected < 0)
                    break;
                last_owner = peers[selected].request.owner_index;
                start_worker(cfg, auth, peers, selected, listener);
                last_background = 0;
                active++;
                queued--;
            }
        }
        struct pollfd polls[PEERS + 1];
        polls[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        for (int i = 0; i < PEERS; i++)
            polls[i + 1] =
                (struct pollfd){.fd = peers[i].pid ? -1 : peers[i].fd,
                                .events = peers[i].phase == 2 ? 0 : POLLIN};
        int polled = poll(polls, PEERS + 1, 25);
        if (polled < 0 && errno != EINTR)
            stopping = 1;
        if (listener >= 0 && (polls[0].revents & POLLIN)) {
            int fd = accept(listener, NULL, NULL), free_slot = -1;
            for (int i = 0; i < PEERS; i++)
                if (peers[i].fd < 0) {
                    free_slot = i;
                    break;
                }
            if (fd >= 0) {
                if (!nonblocking(fd) || free_slot < 0)
                    close(fd);
                else
                    peers[free_slot] =
                        (Peer){.fd = fd, .deadline = current + 5};
            }
        }
        for (int i = 0; i < PEERS; i++) {
            Peer *p = &peers[i];
            if (p->fd < 0 || p->pid)
                continue;
            if (p->phase == 2) {
                if (polls[i + 1].revents & (POLLHUP | POLLERR | POLLNVAL))
                    close_peer(p);
                continue;
            }
            if (polls[i + 1].revents & (POLLERR | POLLNVAL)) {
                close_peer(p);
                continue;
            }
            if (!(polls[i + 1].revents & (POLLIN | POLLHUP)))
                continue;
            int bad = 0;
            while (!p->phase && p->used < SOURCES_HTTP_HEADER_MAX) {
                ssize_t n = recv(p->fd, p->bytes + p->used, 1, 0);
                if (n == 0) {
                    bad = 1;
                    break;
                }
                if (n < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK &&
                        errno != EINTR)
                        bad = 1;
                    break;
                }
                p->used++;
                if (p->used >= 4 &&
                    !memcmp(p->bytes + p->used - 4, "\r\n\r\n", 4)) {
                    int status = sources_http_header(auth, p->bytes, p->used,
                                                     &p->request);
                    if (status) {
                        refusal(p, status, "request_refused");
                        bad = 1;
                        break;
                    }
                    p->used = 0;
                    p->phase = p->request.upload || p->request.health ? 2 : 1;
                    p->control = p->request.health;
                    p->order = ++order;
                    p->deadline = current + 120;
                    if (p->request.upload && queued >= WAITERS) {
                        refusal(p, 503, "admission_busy");
                        bad = 1;
                        break;
                    }
                    if (p->request.upload)
                        queued++;
                    else if (p->request.health) {
                        int waiting = 0;
                        for (int k = 0; k < PEERS; k++)
                            if (k != i && peers[k].phase == 2 &&
                                peers[k].control)
                                waiting++;
                        if (waiting >= 2) {
                            refusal(p, 503, "admission_busy");
                            bad = 1;
                        }
                    } else if (p->request.expect_continue) {
                        Transport t = {.fd = p->fd, .deadline = current + .05};
                        if (!send_bytes(&t, "HTTP/1.1 100 Continue\r\n\r\n",
                                        25))
                            bad = 1;
                    }
                }
            }
            if (!p->phase && p->used == SOURCES_HTTP_HEADER_MAX) {
                refusal(p, 431, "header_limit");
                bad = 1;
            }
            if (!bad && p->phase == 1) {
                ssize_t n = recv(p->fd, p->bytes + p->used,
                                 p->request.content_length - p->used, 0);
                if (n > 0)
                    p->used += (size_t)n;
                else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK &&
                                errno != EINTR))
                    bad = 1;
                if (p->used == p->request.content_length) {
                    Json request;
                    if (json_parse(&request, p->bytes, p->used) &&
                        request.tokens[0].type == JT_OBJECT &&
                        json_get(&request, 0, "owner") >= 0) {
                        refusal(p, 400, "invalid_request");
                        close_peer(p);
                        continue;
                    }
                    if (json_parse(&request, p->bytes, p->used) &&
                        request.tokens[0].type == JT_OBJECT) {
                        char *op =
                            json_string(&request, json_get(&request, 0, "op"));
                        size_t budget = op && !strcmp(op, "analyze_video")
                                            ? 120000u
                                            : 60000u;
                        free(op);
                        size_t requested;
                        if (json_uint(&request,
                                      json_get(&request, 0, "timeout_ms"),
                                      &requested) &&
                            requested && requested <= budget)
                            budget = requested;
                        p->deadline -= 120.0 - (double)budget / 1000.0;
                    }
                    p->control = control_request(p->bytes, p->used);
                    int control_waiters = 0;
                    for (int k = 0; k < PEERS; k++)
                        if (peers[k].phase == 2 && peers[k].control)
                            control_waiters++;
                    if ((!p->control && queued >= WAITERS) ||
                        (p->control && control_waiters >= 2)) {
                        refusal(p, 503, "admission_busy");
                        bad = 1;
                    } else {
                        p->bytes[p->used] = 0;
                        p->phase = 2;
                        p->order = ++order;
                        if (!p->control)
                            queued++;
                    }
                }
            }
            if (bad)
                close_peer(p);
        }
    }
    for (int i = 0; i < PEERS; i++)
        close_peer(&peers[i]);
    if (listener >= 0)
        close(listener);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGPIPE, &old_pipe, NULL);
    sources_service_lease_close(&runtime_lease);
    runtime_config = NULL;
#ifdef __linux__
    (void)prctl(PR_SET_CHILD_SUBREAPER, previous_subreaper);
#endif
    return terminal_failure ? 1 : 0;
}
