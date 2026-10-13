#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "json.h"
#include "native.h"
#include "index.h"
#include "ocr.h"
#include "ocr_pdf_fixture.h"
#include "sha256.h"
#include <fcntl.h>
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x)                                                               \
    do {                                                                       \
        checks++;                                                              \
        if (!(x)) {                                                            \
            fprintf(stderr, "FAIL native OCR %d: %s\n", __LINE__, #x);         \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
static int stopped(void *context) {
    (void)context;
    return 1;
}
typedef struct {
    int pid;
    unsigned started, ready;
    int cancel, expire, memory;
    double ready_at;
} HelperProbe;
static double monotonic(void);
static void helper_event(void *context, int pid, const char *event,
                         SourcesConfig *cfg) {
    HelperProbe *probe = context;
    CHECK(pid > 0);
    probe->pid = pid;
    if (!strcmp(event, "started"))
        probe->started++;
    else {
        CHECK(!strcmp(event, "fault_ready"));
        probe->ready++;
        probe->ready_at = monotonic();
        if (probe->expire)
            cfg->deadline_monotonic = probe->ready_at + 0.05;
        if (probe->memory)
            cfg->native_memory_limit = 64u * 1024u * 1024u;
    }
}
static int helper_cancelled(void *context) {
    HelperProbe *probe = context;
    return probe->cancel && probe->ready;
}
static void helper_reaped(const HelperProbe *probe) {
    CHECK(probe->pid > 0 && probe->started == 1 && probe->ready == 1);
    int status = 0;
    errno = 0;
    CHECK(waitpid((pid_t)probe->pid, &status, WNOHANG) == -1 &&
          errno == ECHILD);
    CHECK(kill((pid_t)probe->pid, 0) == -1 && errno == ESRCH);
}
static void helper_tests(SourcesConfig cfg, const void *bytes, size_t n) {
    const char *faults[] = {"abort",       "early_exit", "bad_protocol",
                            "huge_output", "bad_result", "stall",
                            "memory"};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(*faults); i++) {
        HelperProbe probe = {.expire = i == 5, .memory = i == 6};
        SourcesConfig work = cfg;
        work.native_test_fault = faults[i];
        work.native_test_event = helper_event;
        work.native_test_context = &probe;
        work.deadline_monotonic = monotonic() + 20;
        SourcesOcrResult result = {0};
        const char *error = sources_ocr_extract(&work, bytes, n, &result);
        const char *expected = i < 2    ? "native_helper_failed"
                               : i < 5  ? "native_helper_protocol"
                               : i == 5 ? "deadline_exceeded"
                                        : "native_helper_memory_limit";
        if (!error || strcmp(error, expected))
            fprintf(stderr, "helper fault %s: %s\n", faults[i],
                    error ? error : "success");
        CHECK(error && !strcmp(error, expected) && !result.text);
        double elapsed = monotonic() - probe.ready_at;
        if (!probe.ready || elapsed >= 1)
            fprintf(stderr, "helper fault %s: ready=%u elapsed=%.6fs\n",
                    faults[i], probe.ready, elapsed);
        CHECK(probe.ready && elapsed < 1);
        helper_reaped(&probe);
    }
    HelperProbe cancelled = {.cancel = 1};
    cfg.native_test_fault = "stall";
    cfg.native_test_event = helper_event;
    cfg.native_test_context = &cancelled;
    cfg.cancelled = helper_cancelled;
    cfg.cancellation = &cancelled;
    SourcesOcrResult result = {0};
    const char *error = sources_ocr_extract(&cfg, bytes, n, &result);
    CHECK(error && !strcmp(error, "cancelled") && !result.text);
    helper_reaped(&cancelled);
    SourcesConfig invalid = cfg;
    invalid.cancelled = NULL;
    invalid.cancellation = NULL;
    invalid.native_test_fault = NULL;
    invalid.native_memory_limit = 1;
    error = sources_ocr_extract(&invalid, bytes, n, &result);
    CHECK(error && !strcmp(error, "invalid_configuration") && !result.text);
    invalid.native_memory_limit = 512u * 1024u * 1024u + 1u;
    error = sources_ocr_extract(&invalid, bytes, n, &result);
    CHECK(error && !strcmp(error, "invalid_configuration") && !result.text);
    char path[] = "/tmp/samosa-native-descriptor.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(dup2(fd, 100) == 100);
    CHECK(!fcntl(100, F_SETFD, 0));
    CHECK(!close(fd));
    HelperProbe limits = {0};
    SourcesConfig budget = cfg;
    budget.cancelled = NULL;
    budget.cancellation = NULL;
    budget.native_test_fault = "limits";
    budget.native_test_event = helper_event;
    budget.native_test_context = &limits;
    error = sources_ocr_extract(&budget, bytes, n, &result);
    CHECK(!error && result.text && !strcmp(result.text, "CLOSED:64:32:0"));
    sources_ocr_free(&result);
    helper_reaped(&limits);
    CHECK(fcntl(100, F_GETFD) >= 0);
    CHECK(!close(100));
    CHECK(!unlink(path));
}
typedef struct {
    int fd, descendant;
} DeathProbe;
static void death_event(void *context, int pid, const char *event,
                        SourcesConfig *cfg) {
    (void)cfg;
    DeathProbe *probe = context;
    if (!strcmp(event, "descendant"))
        probe->descendant = pid;
    if (!strcmp(event, "fault_ready")) {
        int ids[2] = {pid, probe->descendant};
        CHECK(write(probe->fd, ids, sizeof(ids)) == (ssize_t)sizeof(ids));
    }
}
static void parent_death_test(SourcesConfig cfg, const void *bytes, size_t n) {
#ifdef __linux__
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1));
#endif
    int ready[2];
    CHECK(!pipe(ready));
    pid_t caller = fork();
    CHECK(caller >= 0);
    if (!caller) {
        close(ready[0]);
        DeathProbe probe = {.fd = ready[1]};
        cfg.native_test_fault = "descendant_stall";
        cfg.native_test_event = death_event;
        cfg.native_test_context = &probe;
        SourcesOcrResult result = {0};
        sources_ocr_extract(&cfg, bytes, n, &result);
        _exit(3);
    }
    close(ready[1]);
    struct pollfd descriptor = {.fd = ready[0], .events = POLLIN};
    int readable = poll(&descriptor, 1, 5000);
    int ids[2] = {0};
    ssize_t got = readable > 0 ? read(ready[0], ids, sizeof(ids)) : 0;
    close(ready[0]);
    CHECK(!kill(caller, SIGKILL));
    int status = 0;
    CHECK(waitpid(caller, &status, 0) == caller);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(got == (ssize_t)sizeof(ids) && ids[0] > 0 && ids[1] > 0);
    for (unsigned i = 0; i < 2; i++) {
        int worker = ids[i];
        double limit = monotonic() + 2;
        int gone = 0;
        while (monotonic() < limit) {
#ifdef __linux__
            pid_t reaped = waitpid((pid_t)worker, &status, WNOHANG);
            if (reaped == (pid_t)worker) {
                CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            }
#endif
            if (kill((pid_t)worker, 0) < 0 && errno == ESRCH) {
                gone = 1;
                break;
            }
            struct timespec pause = {0, 10000000};
            nanosleep(&pause, NULL);
        }
        if (!gone) {
            kill(-(pid_t)worker, SIGKILL);
            kill((pid_t)worker, SIGKILL);
        }
        CHECK(gone);
    }
}
typedef struct {
    unsigned polls;
    int expire;
    double deadline;
    const char *mutate;
} MonitorProbe;
static double monotonic(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static void monitor_event(void *context, const char *event,
                          SourcesConfig *cfg) {
    MonitorProbe *probe = context;
    CHECK(!strcmp(event, "monitor_poll"));
    probe->polls++;
    if (probe->mutate && probe->polls == 1) {
        int fd = open(probe->mutate, O_WRONLY);
        CHECK(fd >= 0);
        unsigned char replacement = 0;
        CHECK(pwrite(fd, &replacement, 1, 0) == 1);
        CHECK(!close(fd));
    }
    if (probe->expire && probe->polls == 1) {
        cfg->deadline_monotonic = monotonic() + 0.05;
        double remaining = 0.07;
        if (remaining > 0) {
            struct timespec delay = {
                (time_t)remaining,
                (long)((remaining - (time_t)remaining) * 1e9)};
            while (nanosleep(&delay, &delay) && errno == EINTR) {
            }
        }
    }
}
static int monitor_cancelled(void *context) {
    MonitorProbe *probe = context;
    return !probe->expire && !probe->mutate && probe->polls > 0;
}
static unsigned cache_count(const SourcesConfig *cfg) {
    char path[600];
    snprintf(path, sizeof(path), "%s/sources.sqlite3", cfg->state);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM evidence_cache", -1,
                             &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    unsigned n = (unsigned)sqlite3_column_int(stmt, 0);
    CHECK(!sqlite3_finalize(stmt));
    CHECK(!sqlite3_close(db));
    return n;
}
static void monitor_tests(SourcesConfig cfg, const void *bytes, size_t n) {
    SourcesOcrConfig engine = *cfg.ocr;
    cfg.ocr = &engine;
    MonitorProbe probe = {0};
    engine.test_event = monitor_event;
    engine.test_context = &probe;
    cfg.cancelled = monitor_cancelled;
    cfg.cancellation = &probe;
    SourcesOcrResult result = {0};
    const char *error = sources_ocr_extract(&cfg, bytes, n, &result);
    CHECK(error && !strcmp(error, "cancelled") && probe.polls && !result.text);
    probe = (MonitorProbe){.expire = 1, .deadline = monotonic() + 20};
    cfg.deadline_monotonic = probe.deadline;
    error = sources_ocr_extract(&cfg, bytes, n, &result);
    CHECK(error && !strcmp(error, "deadline_exceeded") && probe.polls &&
          !result.text);
}
static char *call(const SourcesConfig *cfg, const char *request, int expected) {
    char *out = NULL;
    size_t n = 0;
    FILE *stream = open_memstream(&out, &n);
    CHECK(stream);
    int rc = sources_dispatch(cfg, request, strlen(request), stream);
    CHECK(!fclose(stream));
    if (rc != expected)
        fprintf(stderr, "public OCR response: %s\n", out);
    CHECK(rc == expected);
    Json j;
    CHECK(json_parse(&j, out, n));
    return out;
}
typedef struct {
    HelperProbe helper;
    const SourcesConfig *cfg;
    const char *good;
    char *expected;
    const void *bytes;
    size_t length;
} DurableHelperProbe;
static void durable_helper_event(void *context, int pid, const char *event,
                                 SourcesConfig *work) {
    DurableHelperProbe *probe = context;
    helper_event(&probe->helper, pid, event, work);
    if (strcmp(event, "fault_ready"))
        return;
    const char *cancel = "{\"contract_version\":1,\"op\":\"operation_cancel\","
                         "\"operation_id\":\"native-owned\"}";
    SourcesConfig foreign = *probe->cfg;
    foreign.owner = "bob";
    char *out = call(&foreign, cancel, 1);
    CHECK(strstr(out, "unknown_operation"));
    free(out);
    out = call(probe->cfg,
               "{\"contract_version\":1,\"op\":\"operation_get\","
               "\"operation_id\":\"native-owned\"}", 0);
    CHECK(strstr(out, "\"status\":\"running\"") &&
          strstr(out, "\"cancel_requested\":false"));
    free(out);
    out = call(probe->cfg, probe->good, 0);
    CHECK(!strcmp(out, probe->expected));
    free(out);
    CHECK(!kill((pid_t)pid, 0));
    SourcesOcrResult native_peer = {0};
    CHECK(!sources_ocr_extract(probe->cfg, probe->bytes, probe->length,
                               &native_peer));
    CHECK(native_peer.text && strstr(native_peer.text, "TOKEN 7319"));
    sources_ocr_free(&native_peer);
    CHECK(!kill((pid_t)pid, 0));
    /* A distinct durable peer commits while the owned native worker stalls. */
    out = call(probe->cfg,
               "{\"contract_version\":1,\"op\":\"inventory\",\"path\":\"\","
               "\"operation_id\":\"native-peer\"}", 0);
    CHECK(strstr(out, "entries"));
    free(out);
    out = call(probe->cfg, cancel, 0);
    CHECK(strstr(out, "\"status\":\"running\"") &&
          strstr(out, "\"cancel_requested\":true"));
    free(out);
}
static void durable_helper_test(const SourcesConfig *cfg, const char *fresh,
                                const char *good, const void *bytes, size_t length) {
    size_t n = strlen(fresh);
    CHECK(n && fresh[n - 1] == '}');
    char request[2048];
    CHECK(snprintf(request, sizeof(request),
                   "%.*s,\"operation_id\":\"native-owned\","
                   "\"parent_operation_id\":\"native-parent\","
                   "\"timeout_ms\":20000}", (int)n - 1, fresh) <
          (int)sizeof(request));
    DurableHelperProbe probe = {.cfg = cfg, .good = good,
                               .expected = call(cfg, good, 0),
                               .bytes = bytes, .length = length};
    unsigned count = cache_count(cfg);
    SourcesConfig work = *cfg;
    work.native_test_fault = "stall";
    work.native_test_event = durable_helper_event;
    work.native_test_context = &probe;
    char *out = call(&work, request, 1);
    CHECK(strstr(out, "cancelled") && strstr(out, "native-parent") &&
          !strstr(out, "evidence_id"));
    helper_reaped(&probe.helper);
    CHECK(cache_count(cfg) == count);
    SourcesConfig offline = *cfg;
    offline.root = NULL;
    offline.native_helper = NULL;
    char *retry = call(&offline, request, 1);
    CHECK(!strcmp(out, retry));
    free(out);
    free(retry);
    out = call(cfg,
               "{\"contract_version\":1,\"op\":\"operation_get\","
               "\"operation_id\":\"native-peer\"}", 0);
    CHECK(strstr(out, "\"status\":\"completed\"") &&
          strstr(out, "\"cancel_requested\":false"));
    free(out);
    free(probe.expected);
}
static void public_helper_failures(const SourcesConfig *cfg, const char *fresh,
                                   const char *good_request, const void *bytes,
                                   size_t length) {
    unsigned count = cache_count(cfg);
    char *good = call(cfg, good_request, 0);
    const char *faults[] = {"abort",       "early_exit", "bad_protocol",
                            "huge_output", "bad_result", "stall",
                            "memory"};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(*faults); i++) {
        HelperProbe probe = {.expire = i == 5, .memory = i == 6};
        SourcesConfig work = *cfg;
        work.native_test_fault = faults[i];
        work.native_test_event = helper_event;
        work.native_test_context = &probe;
        work.deadline_monotonic = monotonic() + 20;
        char *out = call(&work, fresh, 1);
        const char *expected = i < 2    ? "native_helper_failed"
                               : i < 5  ? "native_helper_protocol"
                               : i == 5 ? "deadline_exceeded"
                                        : "native_helper_memory_limit";
        CHECK(strstr(out, expected) && !strstr(out, "evidence_id"));
        free(out);
        helper_reaped(&probe);
        CHECK(cache_count(cfg) == count);
        out = call(cfg, good_request, 0);
        CHECK(!strcmp(out, good));
        free(out);
    }
    SourcesConfig absent = *cfg;
    absent.native_helper = NULL;
    char *out = call(&absent, fresh, 1);
    CHECK(strstr(out, "native_helper_unavailable"));
    free(out);
    CHECK(cache_count(cfg) == count);
    durable_helper_test(cfg, fresh, good_request, bytes, length);
    out = call(&absent, good_request, 0);
    CHECK(!strcmp(out, good));
    free(out);
    free(good);
}
static void public_image(SourcesConfig cfg, const unsigned char *bytes,
                         size_t n) {
    char root[] = "/tmp/samosa-ocr-public.XXXXXX";
    CHECK(mkdtemp(root));
    char state[512], path[512];
    snprintf(state, sizeof(state), "%s/state", root);
    snprintf(path, sizeof(path), "%s/image.ppm", root);
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(bytes, 1, n, f) == n);
    CHECK(!fclose(f));
    cfg.root = root;
    cfg.state = state;
    cfg.owner = "alice";
    char *out = call(&cfg,
                     "{\"contract_version\":1,\"op\":\"register_image\","
                     "\"path\":\"image.ppm\"}",
                     0);
    Json j;
    CHECK(json_parse(&j, out, strlen(out)));
    char *id = json_string(&j, json_get(&j, 0, "source_id"));
    char digest[65];
    hash_bytes(bytes, n, digest);
    CHECK(id && !strcmp(id, digest));
    free(out);
    char req[1024];
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_image\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":10}",
             id, id);
    out = call(&cfg, req, 0);
    CHECK(json_parse(&j, out, strlen(out)));
    char *text = json_string(&j, json_get(&j, 0, "text"));
    CHECK(text && !strcmp(text, "TOKEN 7319"));
    free(text);
    CHECK(strstr(out, "ocr_recognition_uncertain") &&
          strstr(out, "original_pixels") && strstr(out, "extracted_utf8"));
    char *again = call(&cfg, req, 0);
    CHECK(!strcmp(out, again));
    free(out);
    free(again);
    unsigned count = cache_count(&cfg);
    SourcesOcrConfig alternate = *cfg.ocr;
    alternate.language = "illegal/path";
    SourcesConfig invalid = cfg;
    invalid.ocr = &alternate;
    out = call(&invalid, req, 1);
    CHECK(strstr(out, "invalid_configuration"));
    free(out);
    CHECK(cache_count(&cfg) == count);
    char linkpath[512];
    snprintf(linkpath, sizeof(linkpath), "%s/model-link", root);
    CHECK(!symlink(cfg.ocr->traineddata_file, linkpath));
    alternate = *cfg.ocr;
    alternate.traineddata_file = linkpath;
    out = call(&invalid, req, 1);
    CHECK(strstr(out, "ocr_artifact_invalid"));
    free(out);
    CHECK(cache_count(&cfg) == count);
    CHECK(!unlink(linkpath));
    CHECK(!mkfifo(linkpath, 0600));
    out = call(&invalid, req, 1);
    CHECK(strstr(out, "ocr_artifact_invalid"));
    free(out);
    CHECK(!unlink(linkpath));
    out = call(&cfg, req, 0);
    CHECK(strstr(out, "7319"));
    free(out);
    char active_req[1024];
    snprintf(active_req, sizeof(active_req),
             "{\"contract_version\":1,\"op\":\"read_image\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_byte\":2,\"end_byte\":5}",
             id, id);
    public_helper_failures(&cfg, active_req, req, bytes, n);
    SourcesOcrConfig monitored = *cfg.ocr;
    MonitorProbe probe = {0};
    monitored.test_event = monitor_event;
    monitored.test_context = &probe;
    SourcesConfig active = cfg;
    active.ocr = &monitored;
    active.cancelled = monitor_cancelled;
    active.cancellation = &probe;
    out = call(&active, active_req, 1);
    CHECK(strstr(out, "cancelled") && probe.polls);
    free(out);
    CHECK(cache_count(&cfg) == count);
    probe = (MonitorProbe){.expire = 1, .deadline = monotonic() + 20};
    active.deadline_monotonic = probe.deadline;
    out = call(&active, active_req, 1);
    CHECK(strstr(out, "deadline_exceeded") && probe.polls);
    free(out);
    CHECK(cache_count(&cfg) == count);
    /* Change a private model copy only after the real recognition monitor runs.
     * The already initialized engine uses its pinned model snapshot. */
    FILE *original = fopen(cfg.ocr->traineddata_file, "rb"),
         *copy = fopen(linkpath, "wb");
    CHECK(original && copy);
    unsigned char chunk[65536], first = 0;
    size_t got;
    int first_chunk = 1;
    while ((got = fread(chunk, 1, sizeof(chunk), original))) {
        if (first_chunk) {
            first = chunk[0];
            first_chunk = 0;
        }
        CHECK(fwrite(chunk, 1, got, copy) == got);
    }
    CHECK(!ferror(original));
    CHECK(!fclose(original));
    CHECK(!fclose(copy));
    monitored.traineddata_file = linkpath;
    probe = (MonitorProbe){.mutate = linkpath};
    active.deadline_monotonic = 0;
    out = call(&active, active_req, 1);
    CHECK(strstr(out, "ocr_artifact_changed") && probe.polls);
    free(out);
    CHECK(cache_count(&cfg) == count);
    int model_fd = open(linkpath, O_WRONLY);
    CHECK(model_fd >= 0);
    CHECK(pwrite(model_fd, &first, 1, 0) == 1);
    CHECK(!close(model_fd));
    CHECK(!unlink(linkpath));
    out = call(&cfg, req, 0);
    CHECK(strstr(out, "7319"));
    free(out);
    SourcesConfig foreign = cfg;
    foreign.owner = "bob";
    out = call(&foreign, req, 1);
    CHECK(strstr(out, "unknown_source"));
    free(out);
    SourcesConfig absent = cfg;
    absent.ocr = NULL;
    out = call(&absent, req, 1);
    CHECK(strstr(out, "ocr_unavailable"));
    free(out);
    CHECK(!unlink(path));
    out = call(&cfg, req, 0);
    CHECK(strstr(out, "7319"));
    free(out);
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_image\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_byte\":6,\"end_byte\":10,"
             "\"operation_id\":\"image-read\",\"parent_operation_id\":"
             "\"parent\",\"timeout_ms\":10000}",
             id, id);
    out = call(&cfg, req, 0);
    CHECK(strstr(out, "7319") && strstr(out, "image-read"));
    cfg.root = NULL;
    again = call(&cfg, req, 0);
    CHECK(!strcmp(out, again));
    free(out);
    free(again);
    cfg.root = root;
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_image\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":999}",
             id, id);
    out = call(&cfg, req, 1);
    CHECK(strstr(out, "invalid_range"));
    free(out);
    free(id);
}
typedef struct {unsigned started;int mode,fired;const char *path;} PdfIndexProbe;
static int pdf_index_cancelled(void *context){PdfIndexProbe *probe=context;return probe->mode==1&&probe->fired;}
static void pdf_index_event(void *context,int pid,const char *event,SourcesConfig *work){
    PdfIndexProbe *probe=context;CHECK(pid>0);if(strcmp(event,"started"))return;
    if(++probe->started!=3)return;
    probe->fired=1;
    if(probe->mode==2)work->deadline_monotonic=monotonic()+0.001;
    if(probe->mode==3){FILE *f=fopen(probe->path,"ab");CHECK(f&&fputs("\n% changed during OCR\n",f)>=0&&!fclose(f));}
}
static void pdf_observe(const SourcesConfig *cfg,const char *version){
    char request[512];
    /* Each changed hash needs a distinct durable observation operation. */
    static unsigned sequence;snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"locator_reconcile\",\"operation_id\":\"pdf-observe-%u\",\"locators\":[{\"path\":\"mixed.pdf\",\"expected_version\":\"%s\"}]}",++sequence,version);
    char *response=call(cfg,request,0);CHECK(strstr(response,"observed_content_hash"));free(response);
}
static char *pdf_artifact_read(const SourcesConfig *cfg,const char *published,const char *operation,const char *version){
    Json j;CHECK(json_parse(&j,published,strlen(published)));int publication=json_get(&j,0,"publication");char *source=json_string(&j,json_get(&j,publication,"external_source_id")),*artifact=json_string(&j,json_get(&j,publication,"external_artifact_id"));CHECK(source&&artifact);
    char *request=NULL;size_t n=0;FILE *f=open_memstream(&request,&n);CHECK(f);fprintf(f,"{\"contract_version\":1,\"op\":\"memory_read\",\"operation_id\":\"%s\",\"timeout_ms\":10000,\"store_id\":\"%s\",\"external_source_id\":\"%s\",\"external_artifact_id\":\"%s\"}",operation,cfg->chutni_store_id,source,artifact);CHECK(!fclose(f));free(source);free(artifact);
    char *response=call(cfg,request,0);CHECK(strstr(response,"EXACT TEXT LAYER 2468")&&strstr(response,"TOKEN 7319")&&strstr(response,"pdfium-index-fallback-v1")&&strstr(response,"ocr_captures")&&strstr(response,"rendered_pixels")&&strstr(response,version)&&strstr(response,"\"type\":\"pages\""));free(response);return request;
}
static void pdf_retry_wait(const char *status){
    Json j;size_t due=0;CHECK(json_parse(&j,status,strlen(status)));int rows=json_get(&j,0,"publications");CHECK(rows>=0&&json_uint(&j,json_get(&j,rows+1,"retry_at_unix_ms"),&due));struct timespec now;CHECK(!clock_gettime(CLOCK_REALTIME,&now));size_t current=(size_t)now.tv_sec*1000+(size_t)now.tv_nsec/1000000;CHECK(due<=current+10000);size_t delay=due>current?due-current+20:0;struct timespec sleep={(time_t)(delay/1000),(long)(delay%1000)*1000000};while(nanosleep(&sleep,&sleep))CHECK(errno==EINTR);
}
static void pdf_rehashed_corruption(const SourcesConfig *cfg,const char *read_request,const char *path,const char *version){
    char *response=call(cfg,read_request,0);Json j;CHECK(json_parse(&j,response,strlen(response)));int result=json_get(&j,json_get(&j,0,"memory"),"result"),provenance=json_get(&j,result,"provenance"),parameters=json_get(&j,json_get(&j,provenance,"derivation"),"parameters"),evidence=json_get(&j,parameters,"sources_evidence");CHECK(evidence>=0);char *capture=strndup(response+j.tokens[evidence].begin,j.tokens[evidence].end-j.tokens[evidence].begin);CHECK(capture);free(response);
    unsigned replaced=0;for(char *p=capture;(p=strstr(p,"TOKEN 7319"));p+=10){memcpy(p,"FORGED0000",10);replaced++;}CHECK(replaced>=2);CHECK(json_parse(&j,capture,strlen(capture)));char *id=json_string(&j,json_get(&j,0,"evidence_id")),*text=source_index_literal(capture,strlen(capture));CHECK(id&&text);char digest[65],key[65];hash_bytes(capture,strlen(capture),digest);memset(key,'f',64);key[64]=0;
    char *metadata=NULL;size_t length=0;FILE *f=open_memstream(&metadata,&length);CHECK(f);const char *names[]={"library","store","store_id","root","path","publication_id","version","evidence_id","evidence_digest","text"},*values[]={cfg->chutni_library,cfg->chutni_store,cfg->chutni_store_id,cfg->root,path,key,version,id,digest,text};fputc('{',f);for(size_t i=0;i<10;i++){if(i)fputc(',',f);json_quote(f,names[i],strlen(names[i]));fputc(':',f);json_quote(f,values[i],strlen(values[i]));}fputs(",\"evidence\":",f);fputs(capture,f);fputc('}',f);CHECK(!fclose(f));
    f=fopen(path,"rb");CHECK(f&&!fseek(f,0,SEEK_END));long bytes=ftell(f);CHECK(bytes>0&&!fseek(f,0,SEEK_SET));unsigned char *snapshot=malloc((size_t)bytes);CHECK(snapshot&&fread(snapshot,1,(size_t)bytes,f)==(size_t)bytes&&!fclose(f));char *body=NULL;size_t size=0;const char *error=sources_native_chutni_publish(cfg,snapshot,(size_t)bytes,metadata,&body,&size);CHECK(error&&!strcmp(error,"corrupt_index")&&!body&&!size);free(snapshot);free(metadata);free(capture);free(id);free(text);
}
static void indexed_pdf(SourcesConfig cfg,const char *library,const unsigned char *bytes,size_t n,const char *sdk_library,const char *sdk_worker){
    char base[]="/tmp/samosa-index-pdf-ocr.XXXXXX",root[512],state[512],path[600],lib[4096];CHECK(mkdtemp(base)&&realpath(library,lib));snprintf(root,sizeof(root),"%s/files",base);CHECK(!mkdir(root,0700));snprintf(state,sizeof(state),"%s/state",base);snprintf(path,sizeof(path),"%s/mixed.pdf",root);CHECK(ocr_pdf_fixture_mixed(path,bytes,n,0,1));cfg.root=root;cfg.state=state;cfg.owner="pdf-index-owner";cfg.pdfium_library=lib;
    char memory_path[512],*store_id=NULL,*old_read=NULL,*new_read=NULL,*original=NULL;void *sdk=NULL;
    if(sdk_library){sdk=dlopen(sdk_library,RTLD_NOW|RTLD_LOCAL);CHECK(sdk);int (*create)(const char *,const char *,void **)=NULL;int (*sdk_call)(void *,const char *,const char *,char **)=NULL;void (*sdk_free)(void *)=NULL;void (*sdk_close)(void *)=NULL;
        void *symbol=dlsym(sdk,"chutni_create");CHECK(symbol);memcpy(&create,&symbol,sizeof(create));symbol=dlsym(sdk,"chutni_call");CHECK(symbol);memcpy(&sdk_call,&symbol,sizeof(sdk_call));symbol=dlsym(sdk,"chutni_free");CHECK(symbol);memcpy(&sdk_free,&symbol,sizeof(sdk_free));symbol=dlsym(sdk,"chutni_close");CHECK(symbol);memcpy(&sdk_close,&symbol,sizeof(sdk_close));snprintf(memory_path,sizeof(memory_path),"%s/memory.chutni",base);void *store=NULL;CHECK(!create(memory_path,"PDF OCR indexing fixture",&store));char *info=NULL;CHECK(!sdk_call(store,"store_info","{}",&info));Json j;CHECK(json_parse(&j,info,strlen(info)));store_id=json_string(&j,json_get(&j,0,"store_id"));CHECK(store_id);sdk_free(info);sdk_close(store);cfg.chutni_library=sdk_library;cfg.chutni_store=memory_path;cfg.chutni_store_id=store_id;cfg.chutni_helper=sdk_worker;cfg.chutni_publish=1;
    }
    char *response=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"mixed-create\"}",0);Json j;CHECK(json_parse(&j,response,strlen(response)));char *job=json_string(&j,json_get(&j,0,"job_id"));CHECK(job);free(response);char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"mixed-step\",\"max_items\":4}",job);response=call(&cfg,request,0);CHECK(strstr(response,"\"indexed_items\":1")&&strstr(response,"\"failed_items\":0"));free(response);
    const char *queries[]={"EXACT TEXT LAYER 2468","TOKEN 7319"};for(size_t i=0;i<2;i++){snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"%s\"}",job,queries[i]);response=call(&cfg,request,0);CHECK(strstr(response,"\"returned_candidates\":1")&&strstr(response,"pdfium-index-fallback-v1")&&strstr(response,"text_layer_with_ocr_fallback")&&strstr(response,"\"ocr_pages\":1")&&strstr(response,"rendered_pixels")&&strstr(response,"\"start_page\":1")&&strstr(response,"\"end_page\":2"));free(response);}
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"TOKEN 7319\"}",job);response=call(&cfg,request,0);CHECK(json_parse(&j,response,strlen(response)));int candidates=json_get(&j,0,"candidates");original=json_string(&j,json_get(&j,candidates+1,"version"));CHECK(original);free(response);
    if(sdk_library){
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_publish_step\",\"job_id\":\"%s\"}",job);
        SourcesConfig absent=cfg;absent.ocr=NULL;response=call(&absent,request,1);CHECK(strstr(response,"ocr_unavailable")&&!strstr(response,"corrupt_index"));free(response);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_publication_status\",\"job_id\":\"%s\"}",job);response=call(&cfg,request,0);CHECK(strstr(response,"ocr_unavailable")&&strstr(response,"\"state\":\"pending\""));pdf_retry_wait(response);free(response);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_publish_step\",\"job_id\":\"%s\"}",job);response=call(&cfg,request,0);CHECK(strstr(response,"\"published_items\":1"));old_read=pdf_artifact_read(&cfg,response,"pdf-old-read",original);free(response);pdf_rehashed_corruption(&cfg,old_read,path,original);
    }
    /* Cold changed versions exercise live mutation, cancellation and deadline
     * during the third native extraction (the selected OCR page). */
    for(int mode=4;mode>=1;mode--){
        FILE *f=fopen(path,"ab");CHECK(f&&fprintf(f,"\n%% new PDF version %d\n",mode)>0&&!fclose(f));pdf_observe(&cfg,original);
        PdfIndexProbe probe={.mode=mode,.path=path};SourcesConfig work=cfg;work.native_test_event=pdf_index_event;work.native_test_context=&probe;work.cancelled=pdf_index_cancelled;work.cancellation=&probe;SourcesOcrConfig missing=*cfg.ocr;if(mode==4){missing.traineddata_file="/missing/samosa-test-ocr-model";work.ocr=&missing;}
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"pdf-controlled-%d\",\"max_items\":1}",job,mode);response=call(&work,request,1);if(mode!=4)CHECK(probe.fired&&probe.started==3);CHECK(strstr(response,mode==4?"ocr_artifact_invalid":mode==1?"cancelled":mode==2?"deadline_exceeded":"stale_source")|| (mode==3&&strstr(response,"source_changed")));free(response);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_pending\",\"job_id\":\"%s\"}",job);response=call(&cfg,request,0);CHECK(strstr(response,"\"pending_items\":1")&&strstr(response,original));free(response);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"TOKEN 7319\"}",job);response=call(&cfg,request,0);CHECK(strstr(response,"\"returned_candidates\":1")&&strstr(response,original));free(response);
    }
    FILE *f=fopen(path,"ab");CHECK(f&&fputs("\n% final healthy PDF version\n",f)>=0&&!fclose(f));pdf_observe(&cfg,original);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":\"%s\",\"operation_id\":\"pdf-healthy-reindex\",\"max_items\":1}",job);response=call(&cfg,request,0);CHECK(strstr(response,"\"reindexed_items\":1"));free(response);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"TOKEN 7319\"}",job);response=call(&cfg,request,0);CHECK(strstr(response,"\"returned_candidates\":2")&&strstr(response,original));CHECK(json_parse(&j,response,strlen(response)));candidates=json_get(&j,0,"candidates");char *current=json_string(&j,json_get(&j,j.tokens[candidates+1].next,"version"));CHECK(current&&strcmp(current,original));free(response);
    if(sdk_library){snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_publish_step\",\"job_id\":\"%s\"}",job);response=call(&cfg,request,0);CHECK(strstr(response,"\"published_items\":1"));new_read=pdf_artifact_read(&cfg,response,"pdf-new-read",current);free(response);}
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_search\",\"job_id\":\"%s\",\"query\":\"TOKEN 7319\"}",job);
    CHECK(!unlink(path));SourcesConfig offline=cfg;offline.ocr=NULL;response=call(&offline,request,0);CHECK(strstr(response,"TOKEN 7319")&&strstr(response,"\"returned_candidates\":2"));free(response);offline.owner="foreign";response=call(&offline,request,1);CHECK(strstr(response,"unknown_index"));free(response);
    if(sdk_library){offline=cfg;offline.root=NULL;offline.ocr=NULL;offline.chutni_helper=NULL;response=call(&offline,old_read,0);CHECK(strstr(response,original)&&strstr(response,"TOKEN 7319"));free(response);response=call(&offline,new_read,0);CHECK(strstr(response,current)&&strstr(response,"TOKEN 7319"));free(response);CHECK(!dlclose(sdk));}
    free(store_id);free(old_read);free(new_read);free(original);free(current);free(job);
}
static void public_pdf(SourcesConfig cfg, const char *library,
                       const unsigned char *bytes, size_t n) {
    char root[] = "/tmp/samosa-ocr-pdf.XXXXXX";
    CHECK(mkdtemp(root));
    char state[512], path[512], lib[4096];
    CHECK(realpath(library, lib));
    snprintf(state, sizeof(state), "%s/state", root);
    snprintf(path, sizeof(path), "%s/image.pdf", root);
    CHECK(ocr_pdf_fixture(path, bytes, n, 0));
    cfg.root = root;
    cfg.state = state;
    cfg.owner = "alice";
    cfg.pdfium_library = lib;
    char *out = call(&cfg,
                     "{\"contract_version\":1,\"op\":\"register_pdf\",\"path\":"
                     "\"image.pdf\"}",
                     0);
    Json j;
    CHECK(json_parse(&j, out, strlen(out)));
    char *id = json_string(&j, json_get(&j, 0, "source_id"));
    CHECK(id);
    free(out);
    char req[1024];
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_pdf\",\"source_id\":\"%s\","
             "\"version\":\"%s\",\"start_page\":2,\"end_page\":2}",
             id, id);
    out = call(&cfg, req, 0);
    CHECK(json_parse(&j, out, strlen(out)));
    int page_array = json_get(&j, 0, "pages");
    CHECK(page_array >= 0 && j.tokens[page_array].type == JT_ARRAY &&
          j.tokens[page_array].count == 1);
    char *layer_text = json_string(&j, json_get(&j, page_array + 1, "text"));
    if (!layer_text || *layer_text)
        fprintf(stderr, "Image-only PDF text layer: %s\n", out);
    CHECK(layer_text && !*layer_text && strstr(out, "empty_text_layer"));
    free(layer_text);
    free(out);
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_pdf_ocr\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_page\":2,\"end_page\":2}",
             id, id);
    out = call(&cfg, req, 0);
    CHECK(strstr(out, "TOKEN 7319") && strstr(out, "\"page\":2") &&
          strstr(out, "ocr_recognition_uncertain") &&
          strstr(out, "rendered_pixels"));
    char *again = call(&cfg, req, 0);
    CHECK(!strcmp(out, again));
    free(out);
    free(again);
    unsigned count = cache_count(&cfg);
    SourcesConfig foreign = cfg;
    foreign.owner = "bob";
    out = call(&foreign, req, 1);
    CHECK(strstr(out, "unknown_source"));
    free(out);
    SourcesConfig absent = cfg;
    absent.ocr = NULL;
    out = call(&absent, req, 1);
    CHECK(strstr(out, "ocr_unavailable"));
    free(out);
    CHECK(cache_count(&cfg) == count);
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_pdf_ocr\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_page\":1,\"end_page\":2}",
             id, id);
    char good_request[1024];
    snprintf(good_request, sizeof(good_request),
             "{\"contract_version\":1,\"op\":\"read_pdf_ocr\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_page\":2,\"end_page\":2}",
             id, id);
    public_helper_failures(&cfg, req, good_request, bytes, n);
    SourcesOcrConfig monitored = *cfg.ocr;
    MonitorProbe probe = {0};
    monitored.test_event = monitor_event;
    monitored.test_context = &probe;
    SourcesConfig active = cfg;
    active.ocr = &monitored;
    active.cancelled = monitor_cancelled;
    active.cancellation = &probe;
    out = call(&active, req, 1);
    CHECK(strstr(out, "cancelled") && probe.polls);
    free(out);
    CHECK(cache_count(&cfg) == count);
    probe = (MonitorProbe){.expire = 1, .deadline = monotonic() + 20};
    active.deadline_monotonic = probe.deadline;
    out = call(&active, req, 1);
    CHECK(strstr(out, "deadline_exceeded") && probe.polls);
    free(out);
    CHECK(cache_count(&cfg) == count);
    out = call(&cfg, req, 0);
    CHECK(strstr(out, "\"page\":1") && strstr(out, "\"page\":2") &&
          strstr(out, "7319"));
    free(out);
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_pdf_ocr\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_page\":2,\"end_page\":2,"
             "\"operation_id\":\"pdf-ocr\",\"timeout_ms\":10000}",
             id, id);
    out = call(&cfg, req, 0);
    cfg.root = NULL;
    again = call(&cfg, req, 0);
    CHECK(!strcmp(out, again));
    free(out);
    free(again);
    cfg.root = root;
    CHECK(ocr_pdf_fixture(path, bytes, n, 1));
    out = call(&cfg,
               "{\"contract_version\":1,\"op\":\"register_pdf\",\"path\":"
               "\"image.pdf\"}",
               0);
    CHECK(json_parse(&j, out, strlen(out)));
    char *big = json_string(&j, json_get(&j, 0, "source_id"));
    CHECK(big);
    free(out);
    snprintf(req, sizeof(req),
             "{\"contract_version\":1,\"op\":\"read_pdf_ocr\",\"source_id\":\"%"
             "s\",\"version\":\"%s\",\"start_page\":2,\"end_page\":2}",
             big, big);
    count = cache_count(&cfg);
    out = call(&cfg, req, 1);
    CHECK(strstr(out, "image_dimension_limit"));
    free(out);
    CHECK(cache_count(&cfg) == count);
    free(big);
    free(id);
}
static void uploaded_ocr(SourcesConfig cfg,const char *library,const unsigned char *bytes,size_t n){
    char state[]="/tmp/samosa-upload-ocr-XXXXXX";CHECK(mkdtemp(state));cfg.root=NULL;cfg.state=state;cfg.owner="alice";
    unsigned char *copy=malloc(n);CHECK(copy);memcpy(copy,bytes,n);char *body=NULL;size_t response_length;FILE *response=open_memstream(&body,&response_length);CHECK(response);CHECK(!sources_upload(&cfg,SOURCES_UPLOAD_IMAGE,copy,n,response)&&!fclose(response));free(copy);
    Json j;CHECK(json_parse(&j,body,strlen(body)));char *id=json_string(&j,json_get(&j,0,"source_id"));CHECK(id);free(body);char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_image\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":10}",id,id);body=call(&cfg,request,0);CHECK(strstr(body,"TOKEN 7319")&&strstr(body,"ocr_recognition_uncertain"));char *again=call(&cfg,request,0);CHECK(!strcmp(body,again));free(body);free(again);cfg.owner="bob";body=call(&cfg,request,1);CHECK(strstr(body,"unknown_source"));free(body);free(id);cfg.owner="alice";
    if(library){char pdf_path[512],pdfium[4096];CHECK(realpath(library,pdfium));cfg.pdfium_library=pdfium;snprintf(pdf_path,sizeof(pdf_path),"%s/input.pdf",state);CHECK(ocr_pdf_fixture(pdf_path,bytes,n,0));FILE *input=fopen(pdf_path,"rb");CHECK(input&&!fseek(input,0,SEEK_END));long length=ftell(input);CHECK(length>0&&!fseek(input,0,SEEK_SET));copy=malloc((size_t)length);CHECK(copy&&fread(copy,1,(size_t)length,input)==(size_t)length&&!fclose(input));response=open_memstream(&body,&response_length);CHECK(response);CHECK(!sources_upload(&cfg,SOURCES_UPLOAD_PDF,copy,(size_t)length,response)&&!fclose(response));free(copy);CHECK(!unlink(pdf_path));CHECK(json_parse(&j,body,strlen(body)));id=json_string(&j,json_get(&j,0,"source_id"));CHECK(id);free(body);
        snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"read_pdf_ocr\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_page\":2,\"end_page\":2}",id,id);body=call(&cfg,request,0);CHECK(strstr(body,"TOKEN 7319")&&strstr(body,"\"page\":2"));again=call(&cfg,request,0);CHECK(!strcmp(body,again));free(body);free(again);free(id);
    }
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--sources-native-worker"))
        return sources_native_worker_main(argc, argv);
    if (argc != 5 && argc != 6 && argc != 8)
        return 2;
    char paths[4][4096];
    for (unsigned i = 0; i < 4; i++)
        CHECK(realpath(argv[i + 1], paths[i]));
    SourcesOcrConfig engine = {.tesseract_library = paths[0],
                               .leptonica_library = paths[1],
                               .traineddata_file = paths[2],
                               .language = "eng"};
    char native_self[4096];
    SourcesConfig cfg = {.ocr = &engine,
                         .native_helper = sources_native_self(
                             native_self, sizeof(native_self), argv[0])};
    CHECK(cfg.native_helper);
    FILE *f = fopen(paths[3], "rb");
    CHECK(f);
    unsigned char *bytes = malloc(SOURCES_INPUT_MAX + 1);
    CHECK(bytes);
    size_t n = fread(bytes, 1, SOURCES_INPUT_MAX + 1, f);
    CHECK(n && n <= SOURCES_INPUT_MAX && !ferror(f));
    CHECK(!fclose(f));
    SourcesOcrResult out = {0};
    const char *error = sources_ocr_extract(&cfg, bytes, n, &out);
    if (error)
        fprintf(stderr, "OCR error: %s\n", error);
    CHECK(!error);
    CHECK(strstr(out.text, "TOKEN 7319"));
    CHECK(out.width == 680 && out.height == 150 && out.confidence > 0 &&
          out.confidence <= 100);
    char fingerprint[65];
    CHECK(!sources_ocr_fingerprint(&cfg, fingerprint) &&
          !strcmp(fingerprint, out.fingerprint));
    sources_ocr_free(&out);
    CHECK(!out.text);
    SourcesConfig absent = {0};
    error = sources_ocr_extract(&absent, bytes, n, &out);
    CHECK(error && !strcmp(error, "ocr_unavailable") && !out.text);
    SourcesConfig cancel = cfg;
    cancel.cancelled = stopped;
    error = sources_ocr_extract(&cancel, bytes, n, &out);
    CHECK(error && !strcmp(error, "cancelled") && !out.text);
    error = sources_ocr_extract(&cfg, bytes, SOURCES_INPUT_MAX + 1, &out);
    CHECK(error && !strcmp(error, "source_too_large"));
    error = sources_ocr_extract(&cfg, "bad", 3, &out);
    CHECK(error && !strcmp(error, "invalid_image"));
    const char large[] = "P6\n9999 2\n255\nABC";
    error = sources_ocr_extract(&cfg, large, sizeof(large) - 1, &out);
    if (error)
        fprintf(stderr, "Large raster: %s\n", error);
    CHECK(error && !strcmp(error, "image_dimension_limit"));
    if(argc==8){
        indexed_pdf(cfg,argv[5],bytes,n,argv[6],argv[7]);free(bytes);
        printf("PASS native PDF OCR index/publication: %u checks\n",checks);return 0;
    }
    helper_tests(cfg, bytes, n);
    parent_death_test(cfg, bytes, n);
    monitor_tests(cfg, bytes, n);
    error = sources_ocr_extract(&cfg, bytes, n, &out);
    CHECK(!error && strstr(out.text, "TOKEN 7319"));
    sources_ocr_free(&out);
    public_image(cfg, bytes, n);
    if (argc >= 6){
        public_pdf(cfg, argv[5], bytes, n);
        indexed_pdf(cfg,argv[5],bytes,n,argc==8?argv[6]:NULL,argc==8?argv[7]:NULL);
    }
    uploaded_ocr(cfg,argc>=6?argv[5]:NULL,bytes,n);
    free(bytes);
    printf(
        "PASS native/public OCR: %u checks; numeric sentinel, provenance, "
        "fingerprint, owner cache, immutable snapshot, durable retry, active "
        "monitor cancellation/deadline, runtime/raster limits and survivor\n",
        checks);
    return 0;
}
