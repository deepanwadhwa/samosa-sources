#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "control.h"
#include "json.h"
#include "ocr.h"
#include "native.h"
#include "sha256.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* Opaque public native C ABI types. No provider or legacy private
 * headers/state. Signatures follow Tesseract capi.h and Leptonica allheaders.h.
 */
typedef struct TessBaseAPI TessBaseAPI;
typedef struct ETEXT_DESC ETEXT_DESC;
typedef struct Pix Pix;
typedef struct {
    const char *path;
    int fd;
    struct stat before;
    char digest[65];
} Artifact;
static int stable(const struct stat *a, const struct stat *b) {
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino ||
        a->st_size != b->st_size)
        return 0;
#ifdef __APPLE__
    return a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
           a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}
static int unchanged(Artifact *a) {
    struct stat opened, named;
    return !fstat(a->fd, &opened) && !lstat(a->path, &named) &&
           stable(&a->before, &opened) && stable(&a->before, &named);
}
static const char *artifact_open(const SourcesConfig *cfg, const char *path,
                                 size_t limit, Artifact *a) {
    a->fd = -1;
    a->path = path;
    if (!path || !*path)
        return "ocr_unavailable";
    a->fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (a->fd < 0)
        return "ocr_artifact_invalid";
    if (fstat(a->fd, &a->before) || !S_ISREG(a->before.st_mode) ||
        a->before.st_size < 1 || (uint64_t)a->before.st_size > limit)
        return "ocr_artifact_invalid";
    Sha256 hash;
    sha256_init(&hash);
    unsigned char buffer[65536], sum[32];
    size_t total = 0;
    for (;;) {
        const char *error = sources_interrupted(cfg);
        if (error)
            return error;
        ssize_t n = read(a->fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return "ocr_artifact_invalid";
        if (!n)
            break;
        if ((size_t)n > limit - total)
            return "ocr_artifact_invalid";
        total += (size_t)n;
        sha256_update(&hash, buffer, (size_t)n);
    }
    if (total != (size_t)a->before.st_size || !unchanged(a))
        return "ocr_artifact_changed";
    sha256_final(&hash, sum);
    sha256_hex(sum, a->digest);
    return NULL;
}
static void close_artifacts(Artifact a[3]) {
    for (unsigned i = 0; i < 3; i++)
        if (a[i].fd >= 0)
            close(a[i].fd);
}
static const char *artifacts(const SourcesConfig *cfg, Artifact a[3],
                             char fingerprint[65]) {
    for (unsigned i = 0; i < 3; i++)
        a[i].fd = -1;
    if (!cfg || !cfg->ocr)
        return "ocr_unavailable";
    const SourcesOcrConfig *ocr = cfg->ocr;
    const char *language = ocr->language ? ocr->language : "eng";
    size_t n = strlen(language);
    if (!n || n > 32)
        return "invalid_configuration";
    for (size_t i = 0; i < n; i++)
        if (!((language[i] >= 'a' && language[i] <= 'z') ||
              (language[i] >= '0' && language[i] <= '9') || language[i] == '_'))
            return "invalid_configuration";
    const char *paths[] = {ocr->tesseract_library, ocr->leptonica_library,
                           ocr->traineddata_file};
    const char *error = NULL;
    for (unsigned i = 0; i < 3 && !error; i++)
        error = artifact_open(cfg, paths[i], (i == 2 ? 32u : 128u) * 1048576u,
                              &a[i]);
    if (error)
        return error;
    static const char contract[] = "ocr-tesseract-v1:4Mi-pixels:4096-axis:"
                                   "64KiB-utf8:scoped-monitor:memory-model";
    Sha256 hash;
    unsigned char sum[32];
    sha256_init(&hash);
    sha256_update(&hash, contract, sizeof(contract));
    sha256_update(&hash, language, n + 1);
    for (unsigned i = 0; i < 3; i++)
        sha256_update(&hash, a[i].digest, 64);
    sha256_final(&hash, sum);
    sha256_hex(sum, fingerprint);
    return NULL;
}
const char *sources_ocr_fingerprint(const SourcesConfig *cfg, char out[65]) {
    Artifact a[3];
    const char *error = artifacts(cfg, a, out);
    close_artifacts(a);
    return error;
}
typedef struct {
    void *tess, *lept;
    TessBaseAPI *(*create)(void);
    void (*destroy)(TessBaseAPI *);
    int (*init)(TessBaseAPI *, const char *, int, const char *, int, char **,
                int, char **, char **, size_t, int);
    void (*set_image)(TessBaseAPI *, Pix *);
    void (*resolution)(TessBaseAPI *, int);
    int (*recognize)(TessBaseAPI *, ETEXT_DESC *);
    char *(*text)(TessBaseAPI *);
    int (*confidence)(TessBaseAPI *);
    void (*free_text)(const char *);
    ETEXT_DESC *(*monitor)(void);
    void (*free_monitor)(ETEXT_DESC *);
    void (*set_cancel)(ETEXT_DESC *, bool (*)(void *, int));
    void (*set_context)(ETEXT_DESC *, void *);
    void (*set_deadline)(ETEXT_DESC *, int);
    int (*header)(const unsigned char *, size_t, int *, int *, int *, int *,
                  int *, int *);
    Pix *(*decode)(const unsigned char *, size_t);
    void (*free_pix)(Pix **);
    int (*dimensions)(const Pix *, int *, int *, int *);
} Runtime;
static int symbol(void *lib, const char *name, void *field, size_t size) {
    void *p = dlsym(lib, name);
    if (!p || size != sizeof(p))
        return 0;
    memcpy(field, &p, size);
    return 1;
}
static void unload(Runtime *r) {
    if (r->tess)
        dlclose(r->tess);
    if (r->lept)
        dlclose(r->lept);
}
#define LOAD(lib, field, name)                                                 \
    do {                                                                       \
        if (!symbol(r->lib, name, &r->field, sizeof(r->field)))                \
            return "ocr_adapter_incompatible";                                 \
    } while (0)
static const char *load(const SourcesOcrConfig *cfg, Runtime *r) {
    r->lept = dlopen(cfg->leptonica_library, RTLD_NOW | RTLD_LOCAL);
    if (!r->lept)
        return "ocr_unavailable";
    r->tess = dlopen(cfg->tesseract_library, RTLD_NOW | RTLD_LOCAL);
    if (!r->tess)
        return "ocr_unavailable";
    LOAD(tess, create, "TessBaseAPICreate");
    LOAD(tess, destroy, "TessBaseAPIDelete");
    LOAD(tess, init, "TessBaseAPIInit5");
    LOAD(tess, set_image, "TessBaseAPISetImage2");
    LOAD(tess, resolution, "TessBaseAPISetSourceResolution");
    LOAD(tess, recognize, "TessBaseAPIRecognize");
    LOAD(tess, text, "TessBaseAPIGetUTF8Text");
    LOAD(tess, confidence, "TessBaseAPIMeanTextConf");
    LOAD(tess, free_text, "TessDeleteText");
    LOAD(tess, monitor, "TessMonitorCreate");
    LOAD(tess, free_monitor, "TessMonitorDelete");
    LOAD(tess, set_cancel, "TessMonitorSetCancelFunc");
    LOAD(tess, set_context, "TessMonitorSetCancelThis");
    LOAD(tess, set_deadline, "TessMonitorSetDeadlineMSecs");
    LOAD(lept, header, "pixReadHeaderMem");
    LOAD(lept, decode, "pixReadMem");
    LOAD(lept, free_pix, "pixDestroy");
    LOAD(lept, dimensions, "pixGetDimensions");
    return NULL;
}
#undef LOAD
static double now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return -1;
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static bool cancel(void *context, int words) {
    (void)words;
    SourcesConfig *cfg = context;
#ifdef SAMOSA_SOURCES_TEST_FAULTS
    if (cfg->ocr->test_event)
        cfg->ocr->test_event(cfg->ocr->test_context, "monitor_poll",cfg);
#endif
    return sources_interrupted(cfg) != NULL;
}
void sources_ocr_free(SourcesOcrResult *result) {
    if (result) {
        free(result->text);
        memset(result, 0, sizeof(*result));
    }
}
static const char *extract(const SourcesConfig *cfg, const void *bytes,
                           size_t length, SourcesOcrResult *result,
                           size_t input_limit) {
    if (!result)
        return "invalid_request";
    memset(result, 0, sizeof(*result));
    if (!bytes || !length || length > input_limit)
        return "source_too_large";
    SourcesConfig work = cfg ? *cfg : (SourcesConfig){0};
    double current = now();
    if (current < 0)
        return "state_unavailable";
    if (!work.deadline_monotonic || work.deadline_monotonic > current + 30)
        work.deadline_monotonic = current + 30;
    Artifact a[3];
    Runtime r = {0};
    TessBaseAPI *api = NULL;
    ETEXT_DESC *monitor = NULL;
    Pix *pix = NULL;
    char *recognized = NULL, *model = NULL;
    const char *error = artifacts(&work, a, result->fingerprint);
    if (!error)
        error = load(work.ocr, &r);
    int width = 0, height = 0, format = 0, bits = 0, samples = 0, colormap = 0;
    if (!error && (r.header(bytes, length, &format, &width, &height, &bits,
                            &samples, &colormap) ||
                   width < 1 || height < 1))
        error = "invalid_image";
    if (!error && (width > 4096 || height > 4096 ||
                   (uint64_t)width * (unsigned)height > 4194304u || bits < 1 ||
                   bits > 16 || samples < 1 || samples > 4))
        error = "image_dimension_limit";
    if (!error) {
        pix = r.decode(bytes, length);
        if (!pix)
            error = "image_decode_failed";
    }
    if (!error) {
        int decoded_width = 0, decoded_height = 0, depth = 0;
        if (r.dimensions(pix, &decoded_width, &decoded_height, &depth) ||
            decoded_width != width || decoded_height != height || depth < 1 ||
            depth > 32)
            error = "image_decode_failed";
    }
    if (!error)
        error = sources_interrupted(&work);
    if (!error) {
        size_t size = (size_t)a[2].before.st_size;
        model = malloc(size);
        if (!model)
            error = "resource_limit";
        else {
            size_t offset = 0;
            while (offset < size) {
                ssize_t n = pread(a[2].fd, model + offset, size - offset,
                                  (off_t)offset);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0) {
                    error = "ocr_artifact_changed";
                    break;
                }
                offset += (size_t)n;
            }
            char digest[65];
            if (!error) {
                hash_bytes(model, size, digest);
                if (strcmp(digest, a[2].digest))
                    error = "ocr_artifact_changed";
            }
        }
    }
    if (!error) {
        api = r.create();
        monitor = r.monitor();
        if (!api || !monitor)
            error = "resource_limit";
    }
    if (!error && r.init(api, model, (int)a[2].before.st_size,
                         work.ocr->language ? work.ocr->language : "eng", 3,
                         NULL, 0, NULL, NULL, 0, 0))
        error = "ocr_model_invalid";
    if (!error) {
        r.set_image(api, pix);
        r.resolution(api, 300);
        r.set_cancel(monitor, cancel);
        r.set_context(monitor, &work);
        double remaining = work.deadline_monotonic - now();
        if (remaining <= 0)
            error = "deadline_exceeded";
        else {
            int ms = (int)(remaining * 1000);
            if (ms < 1)
                ms = 1;
            r.set_deadline(monitor, ms);
            int rc = r.recognize(api, monitor);
            error = sources_interrupted(&work);
            if (!error && rc)
                error = "ocr_recognition_failed";
        }
    }
    if (!error) {
        recognized = r.text(api);
        if (!recognized)
            error = "ocr_recognition_failed";
        else {
            size_t n = strnlen(recognized, SOURCES_READ_MAX + 1);
            if (n > SOURCES_READ_MAX)
                error = "ocr_output_limit";
            else if (!utf8_valid((const unsigned char *)recognized, n))
                error = "ocr_invalid_utf8";
            else {
                result->text = malloc(n + 1);
                if (!result->text)
                    error = "resource_limit";
                else {
                    memcpy(result->text, recognized, n + 1);
                    result->text_bytes = n;
                    result->width = (unsigned)width;
                    result->height = (unsigned)height;
                    result->confidence = r.confidence(api);
                    if (result->confidence < 0 || result->confidence > 100)
                        error = "ocr_invalid_confidence";
                }
            }
        }
    }
    if (!error)
        error = sources_interrupted(&work);
    for (unsigned i = 0; i < 3 && !error; i++)
        if (!unchanged(&a[i]))
            error = "ocr_artifact_changed";
    if (recognized)
        r.free_text(recognized);
    if (monitor)
        r.free_monitor(monitor);
    if (api)
        r.destroy(api);
    if (pix)
        r.free_pix(&pix);
    free(model);
    unload(&r);
    close_artifacts(a);
    if (error)
        sources_ocr_free(result);
    return error;
}
const char *sources_ocr_extract(const SourcesConfig *cfg, const void *bytes,
                                size_t length, SourcesOcrResult *result) {
    if(cfg&&cfg->native_helper)return sources_native_ocr(cfg,bytes,length,0,result);
    return extract(cfg, bytes, length, result, SOURCES_INPUT_MAX);
}
const char *sources_ocr_extract_rendered(const SourcesConfig *cfg,
                                         const void *bytes, size_t length,
                                         SourcesOcrResult *result) {
    if(cfg&&cfg->native_helper)return sources_native_ocr(cfg,bytes,length,1,result);
    return extract(cfg, bytes, length, result, 4194304u + 64u);
}
