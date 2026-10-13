#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "control.h"
#include "json.h"
#include "ocr.h"
#include "native.h"
#include "pdf.h"
#include "sha256.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <fpdf_text.h>
#include <fpdfview.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define PDF_ARTIFACT_MAX (128u * 1048576u)
static int stable_artifact(const struct stat *a, const struct stat *b) {
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
static const char *artifact(const char *path, int *fd, struct stat *before,
                            char digest[65]) {
    *fd = -1;
    if (!path || !*path)
        return "pdf_unavailable";
    *fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (*fd < 0)
        return "pdf_unavailable";
    if (fstat(*fd, before) || !S_ISREG(before->st_mode) ||
        before->st_size < 1 || (uint64_t)before->st_size > PDF_ARTIFACT_MAX)
        return "pdf_adapter_invalid";
    Sha256 hash;
    sha256_init(&hash);
    unsigned char buffer[65536], sum[32];
    size_t total = 0;
    for (;;) {
        ssize_t got = read(*fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0)
            return "pdf_adapter_invalid";
        if (!got)
            break;
        if (total > PDF_ARTIFACT_MAX - (size_t)got)
            return "pdf_adapter_invalid";
        sha256_update(&hash, buffer, (size_t)got);
        total += (size_t)got;
    }
    struct stat after;
    if (total != (size_t)before->st_size || fstat(*fd, &after) ||
        !stable_artifact(before, &after))
        return "pdf_adapter_changed";
    sha256_final(&hash, sum);
    sha256_hex(sum, digest);
    return NULL;
}
const char *pdf_fingerprint(const char *path, char digest[65]) {
    int fd = -1;
    struct stat before;
    const char *error = artifact(path, &fd, &before, digest);
    if (fd >= 0)
        close(fd);
    return error;
}
static int artifact_unchanged(const char *path, int fd,
                              const struct stat *before) {
    struct stat opened, named;
    return !fstat(fd, &opened) && !lstat(path, &named) &&
           stable_artifact(before, &opened) && stable_artifact(before, &named);
}

typedef struct {
    void *handle;
    void(FPDF_CALLCONV *init)(void);
    void(FPDF_CALLCONV *destroy)(void);
    FPDF_DOCUMENT(FPDF_CALLCONV *open)(const void *, size_t, FPDF_BYTESTRING);
    void(FPDF_CALLCONV *close)(FPDF_DOCUMENT);
    int(FPDF_CALLCONV *pages)(FPDF_DOCUMENT);
    FPDF_PAGE(FPDF_CALLCONV *load_page)(FPDF_DOCUMENT, int);
    void(FPDF_CALLCONV *close_page)(FPDF_PAGE);
    FPDF_TEXTPAGE(FPDF_CALLCONV *text_page)(FPDF_PAGE);
    void(FPDF_CALLCONV *close_text)(FPDF_TEXTPAGE);
    int(FPDF_CALLCONV *chars)(FPDF_TEXTPAGE);
    unsigned int(FPDF_CALLCONV *unicode)(FPDF_TEXTPAGE, int);
    unsigned long(FPDF_CALLCONV *error)(void);
    float(FPDF_CALLCONV *width)(FPDF_PAGE);
    float(FPDF_CALLCONV *height)(FPDF_PAGE);
    FPDF_BITMAP(FPDF_CALLCONV *bitmap)(int, int, int, void *, int);
    void(FPDF_CALLCONV *destroy_bitmap)(FPDF_BITMAP);
    void(FPDF_CALLCONV *render)(FPDF_BITMAP, FPDF_PAGE, int, int, int, int, int,
                                int);
} Pdf;
static int symbol(void *lib, const char *name, void *dest, size_t length) {
    void *sym = dlsym(lib, name);
    if (!sym || length != sizeof(sym))
        return 0;
    memcpy(dest, &sym, length);
    return 1;
}
#define LOAD(field, name)                                                      \
    do {                                                                       \
        if (!symbol(pdf.handle, name, &pdf.field, sizeof(pdf.field))) {        \
            dlclose(pdf.handle);                                               \
            return "pdf_adapter_incompatible";                                 \
        }                                                                      \
    } while (0)
static const char *load(Pdf *p, const char *path) {
    if (!path || !*path)
        return "pdf_unavailable";
    Pdf pdf = {0};
    pdf.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!pdf.handle)
        return "pdf_unavailable";
    LOAD(init, "FPDF_InitLibrary");
    LOAD(destroy, "FPDF_DestroyLibrary");
    LOAD(open, "FPDF_LoadMemDocument64");
    LOAD(close, "FPDF_CloseDocument");
    LOAD(pages, "FPDF_GetPageCount");
    LOAD(load_page, "FPDF_LoadPage");
    LOAD(close_page, "FPDF_ClosePage");
    LOAD(text_page, "FPDFText_LoadPage");
    LOAD(close_text, "FPDFText_ClosePage");
    LOAD(chars, "FPDFText_CountChars");
    LOAD(unicode, "FPDFText_GetUnicode");
    LOAD(error, "FPDF_GetLastError");
    *p = pdf;
    return NULL;
}
#undef LOAD
const char *pdf_ocr_fingerprint(const SourcesConfig *cfg, char out[65]) {
    char pdf[65], ocr[65];
    const char *error = pdf_fingerprint(cfg ? cfg->pdfium_library : NULL, pdf);
    if (!error)
        error = sources_ocr_fingerprint(cfg, ocr);
    if (error)
        return error;
    static const char contract[] =
        "pdfium-ocr-v1:150dpi:4096-axis:4Mi-pixels:4-pages:64KiB-total:gray-"
        "raster:absolute-pages";
    Sha256 hash;
    unsigned char sum[32];
    sha256_init(&hash);
    sha256_update(&hash, contract, sizeof(contract));
    sha256_update(&hash, pdf, 64);
    sha256_update(&hash, ocr, 64);
    sha256_final(&hash, sum);
    sha256_hex(sum, out);
    return NULL;
}
const char *pdf_ocr_extract(const SourcesConfig *cfg, const void *bytes,
                            size_t length, const char *source,
                            const char *version, size_t start, size_t end,
                            const char *locator, FILE *out) {
    if(cfg&&cfg->native_helper)return sources_native_pdf(cfg,bytes,length,source,version,start,end,locator,out,1);
    if (!cfg || !bytes || !length || length > SOURCES_INPUT_MAX)
        return "invalid_request";
    if (start < 1 || end < start || end - start >= 4)
        return "invalid_range";
    SourcesConfig work = *cfg;
    struct timespec clock;
    if (clock_gettime(CLOCK_MONOTONIC, &clock))
        return "state_unavailable";
    double deadline = (double)clock.tv_sec + (double)clock.tv_nsec / 1e9 + 30;
    if (!work.deadline_monotonic || work.deadline_monotonic > deadline)
        work.deadline_monotonic = deadline;
    const char *error = sources_interrupted(&work);
    char fingerprint[65], ocr_fingerprint[65], pdf_digest[65];
    if (!error)
        error = pdf_ocr_fingerprint(&work, fingerprint);
    if (!error)
        error = sources_ocr_fingerprint(&work, ocr_fingerprint);
    int fd = -1;
    struct stat before;
    Pdf pdf = {0};
    FPDF_DOCUMENT doc = NULL;
    int initialized = 0, total = 0;
    SourcesOcrResult pages[4] = {{0}};
    size_t text_bytes = 0;
    if (!error)
        error = artifact(work.pdfium_library, &fd, &before, pdf_digest);
    if (!error)
        error = load(&pdf, work.pdfium_library);
#define RENDER_SYMBOL(field, name)                                             \
    do {                                                                       \
        if (!error &&                                                          \
            !symbol(pdf.handle, name, &pdf.field, sizeof(pdf.field)))          \
            error = "pdf_adapter_incompatible";                                \
    } while (0)
    RENDER_SYMBOL(width, "FPDF_GetPageWidthF");
    RENDER_SYMBOL(height, "FPDF_GetPageHeightF");
    RENDER_SYMBOL(bitmap, "FPDFBitmap_CreateEx");
    RENDER_SYMBOL(destroy_bitmap, "FPDFBitmap_Destroy");
    RENDER_SYMBOL(render, "FPDF_RenderPageBitmap");
#undef RENDER_SYMBOL
    if (!error && !artifact_unchanged(work.pdfium_library, fd, &before))
        error = "pdf_adapter_changed";
    if (!error) {
        pdf.init();
        initialized = 1;
        doc = pdf.open(bytes, length, NULL);
        if (!doc)
            error = pdf.error() == FPDF_ERR_PASSWORD ? "pdf_encrypted"
                                                     : "pdf_malformed";
    }
    if (!error) {
        total = pdf.pages(doc);
        if (total < 1 || end > (size_t)total)
            error = "invalid_range";
    }
    for (size_t number = start; !error && number <= end; number++) {
        error = sources_interrupted(&work);
        if (error)
            break;
        FPDF_PAGE page = pdf.load_page(doc, (int)number - 1);
        if (!page) {
            error = "pdf_page_failed";
            break;
        }
        double w = (double)pdf.width(page) * 150.0 / 72.0,
               h = (double)pdf.height(page) * 150.0 / 72.0;
        unsigned width = 0, height = 0;
        unsigned char *raster = NULL;
        FPDF_BITMAP bitmap = NULL;
        if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0 || w > 4096 ||
            h > 4096)
            error = "image_dimension_limit";
        else {
            width = (unsigned)w;
            if ((double)width < w)
                width++;
            height = (unsigned)h;
            if ((double)height < h)
                height++;
            if ((uint64_t)width * height > 4194304u)
                error = "image_dimension_limit";
        }
        size_t pixels = (size_t)width * height;
        char header[64];
        int header_bytes = 0;
        if (!error) {
            header_bytes = snprintf(header, sizeof(header), "P5\n%u %u\n255\n",
                                    width, height);
            if (header_bytes < 0 || (size_t)header_bytes >= sizeof(header))
                error = "resource_limit";
        }
        if (!error) {
            raster = malloc(pixels + (size_t)header_bytes);
            if (!raster)
                error = "resource_limit";
            else {
                memcpy(raster, header, (size_t)header_bytes);
                memset(raster + header_bytes, 255, pixels);
                bitmap = pdf.bitmap((int)width, (int)height, FPDFBitmap_Gray,
                                    raster + header_bytes, (int)width);
                if (!bitmap)
                    error = "pdf_render_failed";
            }
        }
        if (!error) {
            pdf.render(bitmap, page, 0, 0, (int)width, (int)height, 0, 0);
            error = sources_interrupted(&work);
        }
        if (!error)
            error = sources_ocr_extract_rendered(&work, raster,
                                                 pixels + (size_t)header_bytes,
                                                 &pages[number - start]);
        if (!error) {
            SourcesOcrResult *recognized = &pages[number - start];
            if (strcmp(recognized->fingerprint, ocr_fingerprint))
                error = "ocr_artifact_changed";
            else if (recognized->text_bytes > SOURCES_READ_MAX - text_bytes)
                error = "ocr_output_limit";
            else
                text_bytes += recognized->text_bytes;
        }
        if (bitmap)
            pdf.destroy_bitmap(bitmap);
        free(raster);
        pdf.close_page(page);
    }
    if (!error)
        error = sources_interrupted(&work);
    if (!error && !artifact_unchanged(work.pdfium_library, fd, &before))
        error = "pdf_adapter_changed";
    if (!error) {
        char after[65];
        error = pdf_ocr_fingerprint(&work, after);
        if (!error && strcmp(after, fingerprint))
            error = "ocr_artifact_changed";
    }
    if (!error) {
        char selector[256], evidence[65];
        int n =
            snprintf(selector, sizeof(selector), "pdfium-ocr-v1:%s:%s:%zu:%zu",
                     fingerprint, version, start, end);
        hash_bytes(selector, (size_t)n, evidence);
        fprintf(out,
                "{\"contract_version\":1,\"evidence_id\":\"%s\",\"source_id\":"
                "\"%s\",\"version\":\"%s\",\"kind\":\"literal\",\"range\":{"
                "\"start_page\":%zu,\"end_page\":%zu},\"coverage\":{\"status\":"
                "\"partial\",\"reason\":\"ocr_recognition_uncertain\",\"total_"
                "pages\":%d,\"inspected_pages\":%zu},\"pages\":[",
                evidence, source, version, start, end, total, end - start + 1);
        for (size_t i = 0; i <= end - start; i++) {
            if (i)
                fputc(',', out);
            fprintf(out, "{\"page\":%zu,\"text\":", start + i);
            json_quote(out, pages[i].text, pages[i].text_bytes);
            fprintf(out,
                    ",\"recognition\":{\"method\":\"tesseract\",\"confidence\":"
                    "%d,\"raster\":{\"width\":%u,\"height\":%u,\"coordinate_"
                    "space\":\"rendered_pixels\"}}}",
                    pages[i].confidence, pages[i].width, pages[i].height);
        }
        fprintf(out,
                "],\"provenance\":{\"reader\":\"pdfium-ocr-v1\",\"extractor_"
                "fingerprint\":\"%s\",\"render_dpi\":150,\"locator\":",
                fingerprint);
        json_quote(out, locator, strlen(locator));
        fputs("}}\n", out);
    }
    for (unsigned i = 0; i < 4; i++)
        sources_ocr_free(&pages[i]);
    if (doc)
        pdf.close(doc);
    if (initialized)
        pdf.destroy();
    if (pdf.handle)
        dlclose(pdf.handle);
    if (fd >= 0)
        close(fd);
    return error;
}
static size_t encode(unsigned cp, unsigned char out[4]) {
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0;
    if (cp < 128) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 2048) {
        out[0] = (unsigned char)(192 | (cp >> 6));
        out[1] = (unsigned char)(128 | (cp & 63));
        return 2;
    }
    if (cp < 65536) {
        out[0] = (unsigned char)(224 | (cp >> 12));
        out[1] = (unsigned char)(128 | ((cp >> 6) & 63));
        out[2] = (unsigned char)(128 | (cp & 63));
        return 3;
    }
    out[0] = (unsigned char)(240 | (cp >> 18));
    out[1] = (unsigned char)(128 | ((cp >> 12) & 63));
    out[2] = (unsigned char)(128 | ((cp >> 6) & 63));
    out[3] = (unsigned char)(128 | (cp & 63));
    return 4;
}
const char *pdf_extract(const SourcesConfig *cfg, const char *library,
                        const void *bytes, size_t length, const char *source,
                        const char *version, size_t start, size_t end,
                        const char *locator, FILE *out) {
    if(cfg&&cfg->native_helper){SourcesConfig work=*cfg;work.pdfium_library=library;return sources_native_pdf(&work,bytes,length,source,version,start,end,locator,out,0);}
    const char *interruption = sources_interrupted(cfg);
    if (interruption)
        return interruption;
    if (start < 1 || end < start || end - start >= 4)
        return "invalid_range";
    int artifact_fd = -1;
    struct stat artifact_stat;
    char fingerprint[65];
    const char *error =
        artifact(library, &artifact_fd, &artifact_stat, fingerprint);
    if (error) {
        if (artifact_fd >= 0)
            close(artifact_fd);
        return error;
    }
    Pdf pdf = {0};
    error = load(&pdf, library);
    if (error) {
        close(artifact_fd);
        return error;
    }
    if (!artifact_unchanged(library, artifact_fd, &artifact_stat)) {
        dlclose(pdf.handle);
        close(artifact_fd);
        return "pdf_adapter_changed";
    }
    pdf.init();
    FPDF_DOCUMENT doc = pdf.open(bytes, length, NULL);
    char *texts[4] = {0};
    size_t sizes[4] = {0};
    int total = 0;
    if (!doc)
        error = pdf.error() == FPDF_ERR_PASSWORD ? "pdf_encrypted"
                                                 : "pdf_malformed";
    if (!error) {
        total = pdf.pages(doc);
        if (total < 1 || end > (size_t)total)
            error = "invalid_range";
    }
    for (size_t number = start; !error && number <= end; number++) {
        error = sources_interrupted(cfg);
        if (error)
            break;
        size_t index = number - start;
        FPDF_PAGE page = pdf.load_page(doc, (int)number - 1);
        if (!page) {
            error = "pdf_page_failed";
            break;
        }
        error = sources_interrupted(cfg);
        if (error) {
            pdf.close_page(page);
            break;
        }
        FPDF_TEXTPAGE text = pdf.text_page(page);
        if (!text) {
            pdf.close_page(page);
            error = "pdf_text_failed";
            break;
        }
        int chars = pdf.chars(text);
        char *buffer = malloc(65537);
        if (!buffer)
            error = "resource_limit";
        else if (chars < 0)
            error = "pdf_text_failed";
        else if (chars > 65536)
            error = "extraction_too_large";
        for (int i = 0; !error && i < chars; i++) {
            if (i % 256 == 0) {
                error = sources_interrupted(cfg);
                if (error)
                    break;
            }
            unsigned cp = pdf.unicode(text, i);
            unsigned char encoded[4];
            size_t n = encode(cp, encoded);
            if (!n) {
                error = "pdf_text_unreadable";
                break;
            }
            if (sizes[index] + n > 65536) {
                error = "extraction_too_large";
                break;
            }
            memcpy(buffer + sizes[index], encoded, n);
            sizes[index] += n;
        }
        texts[index] = buffer;
        pdf.close_text(text);
        pdf.close_page(page);
    }
    if (!error && !artifact_unchanged(library, artifact_fd, &artifact_stat))
        error = "pdf_adapter_changed";
    if (!error) {
        char selector[256], evidence[65];
        int n =
            snprintf(selector, sizeof(selector), "pdfium-text-v1:%s:%s:%zu:%zu",
                     fingerprint, version, start, end);
        hash_bytes(selector, (size_t)n, evidence);
        fprintf(out,
                "{\"contract_version\":1,\"evidence_id\":\"%s\",\"source_id\":"
                "\"%s\",\"version\":\"%s\",\"kind\":\"literal\",\"range\":{"
                "\"start_page\":%zu,\"end_page\":%zu},\"coverage\":{\"status\":"
                "\"partial\",\"reason\":\"text_layer_only\",\"total_pages\":%d,"
                "\"inspected_pages\":%zu},\"pages\":[",
                evidence, source, version, start, end, total, end - start + 1);
        for (size_t i = 0; i <= end - start; i++) {
            if (i) {
                fputc(',', out);
            }
            fprintf(out, "{\"page\":%zu,\"text\":", start + i);
            json_quote(out, texts[i], sizes[i]);
            fprintf(out, ",\"text_status\":\"%s\"}",
                    sizes[i] ? "extracted" : "empty_text_layer");
        }
        fprintf(out,
                "],\"provenance\":{\"reader\":\"pdfium-text-v1\",\"extractor_"
                "fingerprint\":\"%s\",\"locator\":",
                fingerprint);
        json_quote(out, locator, strlen(locator));
        fputs("}}\n", out);
    }
    for (int i = 0; i < 4; i++)
        free(texts[i]);
    if (doc) {
        pdf.close(doc);
    }
    pdf.destroy();
    dlclose(pdf.handle);
    close(artifact_fd);
    return error;
}
