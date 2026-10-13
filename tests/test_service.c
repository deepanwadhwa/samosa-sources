#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "json.h"
#include "service_lease.h"
#include "sources.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x)                                                               \
    do {                                                                       \
        checks++;                                                              \
        if (!(x)) {                                                            \
            fprintf(stderr, "FAIL service line%d: %s\n", __LINE__, #x);        \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
static const char *tokens[] = {
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
static double now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int wait_fd(int fd, short event, double deadline) {
    while (now() < deadline) {
        struct pollfd p = {.fd = fd, .events = event};
        int n = poll(&p, 1, 50);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n >= 0);
        if (n && (p.revents & (event | POLLHUP | POLLERR)))
            return 1;
    }
    return 0;
}
static void send_all(int fd, const void *body, size_t length) {
    const char *p = body;
    double deadline = now() + 30;
    while (length) {
        CHECK(wait_fd(fd, POLLOUT, deadline));
        ssize_t n = send(fd, p, length, 0);
        if (n < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        CHECK(n > 0);
        p += n;
        length -= (size_t)n;
    }
}
static char *receive(int fd) {
    char *body = calloc(262145, 1);
    CHECK(body);
    size_t used = 0;
    double deadline = now() + 10;
    for (;;) {
        CHECK(wait_fd(fd, POLLIN, deadline));
        ssize_t n = recv(fd, body + used, 262144 - used, 0);
        if (n < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        CHECK(n >= 0);
        CHECK(n != 0);
        used += (size_t)n;
        CHECK(used < 262144);
        char *end = strstr(body, "\r\n\r\n");
        if (end) {
            char *length = strstr(body, "\r\nContent-Length: ");
            size_t bytes;
            CHECK(length && length < end &&
                  sscanf(length, "\r\nContent-Length: %zu", &bytes) == 1);
            if (used >= (size_t)(end + 4 - body) + bytes)
                break;
        }
    }
    close(fd);
    return body;
}
static int connect_to(unsigned port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_port = htons((unsigned short)port),
                            .sin_addr.s_addr = htonl(0x7f000001u)};
    CHECK(!connect(fd, (struct sockaddr *)&a, sizeof(a)));
    CHECK(!fcntl(fd, F_SETFL, O_NONBLOCK));
    return fd;
}
static int request_head(unsigned port, int owner, const char *route, size_t n,
                        const char *extra) {
    int fd = connect_to(port);
    char header[2048];
    int k = snprintf(
        header, sizeof(header),
        "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nAuthorization: Bearer "
        "%s\r\nContent-Length: %zu\r\nContent-Type: %s\r\n%s\r\n",
        route, port, owner < 0 ? "wrong" : tokens[owner], n,
        !strcmp(route, "/v1/uploads")
            ? "multipart/form-data; boundary=source-boundary"
            : "application/json",
        extra);
    CHECK(k > 0 && (size_t)k < sizeof(header));
    send_all(fd, header, (size_t)k);
    return fd;
}
static char *call(unsigned port, int owner, const char *body) {
    int fd = request_head(port, owner, "/v1/sources", strlen(body),
                          "X-Request-ID: native-service-test\r\n");
    send_all(fd, body, strlen(body));
    return receive(fd);
}
static unsigned start(const char *exe, const char *root, const char *state,
                      const char *auth, pid_t *pid) {
    int pipefd[2];
    CHECK(!pipe(pipefd));
    *pid = fork();
    CHECK(*pid >= 0);
    if (!*pid) {
        close(pipefd[0]);
        CHECK(dup2(pipefd[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(pipefd[1]);
        execl(exe, exe, "--serve", "--serve-port", "0", "--serve-auth-file",
              auth, "--root", root, "--state", state, "--discovery-interval-ms",
              "100", "--index-interval-ms", "100", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char line[1024];
    size_t n = 0;
    double deadline = now() + 5;
    while (n < sizeof(line) - 1) {
        CHECK(wait_fd(pipefd[0], POLLIN, deadline));
        ssize_t got = read(pipefd[0], line + n, 1);
        CHECK(got == 1);
        if (line[n++] == '\n')
            break;
    }
    close(pipefd[0]);
    line[n] = 0;
    Json j;
    size_t port;
    CHECK(json_parse(&j, line, n));
    CHECK(json_uint(&j, json_get(&j, 0, "port"), &port));
    CHECK(port && port <= 65535);
    return (unsigned)port;
}
static void stop(pid_t pid) {
    double deadline = now() + 6;
    CHECK(!kill(pid, SIGTERM));
    int status;
    pid_t got;
    do {
        got = waitpid(pid, &status, WNOHANG);
        CHECK(got >= 0);
        if (!got) {
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    } while (!got && now() < deadline);
    CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void refused_config(const char *exe, const char *auth) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        execl(exe, exe, "--serve", "--serve-port", "0", "--serve-auth-file",
              auth, (char *)NULL);
        _exit(127);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
}
static void refused_instance(const char *exe, const char *root,
                             const char *state, const char *auth, int queue) {
    int pipefd[2];
    CHECK(!pipe(pipefd));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(pipefd[0]);
        CHECK(dup2(pipefd[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(pipefd[1]);
        if (queue)
            execl(exe, exe, "--index-run", "--owner", "alice", "--root", root,
                  "--state", state, (char *)NULL);
        else
            execl(exe, exe, "--serve", "--serve-port", "0", "--serve-auth-file",
                  auth, "--root", root, "--state", state, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char body[1024];
    ssize_t n;
    CHECK(wait_fd(pipefd[0], POLLIN, now() + 3));
    n = read(pipefd[0], body, sizeof(body) - 1);
    CHECK(n > 0);
    body[n] = 0;
    close(pipefd[0]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (queue ? 1 : 2));
    CHECK(strstr(body, queue ? "queue_busy" : "service_busy"));
}
static void exited(pid_t pid, int code) {
    int status;
    pid_t got;
    double deadline = now() + 6;
    do {
        got = waitpid(pid, &status, WNOHANG);
        CHECK(got >= 0);
        if (!got) {
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    } while (!got && now() < deadline);
    CHECK(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == code);
}
static void await_admission(const char *state) {
    SourcesConfig cfg = {.state = state};
    SourcesServiceLease lease;
    double deadline = now() + 6;
    while (now() < deadline) {
        const char *error = sources_service_lease_open(&cfg, &lease);
        if (!error) {
            sources_service_lease_close(&lease);
            return;
        }
        CHECK(!strcmp(error, "service_busy"));
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    CHECK(0);
}
static char *reply_string(const char *response, const char *name) {
    const char *body = strstr(response, "\r\n\r\n");
    CHECK(body);
    Json j;
    CHECK(json_parse(&j, body + 4, strlen(body + 4)));
    char *value = json_string(&j, json_get(&j, 0, name));
    CHECK(value);
    return value;
}
static void await_index(unsigned port, int owner, const char *job) {
    char request[256];
    snprintf(request, sizeof(request),
             "{\"contract_version\":1,\"op\":\"index_get\",\"job_id\":\"%s\"}",
             job);
    double deadline = now() + 12;
    while (now() < deadline) {
        char *response = call(port, owner, request);
        CHECK(strstr(response, "200"));
        char *body = strstr(response, "\r\n\r\n");
        CHECK(body);
        Json j;
        size_t indexed;
        CHECK(json_parse(&j, body + 4, strlen(body + 4)));
        CHECK(json_uint(&j, json_get(&j, 0, "indexed_items"), &indexed));
        free(response);
        if (indexed == 1)
            return;
        struct timespec delay = {0, 100000000};
        nanosleep(&delay, NULL);
    }
    CHECK(0);
}
static char *multipart(const void *payload, size_t n, size_t *total) {
    const char *prefix =
        "--source-boundary\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"ignored.txt\"\r\nContent-Type: text/plain\r\n\r\n";
    const char *suffix = "\r\n--source-boundary--\r\n";
    *total = strlen(prefix) + n + strlen(suffix);
    char *bytes = malloc(*total);
    CHECK(bytes);
    memcpy(bytes, prefix, strlen(prefix));
    memcpy(bytes + strlen(prefix), payload, n);
    memcpy(bytes + strlen(prefix) + n, suffix, strlen(suffix));
    return bytes;
}
static void interim(int fd) {
    char response[25];
    size_t n = 0;
    double deadline = now() + 3;
    while (n < sizeof(response)) {
        CHECK(wait_fd(fd, POLLIN, deadline));
        ssize_t got = recv(fd, response + n, sizeof(response) - n, 0);
        CHECK(got > 0);
        n += (size_t)got;
    }
    CHECK(!memcmp(response, "HTTP/1.1 100 Continue\r\n\r\n", 25));
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    alarm(180);
    signal(SIGPIPE, SIG_IGN);
    umask(077);
    char root[] = "/tmp/samosa-service-qa-XXXXXX";
    CHECK(mkdtemp(root));
    char state[512], auth[512];
    snprintf(state, sizeof(state), "%s/state", root);
    snprintf(auth, sizeof(auth), "%s/auth.json", root);
    FILE *f = fopen(auth, "w");
    CHECK(f);
    fprintf(f,
            "[{\"owner\":\"alice\",\"token\":\"%s\"},{\"owner\":\"bob\","
            "\"token\":\"%s\"}]",
            tokens[0], tokens[1]);
    CHECK(!fclose(f));
    CHECK(!chmod(auth, 0644));
    refused_config(argv[1], auth);
    CHECK(!chmod(auth, 0600));
    char link[512];
    snprintf(link, sizeof(link), "%s/auth-link", root);
    CHECK(!symlink(auth, link));
    refused_config(argv[1], link);
    for (int i = 0; i < 2; i++) {
        char directory[512], file[512];
        snprintf(directory, sizeof(directory), "%s/%s", root,
                 i ? "bob-files" : "alice-files");
        CHECK(!mkdir(directory, 0700));
        int n = snprintf(file, sizeof(file), "%s/data.txt", directory);
        CHECK(n > 0 && (size_t)n < sizeof(file));
        f = fopen(file, "w");
        CHECK(f);
        CHECK(fputs(i ? "beta owned corpus\n" : "alpha owned corpus\n", f) >=
              0);
        CHECK(!fclose(f));
    }
    pid_t pid;
    unsigned port = start(argv[1], root, state, auth, &pid);
    refused_instance(argv[1], root, state, auth, 0);
    refused_instance(argv[1], root, state, auth, 1);
    char *jobs[2];
    for (int i = 0; i < 2; i++) {
        char request[256];
        snprintf(request, sizeof(request),
                 "{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"%"
                 "s\",\"operation_id\":\"headless-%d\"}",
                 i ? "bob-files" : "alice-files", i);
        char *response = call(port, i, request);
        CHECK(strstr(response, "200"));
        jobs[i] = reply_string(response, "job_id");
        free(response);
    }
    for (int i = 0; i < 2; i++)
        await_index(port, i, jobs[i]);
    char foreign[256];
    snprintf(foreign, sizeof(foreign),
             "{\"contract_version\":1,\"op\":\"index_get\",\"job_id\":\"%s\"}",
             jobs[1]);
    char *foreign_response = call(port, 0, foreign);
    CHECK(strstr(foreign_response, "400") &&
          strstr(foreign_response, "unknown_index"));
    free(foreign_response);
    char *response =
        call(port, 0, "{\"contract_version\":1,\"op\":\"describe\"}");
    CHECK(strstr(response, "HTTP/1.1 200") &&
          strstr(response, "samosa-sources") &&
          strstr(response, "X-Request-ID: native-service-test"));
    free(response);
    int health = connect_to(port);
    char health_header[512];
    int k = snprintf(health_header, sizeof(health_header),
                     "GET /healthz HTTP/1.1\r\nHost: "
                     "127.0.0.1\r\nAuthorization: Bearer %s\r\n\r\n",
                     tokens[0]);
    CHECK(k > 0);
    send_all(health, health_header, (size_t)k);
    response = receive(health);
    CHECK(strstr(response, "200") && strstr(response, "\"healthy\":true"));
    free(response);
    int denied =
        request_head(port, -1, "/v1/uploads", SOURCES_INPUT_MAX,
                     "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    response = receive(denied);
    CHECK(strstr(response, "401") && !strstr(response, "100 Continue"));
    free(response);
    response =
        call(port, 0,
             "{\"contract_version\":1,\"op\":\"describe\",\"owner\":\"bob\"}");
    CHECK(strstr(response, "400") && strstr(response, "invalid_request"));
    free(response);
    size_t total;
    const char payload[] = "owned native multipart data\n";
    char *body = multipart(payload, sizeof(payload) - 1, &total);
    int upload =
        request_head(port, 0, "/v1/uploads", total,
                     "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    interim(upload);
    send_all(upload, body, total);
    response = receive(upload);
    CHECK(strstr(response, "200"));
    char *json = strstr(response, "\r\n\r\n");
    CHECK(json);
    json += 4;
    Json j;
    CHECK(json_parse(&j, json, strlen(json)));
    char *source = json_string(&j, json_get(&j, 0, "source_id"));
    char *version = json_string(&j, json_get(&j, 0, "version"));
    CHECK(source && version);
    free(response);
    char read[512];
    snprintf(read, sizeof(read),
             "{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":27}",
             source, version);
    response = call(port, 0, read);
    CHECK(strstr(response, "200") &&
          strstr(response, "owned native multipart data"));
    free(response);
    response = call(port, 1, read);
    CHECK(strstr(response, "400") &&
          !strstr(response, "owned native multipart data"));
    free(response);
    body[total - 4] = 'x';
    upload = request_head(port, 0, "/v1/uploads", total,
                          "X-Sources-Format: text\r\n");
    send_all(upload, body, total);
    response = receive(upload);
    CHECK(strstr(response, "400") && strstr(response, "upload_read_failed"));
    free(response);
    free(body);
    const char duplicate[] =
        "first\r\n--source-boundary\r\nContent-Disposition: form-data; "
        "name=\"file\"\r\n\r\nsecond";
    body = multipart(duplicate, sizeof(duplicate) - 1, &total);
    upload = request_head(port, 0, "/v1/uploads", total,
                          "X-Sources-Format: text\r\n");
    send_all(upload, body, total);
    response = receive(upload);
    CHECK(strstr(response, "400"));
    free(response);
    free(body);
    /* Two admitted readers wait on bodies; control bypass and cross-owner
     * round-robin progress are observed from100Continue at actual admission. */
    int holds[2];
    for (int i = 0; i < 2; i++) {
        holds[i] =
            request_head(port, 0, "/v1/uploads", 4096,
                         "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
        interim(holds[i]);
    }
    response = call(port, 1,
                    "{\"contract_version\":1,\"op\":\"operation_cancel\","
                    "\"operation_id\":\"foreign-id\"}");
    CHECK(strstr(response, "unknown_operation"));
    free(response);
    int queued[3];
    queued[0] =
        request_head(port, 0, "/v1/uploads", 4096,
                     "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    queued[1] =
        request_head(port, 0, "/v1/uploads", 4096,
                     "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    queued[2] =
        request_head(port, 1, "/v1/uploads", 4096,
                     "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    struct timespec delay = {0, 200000000};
    nanosleep(&delay, NULL);
    struct pollfd waiting[3];
    for (int i = 0; i < 3; i++)
        waiting[i] = (struct pollfd){.fd = queued[i], .events = POLLIN};
    CHECK(poll(waiting, 3, 100) == 0);
    close(holds[0]);
    interim(queued[2]);
    CHECK(!wait_fd(queued[0], POLLIN, now() + .1));
    close(queued[2]);
    interim(queued[0]);
    CHECK(!wait_fd(queued[1], POLLIN, now() + .1));
    close(queued[0]);
    close(queued[1]);
    close(holds[1]);
    nanosleep(&delay, NULL);
    for (int i = 0; i < 2; i++) {
        holds[i] =
            request_head(port, 0, "/v1/uploads", 4096,
                         "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
        interim(holds[i]);
    }
    int overload[8];
    for (int i = 0; i < 8; i++)
        overload[i] =
            request_head(port, i % 2, "/v1/uploads", 4096,
                         "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    nanosleep(&delay, NULL);
    int rejected =
        request_head(port, 1, "/v1/uploads", 4096,
                     "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    response = receive(rejected);
    CHECK(strstr(response, "503") && strstr(response, "admission_busy") &&
          !strstr(response, "100 Continue"));
    free(response);
    response = call(port, 1,
                    "{\"contract_version\":1,\"op\":\"operation_get\","
                    "\"operation_id\":\"unknown-id\"}");
    CHECK(strstr(response, "unknown_operation"));
    free(response);
    for (int i = 0; i < 8; i++)
        close(overload[i]);
    nanosleep(&delay, NULL);
    int released =
        request_head(port, 1, "/v1/uploads", 4096,
                     "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    CHECK(!wait_fd(released, POLLIN, now() + .1));
    close(released);
    response =
        call(port, 1,
             "{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\","
             "\"operation_id\":\"queued-timeout\",\"timeout_ms\":20}");
    CHECK(strstr(response, "408") && strstr(response, "deadline_exceeded"));
    free(response);
    close(holds[0]);
    close(holds[1]);
    nanosleep(&delay, NULL);
    int slow = connect_to(port);
    send_all(slow, "POST /", 6);
    response = receive(slow);
    CHECK(strstr(response, "408") && strstr(response, "deadline_exceeded"));
    free(response);
    if (getenv("SAMOSA_SERVICE_FULL_UPLOAD")) {
        char *large = malloc(SOURCES_INPUT_MAX);
        CHECK(large);
        memset(large, 'a', SOURCES_INPUT_MAX);
        body = multipart(large, SOURCES_INPUT_MAX, &total);
        free(large);
        upload =
            request_head(port, 0, "/v1/uploads", total,
                         "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
        interim(upload);
        send_all(upload, body, total);
        free(body);
        response = receive(upload);
        CHECK(strstr(response, "200") && strstr(response, "67108864"));
        free(response);
    }
    upload = request_head(port, 0, "/v1/uploads", 4096,
                          "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    interim(upload);
    stop(pid);
    close(upload);
    port = start(argv[1], root, state, auth, &pid);
    upload = request_head(port, 0, "/v1/uploads", 4096,
                          "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    interim(upload);
    char lease_name[544], lease_held[544];
    snprintf(lease_name, sizeof(lease_name), "%s/service-runtime.lock", state);
    snprintf(lease_held, sizeof(lease_held), "%s/service-held.lock", state);
    CHECK(!rename(lease_name, lease_held));
    int replacement = open(lease_name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(replacement >= 0);
    CHECK(!close(replacement));
    exited(pid, 1);
    close(upload);
    CHECK(!unlink(lease_name));
    CHECK(!rename(lease_held, lease_name));
    await_admission(state);
    port = start(argv[1], root, state, auth, &pid);
    char saved_state[512];
    int saved_length =
        snprintf(saved_state, sizeof(saved_state), "%s-saved", state);
    CHECK(saved_length > 0 && (size_t)saved_length < sizeof(saved_state));
    CHECK(!rename(state, saved_state));
    CHECK(!mkdir(state, 0700));
    exited(pid, 1);
    CHECK(!rmdir(state));
    CHECK(!rename(saved_state, state));
    await_admission(state);
    port = start(argv[1], root, state, auth, &pid);
    upload = request_head(port, 0, "/v1/uploads", 4096,
                          "X-Sources-Format: text\r\nExpect: 100-continue\r\n");
    interim(upload);
    CHECK(!kill(pid, SIGKILL));
    int killed_status;
    CHECK(waitpid(pid, &killed_status, 0) == pid &&
          WIFSIGNALED(killed_status) && WTERMSIG(killed_status) == SIGKILL);
    await_admission(state);
    close(upload);
    port = start(argv[1], root, state, auth, &pid);
    response = call(port, 0, read);
    CHECK(strstr(response, "200") &&
          strstr(response, "owned native multipart data"));
    free(response);
    stop(pid);
    port = start(argv[1], root, state, auth, &pid);
    response = call(port, 0, read);
    CHECK(strstr(response, "200") &&
          strstr(response, "owned native multipart data"));
    free(response);
    for (int i = 0; i < 2; i++) {
        await_index(port, i, jobs[i]);
        free(jobs[i]);
    }
    stop(pid);
    free(source);
    free(version);
    printf(
        "PASS native socket Sources service: %u checks; auth/health, "
        "owner-bound multipart/readback, framing refusal, busy control "
        "bypass, owner FIFO/round-robin, active upload shutdown and cold "
        "restart, shared service/queue admission, two-owner headless indexing, "
        "lease/state replacement refusal and service SIGKILL recovery%s; %s\n",
        checks,
        getenv("SAMOSA_SERVICE_FULL_UPLOAD") ? ", actual64MiB upload" : "",
        root);
    return 0;
}
