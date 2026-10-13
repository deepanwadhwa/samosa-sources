#include "service_upload.h"
#include <string.h>
#include <strings.h>
typedef struct {
    SourcesUploadRead read;
    void *context;
    size_t remaining;
    char closing[80];
    size_t closing_length;
    char delimiter[76];
    size_t delimiter_length, matched;
    int ended;
} Multipart;
static int exact(Multipart *m, void *bytes, size_t n) {
    unsigned char *p = bytes;
    while (n) {
        size_t got = 0;
        if (m->read(m->context, p, n, &got) || !got || got > n)
            return 0;
        p += got;
        n -= got;
    }
    return 1;
}
static int multipart_read(void *context, void *bytes, size_t capacity,
                          size_t *got) {
    Multipart *m = context;
    *got = 0;
    if (m->remaining) {
        if (capacity > m->remaining)
            capacity = m->remaining;
        if (m->read(m->context, bytes, capacity, got) || !*got ||
            *got > capacity)
            return 1;
        const unsigned char *data = bytes;
        for (size_t i = 0; i < *got; i++) {
            if (data[i] == (unsigned char)m->delimiter[m->matched])
                m->matched++;
            else
                m->matched = data[i] == '\r' ? 1u : 0u;
            if (m->matched == m->delimiter_length)
                return 1;
        }
        m->remaining -= *got;
        return 0;
    }
    if (!m->ended) {
        char actual[80];
        if (!exact(m, actual, m->closing_length) ||
            memcmp(actual, m->closing, m->closing_length))
            return 1;
        m->ended = 1;
    }
    return 0;
}
static int fail(FILE *out) {
    fputs(
        "{\"contract_version\":1,\"error\":{\"code\":\"invalid_multipart\"}}\n",
        out);
    return 1;
}
int sources_service_upload(const SourcesConfig *cfg,
                           const SourcesHttpRequest *request,
                           SourcesUploadRead read, void *context, FILE *out) {
    static const char *formats[] = {"text",  "pdf", "html", "docx",
                                    "image", "wav", "media"};
    int format = -1;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        if (!strcmp(request->format, formats[i]))
            format = (int)i;
    if (format < 0 || !read || !request->upload || !*request->boundary)
        return fail(out);
    Multipart m = {.read = read, .context = context};
    int closing = snprintf(m.closing, sizeof(m.closing), "\r\n--%s--\r\n",
                           request->boundary);
    if (closing <= 0 || (size_t)closing >= sizeof(m.closing))
        return fail(out);
    m.closing_length = (size_t)closing;
    int delimiter = snprintf(m.delimiter, sizeof(m.delimiter), "\r\n--%s",
                             request->boundary);
    if (delimiter <= 0 || (size_t)delimiter >= sizeof(m.delimiter))
        return fail(out);
    m.delimiter_length = (size_t)delimiter;
    char header[4097];
    size_t used = 0;
    while (used < 4096 && used < request->content_length) {
        if (!exact(&m, header + used, 1))
            return fail(out);
        used++;
        if (used >= 4 && !memcmp(header + used - 4, "\r\n\r\n", 4))
            break;
    }
    if (used < 4 || memcmp(header + used - 4, "\r\n\r\n", 4) ||
        request->content_length < used + m.closing_length)
        return fail(out);
    header[used] = 0;
    for (size_t i = 0; i < used; i++) {
        unsigned char c = (unsigned char)header[i];
        if (c > 126 || (c < 32 && c != '\r' && c != '\n') ||
            (c == '\n' && (!i || header[i - 1] != '\r')) ||
            (c == '\r' && (i + 1 == used || header[i + 1] != '\n')))
            return fail(out);
    }
    char first[80];
    int first_length =
        snprintf(first, sizeof(first), "--%s\r\n", request->boundary);
    if (first_length <= 0 || (size_t)first_length >= used ||
        memcmp(header, first, (size_t)first_length))
        return fail(out);
    char *line = header + first_length;
    int disposition = 0, type = 0;
    while (*line != '\r') {
        char *end = strstr(line, "\r\n");
        if (!end)
            return fail(out);
        *end = 0;
        const char prefix[] = "Content-Disposition: form-data; name=\"file\"";
        if (!strncasecmp(line, prefix, sizeof(prefix) - 1)) {
            if (disposition++)
                return fail(out);
            char *tail = line + sizeof(prefix) - 1;
            if (*tail) {
                const char filename[] = "; filename=\"";
                if (strncmp(tail, filename, sizeof(filename) - 1))
                    return fail(out);
                tail += sizeof(filename) - 1;
                size_t length = strlen(tail);
                if (!length || length > 256 || tail[length - 1] != '"' ||
                    memchr(tail, '"', length - 1) || strchr(tail, '\\'))
                    return fail(out);
            }
        } else if (!strncasecmp(line, "Content-Type: ", 14)) {
            if (type++ || !line[14])
                return fail(out);
        } else
            return fail(out);
        line = end + 2;
    }
    if (!disposition)
        return fail(out);
    m.remaining = request->content_length - used - m.closing_length;
    if (m.remaining > SOURCES_INPUT_MAX)
        return fail(out);
    return sources_upload_stream(cfg, (SourcesUploadFormat)format, m.remaining,
                                 multipart_read, &m, out);
}
