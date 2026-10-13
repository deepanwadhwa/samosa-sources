#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "native.h"
#include "control.h"
#include "json.h"
#include "pdf.h"
#include "sha256.h"
#include "fetch.h"
#include "fetch_public.h"
#include "chutni.h"
#include "semantic.h"
#ifdef SAMOSA_SOURCES_MEDIA_WORKER
#include "media_decode.h"
#include "media_artifacts.h"
#include "media_video.h"
#endif
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifdef __APPLE__
#include <libproc.h>
#include <mach-o/dyld.h>
#endif
extern char **environ;
#define NATIVE_RESPONSE_MAX (2u * 1024u * 1024u)
static size_t response_limit(unsigned kind){return kind==SOURCES_NATIVE_FETCH?SOURCES_HTTP_RESPONSE_MAX:NATIVE_RESPONSE_MAX;}
#define NATIVE_MAGIC 0x534e5731u
#define NATIVE_MEMORY_MAX (512u * 1024u * 1024u)
#if defined(__SANITIZE_ADDRESS__)
#define NATIVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NATIVE_ASAN 1
#endif
#endif
/* Same executable/architecture private protocol. Never a public wire format. */
typedef struct {
    uint32_t magic, kind, length, start, end;
    double deadline;
    char source[65], version[65], locator[4096];
} Request;
typedef struct {
    int stopped;
    double deadline;
} Control;
static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
const char *sources_native_self(char *path, size_t size, const char *argv0) {
#ifdef __linux__
    (void)argv0;
    if (size < sizeof("/proc/self/exe"))
        return NULL;
    memcpy(path, "/proc/self/exe", sizeof("/proc/self/exe"));
    return path;
#elif defined(__APPLE__)
    (void)argv0;
    char executable[4096];
    uint32_t length = sizeof(executable);
    if (_NSGetExecutablePath(executable, &length))
        return NULL;
    char resolved[4096];
    if (!realpath(executable, resolved) || strlen(resolved) >= size)
        return NULL;
    memcpy(path, resolved, strlen(resolved) + 1);
    return path;
#else
    char resolved[4096];
    if (!argv0 || !realpath(argv0, resolved) || strlen(resolved) >= size)
        return NULL;
    memcpy(path, resolved, strlen(resolved) + 1);
    return path;
#endif
}
static ssize_t send_bytes(int fd, const void *bytes, size_t n) {
#ifdef MSG_NOSIGNAL
    return send(fd, bytes, n, MSG_NOSIGNAL);
#else
    return send(fd, bytes, n, 0);
#endif
}
static int child_io(int fd, void *bytes, size_t n, int write_it) {
    size_t offset = 0;
    while (offset < n) {
        ssize_t got =
            write_it
                ? send_bytes(fd, (unsigned char *)bytes + offset, n - offset)
                : read(fd, (unsigned char *)bytes + offset, n - offset);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return 0;
        offset += (size_t)got;
    }
    return 1;
}
static const char *worker_budget(const SourcesConfig *cfg, pid_t pid) {
    const char *error = sources_interrupted(cfg);
    if (error)
        return error;
    uint64_t resident = 0;
#ifdef __APPLE__
    struct proc_taskinfo info;
    errno = 0;
    int n = proc_pidinfo((int)pid, PROC_PIDTASKINFO, 0, &info, sizeof(info));
    if (n == (int)sizeof(info))
        resident = info.pti_resident_size;
    else if (errno && errno != ESRCH)
        return "native_helper_resource_unavailable";
#elif defined(__linux__)
    char path[64], buffer[128];
    snprintf(path, sizeof(path), "/proc/%ld/statm", (long)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? NULL : "native_helper_resource_unavailable";
    }
    ssize_t n;
    do {
        n = read(fd, buffer, sizeof(buffer) - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0)
        return "native_helper_resource_unavailable";
    buffer[n] = 0;
    char *end = NULL;
    errno = 0;
    strtoull(buffer, &end, 10);
    if (errno || end == buffer)
        return "native_helper_resource_unavailable";
    char *begin = end;
    uint64_t pages = strtoull(begin, &end, 10);
    long page_size = sysconf(_SC_PAGESIZE);
    if (errno || end == begin || page_size <= 0 ||
        pages > UINT64_MAX / (uint64_t)page_size)
        return "native_helper_resource_unavailable";
    resident = pages * (uint64_t)page_size;
#else
    (void)pid;
    return "native_helper_resource_unavailable";
#endif
    size_t limit =
        cfg->native_memory_limit ? cfg->native_memory_limit : NATIVE_MEMORY_MAX;
    return resident > limit ? "native_helper_memory_limit" : NULL;
}
typedef struct {const SourcesConfig *cfg;pid_t pid;const char *error;} RateBudget;
static int rate_budget_cancelled(void *opaque){RateBudget *budget=opaque;budget->error=worker_budget(budget->cfg,budget->pid);return budget->error!=NULL;}
static const char *parent_io(int fd, void *bytes, size_t n, int write_it,
                             const SourcesConfig *cfg, pid_t pid) {
    size_t offset = 0;
    while (offset < n) {
        const char *error = worker_budget(cfg, pid);
        if (error)
            return error;
        struct pollfd p = {.fd = fd, .events = write_it ? POLLOUT : POLLIN};
        int ready = poll(&p, 1, 10);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return "native_helper_failed";
        if (!ready)
            continue;
        ssize_t got =
            write_it
                ? send_bytes(fd, (unsigned char *)bytes + offset, n - offset)
                : read(fd, (unsigned char *)bytes + offset, n - offset);
        if (got < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (got <= 0)
            return "native_helper_failed";
        offset += (size_t)got;
    }
    return NULL;
}
const char *sources_native_error_code(const char *code) {
    if(!code)return "native_helper_protocol";
    if(!strcmp(code,"fetch_redirect_limit"))return "fetch_redirect_limit";
    if(!strcmp(code,"fetch_redirect_loop"))return "fetch_redirect_loop";
    if(!strcmp(code,"fetch_redirect_invalid"))return "fetch_redirect_invalid";
    if(!strcmp(code,"fetch_robots_denied"))return "fetch_robots_denied";
    static const char *codes[] = {"fetch_dns_failed", "fetch_nonpublic_address", "fetch_public_port", "fetch_address_changed", "fetch_offline", "fetch_tls_trust_invalid", "fetch_tls_verification_failed", "fetch_tls_failed", "fetch_header_limit", "fetch_unavailable", "fetch_failed", "fetch_http_status", "fetch_too_large", "unauthorized_url", "invalid_fetch_configuration", "cancelled",
                                  "deadline_exceeded",
                                  "invalid_configuration",
                                  "inference_unavailable",
                                  "inference_response_invalid",
                                  "resource_limit",
                                  "state_unavailable",
                                  "invalid_request",
                                  "invalid_range",
                                  "source_too_large",
                                  "media_artifact_changed",
                                  "invalid_media",
                                  "unsupported_audio_format",
                                  "media_artifact_unavailable",
                                  "media_audio_unavailable",
                                  "media_decode_failed",
                                  "media_decoder_unavailable",
                                  "media_external_reference_refused",
                                  "media_format_changed",
                                  "media_frame_limit",
                                  "media_packet_limit",
                                  "media_stream_limit",
                                  "media_timeline_unsupported",
                                  "media_video_unavailable",
                                  "media_visual_format_unsupported",
                                  "media_orientation_unsupported",
                                  "ocr_unavailable",
                                  "ocr_artifact_invalid",
                                  "ocr_artifact_changed",
                                  "ocr_adapter_incompatible",
                                  "invalid_image",
                                  "image_dimension_limit",
                                  "image_decode_failed",
                                  "ocr_model_invalid",
                                  "ocr_recognition_failed",
                                  "ocr_output_limit",
                                  "ocr_invalid_utf8",
                                  "ocr_invalid_confidence",
                                  "pdf_unavailable",
                                  "pdf_adapter_invalid",
                                  "pdf_adapter_changed",
                                  "pdf_adapter_incompatible",
                                  "pdf_encrypted",
                                  "pdf_malformed",
                                  "pdf_page_failed",
                                  "pdf_render_failed",
                                  "pdf_text_failed",
                                  "pdf_text_unreadable",
                                  "extraction_too_large",
                                  "chutni_unavailable",
                                  "chutni_sdk_unavailable",
                                  "chutni_busy",
                                  "chutni_not_found",
                                  "chutni_version_unsupported",
                                  "chutni_corrupt_store",
                                  "chutni_failed",
                                  "chutni_response_invalid",
                                  "chutni_publish_disabled",
                                  "chutni_store_changed",
                                  "chutni_source_changed",
                                  "chutni_publication_conflict",
                                  "corrupt_index"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(*codes); i++)
        if (code && !strcmp(code, codes[i]))
            return codes[i];
    return "native_helper_protocol";
}
static void terminate(pid_t pid) {
    if (pid > 0) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
}
static const char *run(const SourcesConfig *cfg, Request *request,
                       const void *bytes, char **response,
                       size_t *response_bytes) {
    *response = NULL;
    *response_bytes = 0;
    if (!cfg || !cfg->native_helper || !*cfg->native_helper)
        return "native_helper_unavailable";
    SourcesConfig work = *cfg;
    if (work.native_memory_limit &&
        (work.native_memory_limit < 16u * 1024u * 1024u ||
         work.native_memory_limit > NATIVE_MEMORY_MAX))
        return "invalid_configuration";
    const char *initial = sources_interrupted(&work);
    if (initial)
        return initial;
    double current = now();
    if (current < 0)
        return "state_unavailable";
    double duration = request->kind == SOURCES_NATIVE_INFERENCE ? (request->start == 1 ? 120 : 60) : 30;
    if (!work.deadline_monotonic || work.deadline_monotonic > current + duration)
        work.deadline_monotonic = current + duration;
    const char *error = sources_interrupted(&work);
    if (error)
        return error;
    request->deadline = work.deadline_monotonic;
    request->magic = NATIVE_MAGIC;
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        return "native_helper_unavailable";
    for (unsigned i = 0; i < 2; i++) {
        int elevated = fcntl(pair[i], F_DUPFD_CLOEXEC, 4);
        if (elevated < 0) {
            close(pair[0]);
            close(pair[1]);
            return "native_helper_unavailable";
        }
        close(pair[i]);
        pair[i] = elevated;
    }
    for (unsigned i = 0; i < 2; i++)
        if (fcntl(pair[i], F_SETFD, FD_CLOEXEC) < 0) {
            close(pair[0]);
            close(pair[1]);
            return "native_helper_unavailable";
        }
#ifdef SO_NOSIGPIPE
    int no_pipe = 1;
    setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &no_pipe, sizeof(no_pipe));
    setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &no_pipe, sizeof(no_pipe));
#endif
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int actions_ok = 0, attr_ok = 0,
        rc = posix_spawn_file_actions_init(&actions);
    if (!rc) {
        actions_ok = 1;
        rc = posix_spawnattr_init(&attr);
        if (!rc)
            attr_ok = 1;
    }
    short flags =
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    if (!rc)
        rc = posix_spawnattr_setflags(&attr, flags);
    if (!rc)
        rc = posix_spawnattr_setpgroup(&attr, 0);
    if (!rc)
        rc = posix_spawnattr_setsigmask(&attr, &empty);
    if (!rc)
        rc = posix_spawnattr_setsigdefault(&attr, &defaults);
    for (int fd = 0; fd < 3 && !rc; fd++)
        rc = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null",
                                              fd ? O_WRONLY : O_RDONLY, 0600);
    if (!rc)
        rc = posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
#ifdef __linux__
    if (!rc)
        rc = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
#else
    if (!rc && pair[0] != 3)
        rc = posix_spawn_file_actions_addclose(&actions, pair[0]);
    if (!rc && pair[1] != 3)
        rc = posix_spawn_file_actions_addclose(&actions, pair[1]);
#endif
    const SourcesOcrConfig *ocr = cfg->ocr;
    const char *fault = "";
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    fault = cfg->native_test_fault ? cfg->native_test_fault : "";
    if (!strcmp(fault,"media_frame_stall"))
        fault = request->kind == SOURCES_NATIVE_MEDIA_FRAME ? "stall" : "";
    if (!strcmp(fault,"media_frame_descendant_stall"))
        fault = request->kind == SOURCES_NATIVE_MEDIA_FRAME ? "descendant_stall" : "";
    if (!strcmp(fault,"media_audio_stall"))
        fault = request->kind == SOURCES_NATIVE_MEDIA_AUDIO ? "stall" : "";
    if (!strcmp(fault,"media_audio_descendant_stall"))
        fault = request->kind == SOURCES_NATIVE_MEDIA_AUDIO ? "descendant_stall" : "";
#endif
    char *argv[] = {
        (char *)cfg->native_helper,
        "--sources-native-worker",
        (char *)(ocr && ocr->tesseract_library ? ocr->tesseract_library : ""),
        (char *)(ocr && ocr->leptonica_library ? ocr->leptonica_library : ""),
        (char *)(ocr && ocr->traineddata_file ? ocr->traineddata_file : ""),
        (char *)(ocr && ocr->language ? ocr->language : "eng"),
        (char *)(cfg->pdfium_library ? cfg->pdfium_library : ""),
        (char *)fault,
        NULL};
    size_t count = 0;
    while (environ[count])
        count++;
    char **env = calloc(count + 3, sizeof(*env));
    if (!env && !rc)
        rc = ENOMEM;
    if (env) {
        size_t at = 0;
        for (size_t i = 0; i < count; i++)
            if (strncmp(environ[i], "OMP_THREAD_LIMIT=", 17) &&
                strncmp(environ[i], "OMP_NUM_THREADS=", 16))
                env[at++] = environ[i];
        env[at++] = "OMP_THREAD_LIMIT=1";
        env[at++] = "OMP_NUM_THREADS=1";
        env[at] = NULL;
    }
    pid_t pid = -1;
    if (!rc)
        rc = posix_spawn(&pid, cfg->native_helper, &actions, &attr, argv, env);
    free(env);
    if (actions_ok)
        posix_spawn_file_actions_destroy(&actions);
    if (attr_ok)
        posix_spawnattr_destroy(&attr);
    close(pair[1]);
    if (rc) {
        close(pair[0]);
        return "native_helper_unavailable";
    }
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if (work.native_test_event)
        work.native_test_event(work.native_test_context, (int)pid, "started",
                               &work);
#endif
    int original = fcntl(pair[0], F_GETFL, 0);
    if (original < 0 || fcntl(pair[0], F_SETFL, original | O_NONBLOCK) < 0)
        error = "native_helper_failed";
    if (!error)
        error = parent_io(pair[0], request, sizeof(*request), 1, &work, pid);
    if (!error)
        error =
            parent_io(pair[0], (void *)bytes, request->length, 1, &work, pid);
    int replied = 0;unsigned rate_requests=0;
    while (!error && !replied) {
        char type = 0;
        error = parent_io(pair[0], &type, 1, 0, &work, pid);
        if (error)
            break;
        if (type == 'C' || type == 'M') {
#ifdef SAMOSA_SOURCES_TEST_FAULTS
            if (type == 'M' && work.ocr && work.ocr->test_event)
                work.ocr->test_event(work.ocr->test_context, "monitor_poll",
                                     &work);
#else
            if (type == 'M') {
                error = "native_helper_protocol";
                break;
            }
#endif
            const char *stopped = sources_interrupted(&work);
            Control control = {.stopped = stopped != NULL,
                               .deadline = work.deadline_monotonic};
            /* A stopped parent owns teardown; no response callback can delay
             * it. */
            if (stopped) {
                error = stopped;
                break;
            }
            error =
                parent_io(pair[0], &control, sizeof(control), 1, &work, pid);
        }
#ifdef SAMOSA_SOURCES_TEST_FAULTS
        else if (type == 'F') {
            if (work.native_test_event)
                work.native_test_event(work.native_test_context, (int)pid,
                                       "fault_ready", &work);
        } else if (type == 'D') {
            int descendant = 0;
            error = parent_io(pair[0], &descendant, sizeof(descendant), 0,
                              &work, pid);
            if (!error && descendant <= 0)
                error = "native_helper_protocol";
            if (!error && work.native_test_event)
                work.native_test_event(work.native_test_context, descendant,
                                       "descendant", &work);
        }
#endif
        else if(type=='P'){
            char origin[257];
            if(request->kind!=SOURCES_NATIVE_FETCH||!work.fetch_public_web||!work.fetch_public_only||++rate_requests>42){error="native_helper_protocol";break;}
            error=parent_io(pair[0],origin,sizeof(origin),0,&work,pid);
            if(!error&&(!memchr(origin,0,sizeof(origin))||!origin[0]))error="native_helper_protocol";
            if(!error){
                RateBudget budget={.cfg=&work,.pid=pid};SourcesConfig waiting=work;waiting.cancelled=rate_budget_cancelled;waiting.cancellation=&budget;
                error=sources_fetch_rate_wait(&waiting,origin);if(budget.error)error=budget.error;
            }
            if(!error){Control control={.stopped=0,.deadline=work.deadline_monotonic};error=parent_io(pair[0],&control,sizeof(control),1,&work,pid);}
        }
        else if (type == 'R') {
            uint32_t n = 0;
            error = parent_io(pair[0], &n, sizeof(n), 0, &work, pid);
            if (!error && (!n || n > response_limit(request->kind)))
                error = "native_helper_protocol";
            if (!error) {
                *response = malloc((size_t)n + 1);
                if (!*response)
                    error = "resource_limit";
                else {
                    error = parent_io(pair[0], *response, n, 0, &work, pid);
                    (*response)[n] = 0;
                    *response_bytes = n;
                    replied = 1;
                }
            }
        } else
            error = "native_helper_protocol";
    }
    if (!error) {
        for (;;) {
            error = worker_budget(&work, pid);
            if (error)
                break;
            struct pollfd p = {.fd = pair[0], .events = POLLIN};
            int ready = poll(&p, 1, 10);
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0) {
                error = "native_helper_failed";
                break;
            }
            if (!ready)
                continue;
            char byte;
            ssize_t n = read(pair[0], &byte, 1);
            if (n < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            if (n == 0)
                break;
            error = "native_helper_protocol";
            break;
        }
    }
    close(pair[0]);
    if (!error) {
        int status = 0;
        for (;;) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid) {
                pid = -1;
                if (!WIFEXITED(status) || WEXITSTATUS(status))
                    error = "native_helper_failed";
                break;
            }
            if (got < 0 && errno != EINTR) {
                error = "native_helper_failed";
                break;
            }
            error = worker_budget(&work, pid);
            if (error)
                break;
            struct timespec delay = {0, 1000000};
            nanosleep(&delay, NULL);
        }
    }
    if (pid > 0)
        terminate(pid);
    if (!error) {
        Json j;
        if (!json_parse(&j, *response, *response_bytes) ||
            j.tokens[0].type != JT_OBJECT)
            error = "native_helper_protocol";
        else {
            int token = json_get(&j, 0, "error");
            if (token >= 0) {
                char *code = json_string(&j, json_get(&j, token, "code"));
                error = sources_native_error_code(code);
                free(code);
            }
        }
    }
    if (error) {
        free(*response);
        *response = NULL;
        *response_bytes = 0;
    }
    return error;
}
static int valid_media_fingerprint(const char *value) {
    if (!value || strlen(value) != 64) return 0;
    for (size_t i=0;i<64;i++)
        if (!strchr("0123456789abcdef",value[i])) return 0;
    return 1;
}
const char *sources_inference_process_call(void *context,const SourcesInferenceControl *control,
    const char *input,size_t length,char **response,size_t *bytes) {
    SourcesInferenceProcess *process=context;
    if(!process||!process->sources||!process->executable||process->executable[0]!='/'||
       !process->arguments_json||!control||!input||!length||
       (process->temporal_frames!=0&&process->temporal_frames!=1)||
       length>(process->temporal_frames?25165824u:2097152u))return "invalid_configuration";
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);
    if(!out)return "resource_limit";
    fputs("{\"executable\":",out);json_quote(out,process->executable,strlen(process->executable));
    fprintf(out,",\"temporal_frames\":%d,\"arguments\":%s,\"request\":",process->temporal_frames,process->arguments_json);
    json_quote(out,input,length);fputc('}',out);
    if(fclose(out)||n>(process->temporal_frames?50331648u:4194304u)){free(body);return "resource_limit";}
    SourcesConfig cfg=*process->sources;cfg.deadline_monotonic=control->deadline_monotonic;
    cfg.cancelled=control->cancelled;cfg.cancellation=control->cancellation;
    Request request={.kind=SOURCES_NATIVE_INFERENCE,.length=(uint32_t)n,.start=(uint32_t)process->temporal_frames};
    const char *error=run(&cfg,&request,body,response,bytes);free(body);
    if(!error){
        Json j;int token=-1;
        if(json_parse(&j,*response,*bytes))token=json_get(&j,0,"response");
        if(token<0||j.tokens[token].type!=JT_OBJECT)error="inference_response_invalid";
        else{size_t size=j.tokens[token].end-j.tokens[token].begin;char *copy=malloc(size+1);
            if(!copy)error="resource_limit";
            else{memcpy(copy,j.text+j.tokens[token].begin,size);copy[size]=0;free(*response);*response=copy;*bytes=size;}}
        if(error){free(*response);*response=NULL;*bytes=0;}
    }
    return error;
}
const char *sources_native_media_fingerprint(const SourcesConfig *cfg, char out[65]) {
    if (!out) return "invalid_request";
    out[0]=0;
    if (!cfg || !cfg->media_helper || !*cfg->media_helper) return "media_unavailable";
    SourcesConfig selected=*cfg; selected.native_helper=cfg->media_helper;
    unsigned char sentinel=0;
    Request request={.kind=SOURCES_NATIVE_MEDIA_FINGERPRINT,.length=1};
    char *body=NULL; size_t n=0;
    const char *error=run(&selected,&request,&sentinel,&body,&n);
    if (!error) {
        Json j; json_parse(&j,body,n);
        char *fp=json_string(&j,json_get(&j,0,"fingerprint"));
        if (!valid_media_fingerprint(fp)) error="native_helper_protocol";
        else memcpy(out,fp,65);
        free(fp);
    }
    free(body);
    return error;
}
static const char *native_media_request(const SourcesConfig *cfg, const void *bytes,
    size_t length, size_t start, size_t end, const char *expected,
    char **body, size_t *n, int visual) {
    if (!body || !n) return "invalid_request";
    *body = NULL;
    *n = 0;
    if (!valid_media_fingerprint(expected)) return "invalid_configuration";
    if (!cfg || !cfg->media_helper || !*cfg->media_helper) return "media_unavailable";
    SourcesConfig selected=*cfg; selected.native_helper=cfg->media_helper;
    if (!bytes || !length || length > SOURCES_INPUT_MAX) return "source_too_large";
    if (end <= start || end - start > 30000 || end > 14400000) return "invalid_range";
    Request request = {.kind = visual==2?SOURCES_NATIVE_MEDIA_LAST_FRAME:visual?SOURCES_NATIVE_MEDIA_FRAME:SOURCES_NATIVE_MEDIA_AUDIO,
        .length = (uint32_t)length, .start = (uint32_t)start, .end = (uint32_t)end};
    const char *error = run(&selected, &request, bytes, body, n);
    if (!error) error = visual?sources_media_frame_validate(cfg,*body,*n,start,end):sources_media_response_validate(cfg, *body, *n, start, end);
    if (!error) {
        Json j; json_parse(&j,*body,*n);
        char *fp=json_string(&j,json_get(&j,0,"fingerprint"));
        if (!fp || strcmp(fp,expected)) error="media_artifact_changed";
        free(fp);
    }
    if (error) { free(*body); *body = NULL; *n = 0; }
    return error;
}
const char *sources_native_media_audio(const SourcesConfig *cfg,const void *bytes,size_t length,size_t start,size_t end,const char *fp,char **body,size_t *n) {
    return native_media_request(cfg,bytes,length,start,end,fp,body,n,0);
}
const char *sources_native_media_frame(const SourcesConfig *cfg,const void *bytes,size_t length,size_t start,size_t end,const char *fp,char **body,size_t *n) {
    return native_media_request(cfg,bytes,length,start,end,fp,body,n,1);
}
const char *sources_native_media_last_frame(const SourcesConfig *cfg,const void *bytes,size_t length,size_t start,size_t end,const char *fp,char **body,size_t *n) {
    return native_media_request(cfg,bytes,length,start,end,fp,body,n,2);
}
const char *sources_native_media_probe(const SourcesConfig *cfg,const void *bytes,
    size_t length,const char *expected,char **body,size_t *n) {
    if(!body||!n)return "invalid_request";
    *body=NULL;*n=0;
    if(!valid_media_fingerprint(expected))return "invalid_configuration";
    if(!cfg||!cfg->media_helper||!*cfg->media_helper)return "media_unavailable";
    if(!bytes||!length||length>SOURCES_INPUT_MAX)return "source_too_large";
    SourcesConfig selected=*cfg;selected.native_helper=cfg->media_helper;
    Request request={.kind=SOURCES_NATIVE_MEDIA_PROBE,.length=(uint32_t)length};
    const char *error=run(&selected,&request,bytes,body,n);
    if(!error)error=sources_media_probe_validate(cfg,*body,*n);
    if(!error){Json j;json_parse(&j,*body,*n);char *fp=json_string(&j,json_get(&j,0,"fingerprint"));if(!fp||strcmp(fp,expected))error="media_artifact_changed";free(fp);}
    if(error){free(*body);*body=NULL;*n=0;}return error;
}
const char *sources_native_video_probe(const SourcesConfig *cfg,const void *bytes,
    size_t length,const char *expected,char **body,size_t *n) {
    if(!body||!n)return "invalid_request";
    *body=NULL;*n=0;
    if(!valid_media_fingerprint(expected))return "invalid_configuration";
    if(!cfg||!cfg->media_helper||!*cfg->media_helper)return "media_unavailable";
    if(!bytes||!length||length>SOURCES_INPUT_MAX)return "source_too_large";
    SourcesConfig selected=*cfg;selected.native_helper=cfg->media_helper;
    Request request={.kind=SOURCES_NATIVE_VIDEO_PROBE,.length=(uint32_t)length};
    const char *error=run(&selected,&request,bytes,body,n);
    if(!error)error=sources_video_probe_validate(cfg,*body,*n);
    if(!error){Json j;json_parse(&j,*body,*n);char *fp=json_string(&j,json_get(&j,0,"fingerprint"));if(!fp||strcmp(fp,expected))error="media_artifact_changed";free(fp);}
    if(error){free(*body);*body=NULL;*n=0;}return error;
}
static const char *native_http_capture(const SourcesConfig *cfg,const char *url,const char *method,const char *headers,const char *post,
                                 size_t limit,unsigned char **bytes,size_t *length,char **final_url,char **urls) {
    *bytes=NULL; *length=0;
    if(final_url)*final_url=NULL;
    if(urls)*urls=NULL;
    if(!limit||limit>SOURCES_FETCH_MAX)return "invalid_fetch_configuration";
    const char *error=sources_fetch_authorize(cfg,url);
    if(!error)error=sources_http_options_validate(method,headers,post);
    if(error)return error;
    char *input=NULL;size_t input_length=0;
    FILE *out=open_memstream(&input,&input_length);
    if(!out)return "resource_limit";
    fputs("{\"origin\":",out);json_quote(out,cfg->fetch_origin,strlen(cfg->fetch_origin));
    fputs(",\"url\":",out);json_quote(out,url,strlen(url));
    if(cfg->fetch_ca_pem){fputs(",\"ca_pem\":",out);json_quote(out,cfg->fetch_ca_pem,strlen(cfg->fetch_ca_pem));}
    if(cfg->fetch_public_only)fputs(",\"public_only\":1",out);
    if(final_url&&cfg->fetch_public_web)fputs(",\"public_web\":1",out);
    fputs(",\"method\":",out);json_quote(out,method,strlen(method));
    if(headers){fputs(",\"headers\":",out);json_quote(out,headers,strlen(headers));}
    if(post){fputs(",\"post\":",out);json_quote(out,post,strlen(post));}
    fprintf(out,",\"max_bytes\":%zu}",limit);
    if(fclose(out)){free(input);return "resource_limit";}
    if(input_length>SOURCES_HTTP_REQUEST_MAX){free(input);return "invalid_fetch_configuration";}
    SourcesConfig selected=*cfg;selected.native_helper=cfg->fetch_helper;
    Request request={.kind=SOURCES_NATIVE_FETCH,.length=(uint32_t)input_length};
    char *body=NULL;size_t n=0;
    error=run(&selected,&request,input,&body,&n);free(input);
    if(!error){
        if(final_url&&cfg->fetch_public_web)error=sources_fetch_validate_route(cfg,url,body,n,bytes,length,final_url,urls);
        else error=sources_fetch_validate(url,body,n,bytes,length);
    }
    if(!error&&*length>limit){free(*bytes);*bytes=NULL;*length=0;error="native_helper_protocol";}
    free(body);return error;
}
const char *sources_native_http_limited(const SourcesConfig *cfg,const char *url,const char *method,const char *headers,const char *post,size_t limit,unsigned char **bytes,size_t *length){
    return native_http_capture(cfg,url,method,headers,post,limit,bytes,length,NULL,NULL);
}
const char *sources_native_fetch_capture(const SourcesConfig *cfg,const char *url,unsigned char **bytes,size_t *length,char **final_url,char **urls){
    *bytes=NULL;*length=0;*final_url=NULL;*urls=NULL;
    if(!cfg||cfg->fetch_public_web<0||cfg->fetch_public_web>1||(cfg->fetch_public_web&&!cfg->fetch_public_only))return "invalid_fetch_configuration";
    return native_http_capture(cfg,url,"GET",NULL,NULL,SOURCES_FETCH_MAX,bytes,length,final_url,urls);
}
const char *sources_native_http(const SourcesConfig *cfg,const char *url,const char *method,const char *headers,const char *post,unsigned char **bytes,size_t *length){
    return sources_native_http_limited(cfg,url,method,headers,post,SOURCES_FETCH_MAX,bytes,length);
}
const char *sources_native_fetch(const SourcesConfig *cfg,const char *url,unsigned char **bytes,size_t *length){
    char *final_url=NULL,*urls=NULL;const char *error=sources_native_fetch_capture(cfg,url,bytes,length,&final_url,&urls);free(final_url);free(urls);return error;
}
const char *sources_native_chutni(const SourcesConfig *cfg,const char *query,char **body,size_t *n){
    *body=NULL;*n=0;
    if(!cfg->chutni_library||!cfg->chutni_store||!cfg->chutni_helper||cfg->chutni_library[0]!='/'||cfg->chutni_store[0]!='/'||strlen(cfg->chutni_library)>4096||strlen(cfg->chutni_store)>4096)return "chutni_unavailable";
    char *input=NULL;size_t length=0;FILE *out=open_memstream(&input,&length);if(!out)return "resource_limit";
    fputs("{\"library\":",out);json_quote(out,cfg->chutni_library,strlen(cfg->chutni_library));fputs(",\"store\":",out);json_quote(out,cfg->chutni_store,strlen(cfg->chutni_store));fputs(",\"query\":",out);json_quote(out,query,strlen(query));fputc('}',out);
    if(fclose(out)){free(input);return "resource_limit";}
    SourcesConfig selected=*cfg;selected.native_helper=cfg->chutni_helper;
    Request request={.kind=SOURCES_NATIVE_CHUTNI,.length=(uint32_t)length};
    const char *error=run(&selected,&request,input,body,n);free(input);
    if(!error)error=sources_chutni_validate(*body,*n,query);
    if(error){free(*body);*body=NULL;*n=0;}return error;
}
const char *sources_native_chutni_semantic(const SourcesConfig *cfg,const char *query,char **body,size_t *n){
    *body=NULL;*n=0;
    if(!cfg->chutni_library||!cfg->chutni_store||!cfg->chutni_helper||cfg->chutni_library[0]!='/'||cfg->chutni_store[0]!='/'||strlen(cfg->chutni_library)>4096||strlen(cfg->chutni_store)>4096)return "chutni_unavailable";
    if(!cfg->chutni_store_id||strlen(cfg->chutni_store_id)!=36||sources_semantic_arguments_validate(query))return "invalid_semantic_configuration";
    char *input=NULL;size_t length=0;FILE *out=open_memstream(&input,&length);if(!out)return "resource_limit";
    fputs("{\"library\":",out);json_quote(out,cfg->chutni_library,strlen(cfg->chutni_library));fputs(",\"store\":",out);json_quote(out,cfg->chutni_store,strlen(cfg->chutni_store));fputs(",\"store_id\":",out);json_quote(out,cfg->chutni_store_id,strlen(cfg->chutni_store_id));fputs(",\"query\":",out);fputs(query,out);fputc('}',out);
    if(fclose(out)){free(input);return "resource_limit";}
    SourcesConfig selected=*cfg;selected.native_helper=cfg->chutni_helper;
    Request request={.kind=SOURCES_NATIVE_CHUTNI,.length=(uint32_t)length};
    const char *error=run(&selected,&request,input,body,n);free(input);
    if(!error)error=sources_chutni_semantic_validate(*body,*n,query,cfg->chutni_store_id);
    if(error){free(*body);*body=NULL;*n=0;}return error;
}
const char *sources_native_chutni_read(const SourcesConfig *cfg,const char *query,char **body,size_t *n){
    *body=NULL;*n=0;if(!cfg->chutni_library||!cfg->chutni_store||!cfg->chutni_helper||cfg->chutni_library[0]!='/'||cfg->chutni_store[0]!='/'||strlen(cfg->chutni_library)>4096||strlen(cfg->chutni_store)>4096)return "chutni_unavailable";
    char *input=NULL;size_t length=0;FILE *out=open_memstream(&input,&length);if(!out)return "resource_limit";
    fputs("{\"library\":",out);json_quote(out,cfg->chutni_library,strlen(cfg->chutni_library));fputs(",\"store\":",out);json_quote(out,cfg->chutni_store,strlen(cfg->chutni_store));fputs(",\"request\":",out);fputs(query,out);fputc('}',out);
    if(fclose(out)){free(input);return "resource_limit";}SourcesConfig selected=*cfg;selected.native_helper=cfg->chutni_helper;Request request={.kind=SOURCES_NATIVE_CHUTNI_READ,.length=(uint32_t)length};const char *error=run(&selected,&request,input,body,n);free(input);
    if(!error)error=sources_chutni_read_validate(query,*body,*n);
    if(error){free(*body);*body=NULL;*n=0;}return error;
}
const char *sources_native_chutni_embedding(const SourcesConfig *cfg,const char *metadata,char **body,size_t *n){
    *body=NULL;*n=0;
    if(!cfg||!metadata||strlen(metadata)>524288)return "invalid_request";
    if(!cfg->chutni_library||!cfg->chutni_store||!cfg->chutni_helper||!cfg->chutni_store_id)return "chutni_unavailable";
    Json request;
    if(!json_parse(&request,metadata,strlen(metadata)))return "invalid_request";
    const char *keys[]={"library","store","store_id"};const char *values[]={cfg->chutni_library,cfg->chutni_store,cfg->chutni_store_id};
    for(size_t i=0;i<3;i++){char *value=json_string(&request,json_get(&request,0,keys[i]));int good=value&&!strcmp(value,values[i]);free(value);if(!good)return "invalid_request";}
    int arguments=json_get(&request,0,"arguments");
    if(arguments<0)return "invalid_request";
    if(request.tokens[arguments].type!=JT_NULL&&!cfg->chutni_publish)return "chutni_publish_disabled";
    if(request.tokens[arguments].type!=JT_NULL){
        char *expected=NULL;
        const char *profile_error=sources_semantic_arguments(cfg->chutni_semantic_profile,&request,json_get(&request,arguments,"vector"),&expected);
        size_t length=request.tokens[arguments].end-request.tokens[arguments].begin;
        int match=!profile_error&&strlen(expected)==length&&!memcmp(expected,request.text+request.tokens[arguments].begin,length);
        free(expected);if(!match)return "invalid_semantic_configuration";
    }
    SourcesConfig selected=*cfg;selected.native_helper=cfg->chutni_helper;
    Request wire={.kind=SOURCES_NATIVE_CHUTNI_EMBEDDING,.length=(uint32_t)strlen(metadata)};
    const char *error=run(&selected,&wire,metadata,body,n);
    if(!error)error=sources_chutni_embedding_validate(metadata,*body,*n);
    if(error){free(*body);*body=NULL;*n=0;}
    return error;
}
const char *sources_native_chutni_publish(const SourcesConfig *cfg,const void *snapshot,size_t bytes,const char *metadata,char **body,size_t *n){
    *body=NULL;*n=0;if(!cfg->chutni_publish||!cfg->chutni_helper)return "chutni_publish_disabled";
    size_t text=strlen(metadata);if(text>1048572||bytes>SOURCES_INPUT_MAX)return "invalid_request";
    size_t total=4+text+bytes;unsigned char *input=malloc(total);if(!input)return "resource_limit";uint32_t size=(uint32_t)text;memcpy(input,&size,4);memcpy(input+4,metadata,text);if(bytes)memcpy(input+4+text,snapshot,bytes);
    SourcesConfig selected=*cfg;selected.native_helper=cfg->chutni_helper;Request request={.kind=SOURCES_NATIVE_CHUTNI_PUBLISH,.length=(uint32_t)total};
    const char *error=run(&selected,&request,input,body,n);free(input);
    if(!error)error=sources_chutni_publish_validate(metadata,*body,*n);
    if(error){free(*body);*body=NULL;*n=0;}return error;
}
const char *sources_native_ocr(const SourcesConfig *cfg, const void *bytes,
                               size_t length, int rendered,
                               SourcesOcrResult *out) {
    if (!out)
        return "invalid_request";
    memset(out, 0, sizeof(*out));
    if (!bytes || !length || length > (rendered ? 4194368u : SOURCES_INPUT_MAX))
        return "source_too_large";
    Request request = {.kind = rendered ? SOURCES_NATIVE_RENDERED
                                        : SOURCES_NATIVE_IMAGE,
                       .length = (uint32_t)length};
    char *body = NULL;
    size_t n = 0;
    const char *error = run(cfg, &request, bytes, &body, &n);
    if (!error) {
        Json j;
        size_t width = 0, height = 0, confidence = 0;
        json_parse(&j, body, n);
        out->text = json_string(&j, json_get(&j, 0, "text"));
        char *fingerprint = json_string(&j, json_get(&j, 0, "fingerprint"));
        if (!out->text || strlen(out->text) > SOURCES_READ_MAX ||
            !utf8_valid((const unsigned char *)out->text, strlen(out->text)) ||
            !fingerprint || strlen(fingerprint) != 64 ||
            !json_uint(&j, json_get(&j, 0, "width"), &width) ||
            !json_uint(&j, json_get(&j, 0, "height"), &height) ||
            !json_uint(&j, json_get(&j, 0, "confidence"), &confidence) ||
            !width || !height || width > 4096 || height > 4096 ||
            width * height > 4194304u || confidence > 100)
            error = "native_helper_protocol";
        else {
            for (unsigned i = 0; i < 64; i++)
                if (!strchr("0123456789abcdef", fingerprint[i]))
                    error = "native_helper_protocol";
            if (!error) {
                memcpy(out->fingerprint, fingerprint, 65);
                out->text_bytes = strlen(out->text);
                out->width = (unsigned)width;
                out->height = (unsigned)height;
                out->confidence = (int)confidence;
            }
        }
        free(fingerprint);
    }
    free(body);
    if (error)
        sources_ocr_free(out);
    return error;
}
const char *sources_native_pdf(const SourcesConfig *cfg, const void *bytes,
                               size_t length, const char *source,
                               const char *version, size_t start, size_t end,
                               const char *locator, FILE *out, int ocr) {
    if (!bytes || !length || length > SOURCES_INPUT_MAX || !source ||
        strlen(source) != 64 || !version || strlen(version) != 64 || !locator ||
        strlen(locator) >= 4096)
        return "invalid_request";
    if (start < 1 || end < start || end - start >= 4 || end > INT32_MAX)
        return "invalid_range";
    Request request = {.kind = ocr ? SOURCES_NATIVE_PDF_OCR
                                   : SOURCES_NATIVE_PDF_TEXT,
                       .length = (uint32_t)length,
                       .start = (uint32_t)start,
                       .end = (uint32_t)end};
    memcpy(request.source, source, 65);
    memcpy(request.version, version, 65);
    memcpy(request.locator, locator, strlen(locator) + 1);
    char *body = NULL;
    size_t n = 0;
    const char *error = run(cfg, &request, bytes, &body, &n);
    if (!error) {
        Json j;
        json_parse(&j, body, n);
        char *actual_source = json_string(&j, json_get(&j, 0, "source_id")),
             *actual_version = json_string(&j, json_get(&j, 0, "version")),
             *kind = json_string(&j, json_get(&j, 0, "kind"));
        int range = json_get(&j, 0, "range"),
            coverage = json_get(&j, 0, "coverage"),
            provenance = json_get(&j, 0, "provenance"),
            pages = json_get(&j, 0, "pages");
        size_t lo = 0, hi = 0, total = 0, inspected = 0;
        char *status = json_string(&j, json_get(&j, coverage, "status")),
             *reason = json_string(&j, json_get(&j, coverage, "reason")),
             *reader = json_string(&j, json_get(&j, provenance, "reader")),
             *fingerprint = json_string(
                 &j, json_get(&j, provenance, "extractor_fingerprint")),
             *actual_locator =
                 json_string(&j, json_get(&j, provenance, "locator")),
             *evidence = json_string(&j, json_get(&j, 0, "evidence_id"));
        if (!actual_locator || strcmp(actual_locator, locator) ||
            !actual_source || strcmp(actual_source, source) ||
            !actual_version || strcmp(actual_version, version) || !kind ||
            strcmp(kind, "literal") ||
            !json_uint(&j, json_get(&j, range, "start_page"), &lo) ||
            lo != start ||
            !json_uint(&j, json_get(&j, range, "end_page"), &hi) || hi != end ||
            !status || strcmp(status, "partial") || !reason ||
            strcmp(reason,
                   ocr ? "ocr_recognition_uncertain" : "text_layer_only") ||
            !reader ||
            strcmp(reader, ocr ? "pdfium-ocr-v1" : "pdfium-text-v1") ||
            !fingerprint || strlen(fingerprint) != 64 || !evidence ||
            strlen(evidence) != 64 ||
            !json_uint(&j, json_get(&j, coverage, "total_pages"), &total) ||
            total < end ||
            !json_uint(&j, json_get(&j, coverage, "inspected_pages"),
                       &inspected) ||
            inspected != end - start + 1 || pages < 0 ||
            j.tokens[pages].type != JT_ARRAY ||
            j.tokens[pages].count != (int)(end - start + 1))
            error = "native_helper_protocol";
        if (!error) {
            char selector[256], expected[65];
            int length =
                snprintf(selector, sizeof(selector), "%s:%s:%s:%zu:%zu", reader,
                         fingerprint, version, start, end);
            hash_bytes(selector, (size_t)length, expected);
            if (strcmp(expected, evidence))
                error = "native_helper_protocol";
        }
        size_t text_bytes = 0;
        int page = pages + 1;
        for (size_t i = start; !error && i <= end; i++) {
            size_t number = 0;
            char *text = json_string(&j, json_get(&j, page, "text"));
            if (!text || !json_uint(&j, json_get(&j, page, "page"), &number) ||
                number != i || strlen(text) > SOURCES_READ_MAX)
                error = "native_helper_protocol";
            else
                text_bytes += strlen(text);
            free(text);
            if (!error && ocr) {
                int recognition = json_get(&j, page, "recognition"),
                    raster = json_get(&j, recognition, "raster");
                size_t confidence = 0, width = 0, height = 0;
                char *method =
                         json_string(&j, json_get(&j, recognition, "method")),
                     *space = json_string(
                         &j, json_get(&j, raster, "coordinate_space"));
                if (!method || strcmp(method, "tesseract") || !space ||
                    strcmp(space, "rendered_pixels") ||
                    !json_uint(&j, json_get(&j, recognition, "confidence"),
                               &confidence) ||
                    confidence > 100 ||
                    !json_uint(&j, json_get(&j, raster, "width"), &width) ||
                    !json_uint(&j, json_get(&j, raster, "height"), &height) ||
                    !width || !height || width > 4096 || height > 4096 ||
                    width * height > 4194304u || text_bytes > SOURCES_READ_MAX)
                    error = "native_helper_protocol";
                free(method);
                free(space);
            }
            if (!error)
                page = j.tokens[page].next;
        }
        free(actual_source);
        free(actual_version);
        free(actual_locator);
        free(kind);
        free(status);
        free(reason);
        free(reader);
        free(fingerprint);
        free(evidence);
    }
    if (!error && fwrite(body, 1, n, out) != n)
        error = "resource_limit";
    free(body);
    return error;
}
typedef struct {
    int fd;
    SourcesConfig *cfg;
    int failed;
    double check_interval,next_check;
    int stopped;
} WorkerControl;
static const char *worker_rate(void *opaque,const char *origin){
    WorkerControl *worker=opaque;char destination[257]={0},type='P';Control control;
    if(!origin||strlen(origin)>256)return "invalid_fetch_configuration";
    memcpy(destination,origin,strlen(origin));
    if(!child_io(worker->fd,&type,1,1)||!child_io(worker->fd,destination,sizeof(destination),1)||!child_io(worker->fd,&control,sizeof(control),0)){worker->failed=1;return "cancelled";}
    worker->cfg->deadline_monotonic=control.deadline;return control.stopped?"cancelled":NULL;
}
typedef struct {
    int fd;
    atomic_int stopped;
} Watchdog;
static void worker_die(int signal_number) {
    (void)signal_number;
    pid_t self = getpid();
    if (getpgrp() == self)
        kill(-self, SIGKILL);
    kill(self, SIGKILL);
    _exit(125);
}
static void *watch_parent(void *opaque) {
    Watchdog *watch = opaque;
    while (!atomic_load(&watch->stopped)) {
        struct pollfd p = {.fd = watch->fd, .events = POLLIN};
        int ready = poll(&p, 1, 25);
        if (ready < 0 && errno == EINTR)
            continue;
        int gone = ready < 0 ||
                   (ready > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL)));
        if (!gone && ready > 0 && (p.revents & POLLIN)) {
            char byte;
            ssize_t n = recv(watch->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            gone = n == 0 || (n < 0 && errno != EAGAIN &&
                              errno != EWOULDBLOCK && errno != EINTR);
            /* Ordinary control replies belong to the native-call thread. */
            if (n > 0) {
                struct timespec pause = {0, 1000000};
                nanosleep(&pause, NULL);
            }
        }
        if (gone) {
            worker_die(0);
            return NULL;
        }
    }
    return NULL;
}
static int worker_done(Watchdog *watch, pthread_t watcher, int status) {
    atomic_store(&watch->stopped, 1);
    return pthread_join(watcher, NULL) ? 125 : status;
}
static int worker_check(void *opaque) {
    WorkerControl *worker = opaque;
    if(worker->failed)return 1;
    if(worker->check_interval) {
        double current=now();
        if(current<0){worker->failed=1;return 1;}
        if(current<worker->next_check)return worker->stopped;
        worker->next_check=current+worker->check_interval;
    }
    char type = 'C';
    Control reply = {0};
    if (!child_io(worker->fd, &type, 1, 1) ||
        !child_io(worker->fd, &reply, sizeof(reply), 0)) {
        worker->failed = 1;
        return 1;
    }
    worker->cfg->deadline_monotonic = reply.deadline;
    worker->stopped=reply.stopped;
    return worker->stopped;
}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
static void worker_monitor(void *opaque, const char *event,
                           SourcesConfig *cfg) {
    (void)event;
    WorkerControl *worker = opaque;
    char type = 'M';
    Control reply = {0};
    if (!child_io(worker->fd, &type, 1, 1) ||
        !child_io(worker->fd, &reply, sizeof(reply), 0)) {
        worker->failed = 1;
        return;
    }
    cfg->deadline_monotonic = reply.deadline;
}
#endif
int sources_native_worker_main(int argc, char **argv) {
    if (argc != 8 || strcmp(argv[1], "--sources-native-worker"))
        return 125;
    struct rlimit zero = {0, 0};
    if (setrlimit(RLIMIT_CORE, &zero))
        return 125;
    struct rlimit files = {64, 64};
    if (setrlimit(RLIMIT_NOFILE, &files) ||
        fcntl(3, F_SETFD, FD_CLOEXEC) < 0)
        return 125;
#ifdef __linux__
    pid_t parent = getppid();
    struct sigaction death = {.sa_handler = worker_die};
    sigemptyset(&death.sa_mask);
    if (parent == 1 || sigaction(SIGTERM, &death, NULL) ||
        prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)
        return 125;
#endif
    SourcesOcrConfig ocr = {.tesseract_library = argv[2],
                            .leptonica_library = argv[3],
                            .traineddata_file = argv[4],
                            .language = argv[5]};
    SourcesConfig cfg = {.ocr =
                             (*argv[2] || *argv[3] || *argv[4]) ? &ocr : NULL,
                         .pdfium_library = *argv[6] ? argv[6] : NULL};
    Request request = {0};
    if (!child_io(3, &request, sizeof(request), 0) ||
        request.magic != NATIVE_MAGIC || request.kind < 1 ||
#ifdef SAMOSA_SOURCES_FETCH_WORKER
        request.kind != SOURCES_NATIVE_FETCH ||
#elif defined(SAMOSA_SOURCES_CHUTNI_WORKER)
        (request.kind != SOURCES_NATIVE_CHUTNI && request.kind != SOURCES_NATIVE_CHUTNI_PUBLISH && request.kind != SOURCES_NATIVE_CHUTNI_READ && request.kind != SOURCES_NATIVE_CHUTNI_EMBEDDING) ||
#elif defined(SAMOSA_SOURCES_MEDIA_WORKER)
        (request.kind != SOURCES_NATIVE_MEDIA_AUDIO && request.kind != SOURCES_NATIVE_MEDIA_FINGERPRINT &&
         request.kind != SOURCES_NATIVE_MEDIA_FRAME && request.kind != SOURCES_NATIVE_MEDIA_LAST_FRAME && request.kind != SOURCES_NATIVE_MEDIA_PROBE && request.kind != SOURCES_NATIVE_VIDEO_PROBE) ||
#else
        (request.kind > SOURCES_NATIVE_RENDERED && request.kind != SOURCES_NATIVE_INFERENCE) ||
#endif
        !request.length ||
        request.length > (request.kind == SOURCES_NATIVE_CHUTNI_PUBLISH
                              ? SOURCES_INPUT_MAX+1048576u
                              : request.kind == SOURCES_NATIVE_RENDERED
                              ? 4194368u
                              : SOURCES_INPUT_MAX) ||
        !isfinite(request.deadline) || request.deadline <= 0 ||
        !memchr(request.source, 0, sizeof(request.source)) ||
        !memchr(request.version, 0, sizeof(request.version)) ||
        !memchr(request.locator, 0, sizeof(request.locator)))
        return 125;
    /* Select limits only after the host-private header is validated and before
     * reading payload or launching a provider. Temporal model descendants need
     * the declared120s budget and3750MiB package residency plus runtime space. */
    int temporal=request.kind==SOURCES_NATIVE_INFERENCE&&request.start==1;
    struct rlimit cpu = {temporal?120u:32u,temporal?120u:32u};
    if(setrlimit(RLIMIT_CPU,&cpu))return 125;
#if defined(__linux__) && !defined(NATIVE_ASAN)
    rlim_t address_limit=(temporal?8ull:2ull)*1024ull*1024ull*1024ull;
    struct rlimit address_space={address_limit,address_limit};
    if(setrlimit(RLIMIT_AS,&address_space))return 125;
#endif
    unsigned char *bytes = malloc(request.length);
    if (!bytes)
        return 125;
    if (!child_io(3, bytes, request.length, 0)) {
        free(bytes);
        return 125;
    }
    cfg.deadline_monotonic = request.deadline;
    /* Full scans check local deadlines at every interruption point, but do not
     * make several synchronous parent/SQLite round trips per decoded packet.
     * Refresh cancellation at the existing25ms parent-watch cadence. */
    WorkerControl worker = {.fd = 3, .cfg = &cfg,
        .check_interval=(request.kind==SOURCES_NATIVE_MEDIA_PROBE||request.kind==SOURCES_NATIVE_VIDEO_PROBE)?0.025:0};
    cfg.cancelled = worker_check;
    cfg.cancellation = &worker;
    cfg.fetch_rate=worker_rate;cfg.fetch_rate_context=&worker;
    Watchdog watch = {.fd = 3};
    pthread_t watcher;
    if (pthread_create(&watcher, NULL, watch_parent, &watch)) {
        free(bytes);
        return 125;
    }
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if (!strcmp(argv[7], "descendant_stall")) {
        int ready[2];
        if (pipe(ready)) {
            free(bytes);
            return worker_done(&watch, watcher, 125);
        }
        pid_t descendant = fork();
        if (descendant < 0) {
            close(ready[0]);
            close(ready[1]);
            free(bytes);
            return worker_done(&watch, watcher, 125);
        }
        if (!descendant) {
            close(ready[0]);
            char started = 1;
            write(ready[1], &started, 1);
            close(ready[1]);
            for (;;)
                pause();
        }
        close(ready[1]);
        char started = 0;
        ssize_t n;
        do {
            n = read(ready[0], &started, 1);
        } while (n < 0 && errno == EINTR);
        close(ready[0]);
        if (n != 1) {
            free(bytes);
            return worker_done(&watch, watcher, 125);
        }
        char type = 'D';
        int pid = (int)descendant;
        child_io(3, &type, 1, 1);
        child_io(3, &pid, sizeof(pid), 1);
    }
    if (*argv[7]) {
        char type = 'F';
        if (!child_io(3, &type, 1, 1)) {
            free(bytes);
            return worker_done(&watch, watcher, 125);
        }
    }
    if (!strcmp(argv[7], "abort"))
        abort();
    if (!strcmp(argv[7], "stall") || !strcmp(argv[7], "descendant_stall")) {
        for (;;)
            pause();
    }
    if (!strcmp(argv[7], "bad_protocol")) {
        char bad = 'X';
        child_io(3, &bad, 1, 1);
        free(bytes);
        return worker_done(&watch, watcher, 0);
    }
    if (!strcmp(argv[7], "huge_output")) {
        char type = 'R';
        uint32_t huge = (uint32_t)response_limit(request.kind) + 1;
        child_io(3, &type, 1, 1);
        child_io(3, &huge, sizeof(huge), 1);
        free(bytes);
        return worker_done(&watch, watcher, 0);
    }
    if (!strcmp(argv[7], "early_exit")) {
        free(bytes);
        return worker_done(&watch, watcher, 7);
    }
    if (!strcmp(argv[7], "limits")) {
        struct rlimit files, cpu, core;
        int closed = fcntl(100, F_GETFD) < 0 && errno == EBADF;
        if (getrlimit(RLIMIT_NOFILE, &files) || getrlimit(RLIMIT_CPU, &cpu) ||
            getrlimit(RLIMIT_CORE, &core)) {
            free(bytes);
            return worker_done(&watch, watcher, 125);
        }
        char body[256];
        int n = snprintf(
            body, sizeof(body),
            "{\"text\":\"%s:%llu:%llu:%llu\",\"fingerprint\":"
            "\"0000000000000000000000000000000000000000000000000000000000000000"
            "\",\"width\":1,\"height\":1,\"confidence\":0}",
            closed ? "CLOSED" : "LEAKED", (unsigned long long)files.rlim_cur,
            (unsigned long long)cpu.rlim_cur,
            (unsigned long long)core.rlim_cur);
        char type = 'R';
        uint32_t length = (uint32_t)n;
        int ok = child_io(3, &type, 1, 1) &&
                 child_io(3, &length, sizeof(length), 1) &&
                 child_io(3, body, length, 1);
        free(bytes);
        return worker_done(&watch, watcher, ok ? 0 : 125);
    }
    if (!strcmp(argv[7], "memory")) {
        unsigned char *memory = malloc(256u * 1024u * 1024u);
        if (!memory) {
            free(bytes);
            return worker_done(&watch, watcher, 125);
        }
        for (size_t offset = 0; offset < 256u * 1024u * 1024u;
             offset += 1048576) {
            /* Observable page writes keep the fault resident under optimization. */
            volatile unsigned char *resident = memory;
            for (size_t page = offset; page < offset + 1048576; page += 4096)
                resident[page] = 1;
            struct timespec pause = {0, 100000};
            nanosleep(&pause, NULL);
        }
        for (;;)
            pause();
    }
    if (!strcmp(argv[7], "bad_result")) {
        static char body[] =
            "{\"text\":\"INJECTED\",\"fingerprint\":"
            "\"0000000000000000000000000000000000000000000000000000000000000000"
            "\",\"width\":99999,\"height\":1,\"confidence\":101}";
        char type = 'R';
        uint32_t n = sizeof(body) - 1;
        child_io(3, &type, 1, 1);
        child_io(3, &n, sizeof(n), 1);
        child_io(3, body, n, 1);
        free(bytes);
        return worker_done(&watch, watcher, 0);
    }
    if (*argv[7]) {
        free(bytes);
        return worker_done(&watch, watcher, 125);
    }
    ocr.test_event = worker_monitor;
    ocr.test_context = &worker;
#else
    if (*argv[7]) {
        free(bytes);
        return worker_done(&watch, watcher, 125);
    }
#endif
    char *response = NULL;
    size_t n = 0;
    FILE *stream = open_memstream(&response, &n);
    if (!stream) {
        free(bytes);
        return worker_done(&watch, watcher, 125);
    }
    const char *error = NULL;
    if (request.kind == SOURCES_NATIVE_INFERENCE) {
        error=sources_inference_process_worker(&cfg,bytes,request.length,stream);
    } else if (request.kind == SOURCES_NATIVE_CHUTNI_EMBEDDING) {
#ifdef SAMOSA_SOURCES_CHUTNI_WORKER
        error=sources_chutni_embedding_worker(&cfg,bytes,request.length,stream);
#else
        error="chutni_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_CHUTNI_PUBLISH) {
#ifdef SAMOSA_SOURCES_CHUTNI_WORKER
        error=sources_chutni_publish_worker(&cfg,bytes,request.length,stream);
#else
        error="chutni_publish_disabled";
#endif
    } else if (request.kind == SOURCES_NATIVE_CHUTNI_READ) {
#ifdef SAMOSA_SOURCES_CHUTNI_WORKER
        error=sources_chutni_read_worker(&cfg,bytes,request.length,stream);
#else
        error="chutni_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_CHUTNI) {
#ifdef SAMOSA_SOURCES_CHUTNI_WORKER
        error=sources_chutni_worker(&cfg,bytes,request.length,stream);
#else
        error="chutni_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_FETCH) {
#ifdef SAMOSA_SOURCES_FETCH_WORKER
        error = sources_fetch_worker(&cfg, bytes, request.length, stream);
#else
        error = "fetch_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_MEDIA_FINGERPRINT) {
#ifdef SAMOSA_SOURCES_MEDIA_WORKER
        if (request.length != 1 || bytes[0] || request.start || request.end)
            error="invalid_request";
        else {
            SourcesMediaArtifacts artifacts;
            char fingerprint[65];
            error=sources_media_artifacts_open(&cfg,&artifacts,fingerprint);
            if (!error) error=sources_media_artifacts_check(&cfg,&artifacts);
            sources_media_artifacts_close(&artifacts);
            if (!error) fprintf(stream,"{\"fingerprint\":\"%s\"}\n",fingerprint);
        }
#else
        error="media_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_MEDIA_PROBE) {
#ifdef SAMOSA_SOURCES_MEDIA_WORKER
        error=request.start||request.end?"invalid_request":
            sources_media_probe_response(&cfg,bytes,request.length,stream);
#else
        error="media_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_VIDEO_PROBE) {
#ifdef SAMOSA_SOURCES_MEDIA_WORKER
        error=request.start||request.end?"invalid_request":
            sources_video_probe_response(&cfg,bytes,request.length,stream);
#else
        error="media_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_MEDIA_AUDIO) {
#ifdef SAMOSA_SOURCES_MEDIA_WORKER
        error = sources_media_audio_response(&cfg, bytes, request.length,
                                             request.start, request.end, stream);
#else
        error = "media_unavailable";
#endif
    } else if(request.kind==SOURCES_NATIVE_MEDIA_FRAME||request.kind==SOURCES_NATIVE_MEDIA_LAST_FRAME) {
#ifdef SAMOSA_SOURCES_MEDIA_WORKER
        error=request.kind==SOURCES_NATIVE_MEDIA_LAST_FRAME?
            sources_media_last_frame_response(&cfg,bytes,request.length,request.start,request.end,stream):
            sources_media_frame_response(&cfg,bytes,request.length,request.start,request.end,stream);
#else
        error="media_unavailable";
#endif
    } else if (request.kind == SOURCES_NATIVE_IMAGE ||
        request.kind == SOURCES_NATIVE_RENDERED) {
        SourcesOcrResult result = {0};
        error = request.kind == SOURCES_NATIVE_IMAGE
                    ? sources_ocr_extract(&cfg, bytes, request.length, &result)
                    : sources_ocr_extract_rendered(&cfg, bytes, request.length,
                                                   &result);
        if (!error) {
            fputs("{\"text\":", stream);
            json_quote(stream, result.text, result.text_bytes);
            fprintf(stream,
                    ",\"fingerprint\":\"%s\",\"width\":%u,\"height\":%u,"
                    "\"confidence\":%d}\n",
                    result.fingerprint, result.width, result.height,
                    result.confidence);
        }
        sources_ocr_free(&result);
    } else
        error =
            request.kind == SOURCES_NATIVE_PDF_OCR
                ? pdf_ocr_extract(&cfg, bytes, request.length, request.source,
                                  request.version, request.start, request.end,
                                  request.locator, stream)
                : pdf_extract(&cfg, cfg.pdfium_library, bytes, request.length,
                              request.source, request.version, request.start,
                              request.end, request.locator, stream);
    free(bytes);
    if (fclose(stream) || worker.failed) {
        free(response);
        return worker_done(&watch, watcher, 125);
    }
    if (error) {
        free(response);
        response = NULL;
        n = 0;
        stream = open_memstream(&response, &n);
        if (!stream)
            return worker_done(&watch, watcher, 125);
        fputs("{\"error\":{\"code\":", stream);
        json_quote(stream, error, strlen(error));
        fputs("}}\n", stream);
        if (fclose(stream)) {
            free(response);
            return worker_done(&watch, watcher, 125);
        }
    }
    if (!response || !n || n > response_limit(request.kind)) {
        free(response);
        return worker_done(&watch, watcher, 125);
    }
    if (worker_done(&watch, watcher, 0)) {
        free(response);
        return 125;
    }
    char type = 'R';
    uint32_t length = (uint32_t)n;
    int ok = child_io(3, &type, 1, 1) &&
             child_io(3, &length, sizeof(length), 1) &&
             child_io(3, response, n, 1);
    free(response);
    close(3);
    return ok ? 0 : 125;
}
