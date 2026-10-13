/* Reuse the native socket client and owned process helpers, not a second
 * transport implementation. The provider below is explicitly a stalled fixture.
 */
#define _XOPEN_SOURCE 700
#define main service_socket_corpus
#include "test_service.c"
#undef main
#ifdef __linux__
#include <sys/prctl.h>
#endif
static unsigned control_start(const char *exe, const char *provider,
                              const char *root, const char *state,
                              const char *auth, const char *arguments,
                              pid_t *pid) {
    int pipefd[2];
    CHECK(!pipe(pipefd));
    *pid = fork();
    CHECK(*pid >= 0);
    if (!*pid) {
        close(pipefd[0]);
        CHECK(dup2(pipefd[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(pipefd[1]);
        if (!provider)
            execl(exe, exe, "--serve", "--serve-port", "0", "--serve-auth-file",
                  auth, "--root", root, "--state", state,
                  "--inference-provider-id", "fixture-whisper-v1",
                  "--inference-engine-sha256", tokens[0],
                  "--inference-model-sha256", tokens[1], (char *)NULL);
        execl(exe, exe, "--serve", "--serve-port", "0", "--serve-auth-file",
              auth, "--root", root, "--state", state, "--inference-executable",
              provider, "--inference-arguments-file", arguments,
              "--inference-provider-id", "fixture-whisper-v1",
              "--inference-engine-sha256", tokens[0],
              "--inference-model-sha256", tokens[1], (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char line[1024];
    size_t n = 0;
    double deadline = now() + 5;
    while (n < sizeof(line) - 1) {
        CHECK(wait_fd(pipefd[0], POLLIN, deadline));
        CHECK(read(pipefd[0], line + n, 1) == 1);
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
static pid_t ready_provider(const char *marker) {
    double deadline = now() + 5;
    while (now() < deadline) {
        FILE *f = fopen(marker, "r");
        long pid = 0;
        if (f) {
            int valid = fscanf(f, "%ld", &pid) == 1;
            CHECK(!fclose(f));
            if (valid && pid > 1) {
                CHECK(!kill((pid_t)pid, 0));
                return (pid_t)pid;
            }
        }
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    CHECK(0);
    return 0;
}
static void provider_gone(pid_t pid) {
    double deadline = now() + 5;
    while (now() < deadline) {
        int status;
        (void)waitpid(pid, &status, WNOHANG);
        if (kill(pid, 0) && errno == ESRCH)
            return;
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    CHECK(0);
}
static char *retained_job(unsigned port, int owner, const char *request) {
    double deadline = now() + 5;
    for (;;) {
        char *response = call(port, owner, request);
        if (!strstr(response, "\"code\":\"busy\""))
            return response;
        free(response);
        CHECK(now() < deadline);
        struct timespec delay = {0, 20000000};
        nanosleep(&delay, NULL);
    }
}
static void put32(unsigned char *p, unsigned value) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "stt-stall")) {
        unsigned char *request = malloc(2097153);
        if (!request)
            return 2;
        size_t n = fread(request, 1, 2097153, stdin);
        free(request);
        if (!n || n > 2097152 || ferror(stdin))
            return 2;
        FILE *f = fopen(argv[2], "w");
        if (!f)
            return 2;
        fprintf(f, "%ld\n", (long)getpid());
        if (fclose(f))
            return 2;
        signal(SIGTERM, SIG_IGN);
        for (;;)
            pause();
    }
    CHECK(argc == 2);
#ifdef __linux__
    /* The test owns the entire service tree, including its descendants after
     * deliberate service SIGKILL. Do not rely on the container's sleeping PID1.
     */
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1));
#endif
    alarm(180);
    signal(SIGPIPE, SIG_IGN);
    umask(077);
    char provider[4096];
    CHECK(realpath(argv[0], provider));
    char root[] = "/tmp/samosa-service-control-XXXXXX";
    CHECK(mkdtemp(root));
    char state[512], auth[512], arguments[512], marker[512];
    snprintf(state, sizeof(state), "%s/state", root);
    snprintf(auth, sizeof(auth), "%s/auth.json", root);
    snprintf(arguments, sizeof(arguments), "%s/arguments.json", root);
    snprintf(marker, sizeof(marker), "%s/provider-ready", root);
    FILE *f = fopen(auth, "w");
    CHECK(f);
    fprintf(f,
            "[{\"owner\":\"alice\",\"token\":\"%s\"},{\"owner\":\"bob\","
            "\"token\":\"%s\"}]",
            tokens[0], tokens[1]);
    CHECK(!fclose(f));
    f = fopen(arguments, "w");
    CHECK(f);
    fputs("[\"stt-stall\",", f);
    json_quote(f, marker, strlen(marker));
    fputc(']', f);
    CHECK(!fclose(f));
    pid_t service;
    unsigned port = control_start(argv[1], provider, root, state, auth,
                                  arguments, &service);
    unsigned char *wave = calloc(32044, 1);
    CHECK(wave);
    memcpy(wave, "RIFF", 4);
    put32(wave + 4, 32036);
    memcpy(wave + 8, "WAVEfmt ", 8);
    put32(wave + 16, 16);
    wave[20] = wave[22] = 1;
    put32(wave + 24, 16000);
    put32(wave + 28, 32000);
    wave[32] = 2;
    wave[34] = 16;
    memcpy(wave + 36, "data", 4);
    put32(wave + 40, 32000);
    size_t total;
    char *body = multipart(wave, 32044, &total);
    free(wave);
    int fd = request_head(port, 0, "/v1/uploads", total,
                          "X-Sources-Format: wav\r\n");
    send_all(fd, body, total);
    free(body);
    char *response = receive(fd);
    CHECK(strstr(response, "200"));
    char *source = reply_string(response, "source_id");
    free(response);
    for (int mode = 0; mode < 5; mode++) {
        (void)unlink(marker);
        char request[1024];
        int n = snprintf(
            request, sizeof(request),
            "{\"contract_version\":1,\"op\":\"transcribe_audio\",\"source_id\":"
            "\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":1000,"
            "\"operation_id\":\"active-%d\",\"timeout_ms\":%u}",
            source, source, mode, mode == 1 ? 3000u : 60000u);
        CHECK(n > 0 && (size_t)n < sizeof(request));
        fd = request_head(port, 0, "/v1/sources", (size_t)n, "");
        send_all(fd, request, (size_t)n);
        pid_t child = ready_provider(marker);
        if (mode == 0) {
            response = call(port, 1,
                            "{\"contract_version\":1,\"op\":\"operation_"
                            "cancel\",\"operation_id\":\"active-0\"}");
            CHECK(strstr(response, "unknown_operation"));
            free(response);
            CHECK(!kill(child, 0));
            response = call(port, 0,
                            "{\"contract_version\":1,\"op\":\"operation_"
                            "cancel\",\"operation_id\":\"active-0\"}");
            CHECK(strstr(response, "200"));
            free(response);
            response = receive(fd);
            CHECK(strstr(response, "400") && strstr(response, "cancelled"));
            free(response);
        } else if (mode == 1) {
            response = receive(fd);
            CHECK(strstr(response, "400") &&
                  strstr(response, "deadline_exceeded"));
            free(response);
        } else if (mode == 2)
            close(fd);
        else if (mode == 3) {
            stop(service);
            close(fd);
        } else {
            CHECK(!kill(service, SIGKILL));
            int status;
            CHECK(waitpid(service, &status, 0) == service &&
                  WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            close(fd);
        }
        provider_gone(child);
        if (mode >= 3) {
            await_admission(state);
            port = control_start(argv[1], provider, root, state, auth,
                                 arguments, &service);
        }
        char status_request[256];
        snprintf(status_request, sizeof(status_request),
                 "{\"contract_version\":1,\"op\":\"operation_get\",\"operation_"
                 "id\":\"active-%d\"}",
                 mode);
        response = call(port, 0, status_request);
        CHECK(strstr(response, "200") &&
              !strstr(response, "\"status\":\"running\"") &&
              !strstr(response, "\"status\":\"succeeded\""));
        free(response);
    }
    (void)unlink(marker);
    char background_request[512];
    snprintf(background_request, sizeof(background_request),
             "{\"contract_version\":1,\"op\":\"audio_index_create\",\"source_"
             "id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":1000}",
             source, source);
    response = retained_job(port, 0, background_request);
    CHECK(strstr(response, "200"));
    char *background_job = reply_string(response, "job_id");
    free(response);
    pid_t background_provider = ready_provider(marker);
    stop(service);
    provider_gone(background_provider);
    await_admission(state);
    port = control_start(argv[1], NULL, root, state, auth, arguments, &service);
    snprintf(
        background_request, sizeof(background_request),
        "{\"contract_version\":1,\"op\":\"audio_index_get\",\"job_id\":\"%s\"}",
        background_job);
    response = retained_job(port, 0, background_request);
    fprintf(stderr, "QA recovered background audio: %s\n", response);
    CHECK(strstr(response, "200") &&
          strstr(response, "\"captured_windows\":0") &&
          strstr(response, "\"status\":\"ready\""));
    free(response);
    response = retained_job(port, 1, background_request);
    CHECK(strstr(response, "unknown_audio_index"));
    free(response);
    stop(service);
    free(background_job);
    free(source);
#ifdef __linux__
    double drained = now() + 5;
    for (;;) {
        int status;
        pid_t child = waitpid(-1, &status, WNOHANG);
        if (child < 0 && errno == ECHILD)
            break;
        CHECK(child >= 0 || errno == EINTR);
        CHECK(now() < drained);
        if (!child) {
            struct timespec delay = {0, 10000000};
            nanosleep(&delay, NULL);
        }
    }
#endif
    printf("PASS native socket active-provider controls: %u checks; actual "
           "native WAV input, stalled fixture process, foreign/owned "
           "cancellation, deadline, disconnect, TERM/SIGKILL service "
           "interruption, provider cleanup, interrupted background audio and "
           "transport-free cold operation/job recovery; %s\n",
           checks, root);
    return 0;
}
