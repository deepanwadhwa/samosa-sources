#include "json.h"
#include "service_http.h"
#include "sources.h"
#include <strings.h>

static int identity(const char *s) {
    if (!s || !*s || strlen(s) > 128)
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || strchr("-_:.", *s)))
            return 0;
    return 1;
}
static int token_valid(const char *s) {
    if (!s || strlen(s) != 64)
        return 0;
    for (size_t i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}
int sources_http_auth_parse(const char *body, size_t n, SourcesHttpAuth *auth) {
    SourcesHttpAuth result = {0};
    Json j;
    if (!body || !auth || n > 4096 || !json_parse(&j, body, n) ||
        j.tokens[0].type != JT_ARRAY || !j.tokens[0].count ||
        (size_t)j.tokens[0].count > SOURCES_HTTP_PRINCIPALS_MAX)
        return 0;
    for (int t = 1; t < j.tokens[0].next; t = j.tokens[t].next) {
        if (j.tokens[t].type != JT_OBJECT || j.tokens[t].count != 2)
            return 0;
        char *owner = json_string(&j, json_get(&j, t, "owner"));
        char *token = json_string(&j, json_get(&j, t, "token"));
        int valid = identity(owner) && token_valid(token);
        for (size_t i = 0; valid && i < result.count; i++)
            if (!strcmp(owner, result.principals[i].owner) ||
                !strcmp(token, result.principals[i].token))
                valid = 0;
        if (valid) {
            strcpy(result.principals[result.count].owner, owner);
            strcpy(result.principals[result.count++].token, token);
        }
        free(owner);
        free(token);
        if (!valid)
            return 0;
    }
    *auth = result;
    return 1;
}
static int decimal(const char *s, size_t *value) {
    if (!*s)
        return 0;
    size_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (SIZE_MAX - (size_t)(*s - '0')) / 10)
            return 0;
        v = v * 10 + (size_t)(*s - '0');
    }
    *value = v;
    return 1;
}
int sources_http_header(const SourcesHttpAuth *auth, const char *body, size_t n,
                        SourcesHttpRequest *request) {
    if (!auth || !auth->count || auth->count > SOURCES_HTTP_PRINCIPALS_MAX ||
        !request || !body)
        return 500;
    if (n > SOURCES_HTTP_HEADER_MAX)
        return 431;
    if (n < 4 || memchr(body, 0, n) || memcmp(body + n - 4, "\r\n\r\n", 4))
        return 400;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c > 126 || (c < 32 && c != '\r' && c != '\n' && c != '\t') ||
            (c == '\n' && (!i || body[i - 1] != '\r')) ||
            (c == '\r' && (i + 1 == n || body[i + 1] != '\n')))
            return 400;
    }
    char copy[SOURCES_HTTP_HEADER_MAX + 1];
    memcpy(copy, body, n);
    copy[n] = 0;
    char *line = copy, *end = strstr(line, "\r\n");
    if (!end)
        return 400;
    *end = 0;
    SourcesHttpRequest result = {0};
    if (!strcmp(line, "POST /v1/sources HTTP/1.1"))
        result.upload = 0;
    else if (!strcmp(line, "POST /v1/uploads HTTP/1.1"))
        result.upload = 1;
    else if (!strcmp(line, "GET /healthz HTTP/1.1"))
        result.health = 1;
    else
        return 404;
    const char *names[64];
    size_t count = 0, length = 0;
    char *authorization = NULL, *content_type = NULL;
    int have_length = 0, have_host = 0;
    line = end + 2;
    while (*line) {
        end = strstr(line, "\r\n");
        if (!end)
            return 400;
        *end = 0;
        if (!*line) {
            if (end != copy + n - 2)
                return 400;
            break;
        }
        if (count == 64 || *line == ' ' || *line == '\t')
            return 400;
        char *colon = strchr(line, ':');
        if (!colon || colon == line)
            return 400;
        *colon = 0;
        for (char *p = line; *p; p++)
            if (!(isalnum((unsigned char)*p) || *p == '-'))
                return 400;
        for (size_t i = 0; i < count; i++)
            if (!strcasecmp(names[i], line))
                return 400;
        names[count++] = line;
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        char *tail = value + strlen(value);
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t'))
            *--tail = 0;
        if (!strcasecmp(line, "Authorization"))
            authorization = value;
        else if (!strcasecmp(line, "Content-Length")) {
            if (!decimal(value, &length))
                return 400;
            have_length = 1;
        } else if (!strcasecmp(line, "Host")) {
            if (!*value || strchr(value, ' ') || strchr(value, '\t'))
                return 400;
            have_host = 1;
        } else if (!strcasecmp(line, "Content-Type"))
            content_type = value;
        else if (!strcasecmp(line, "Transfer-Encoding"))
            return 400;
        else if (!strcasecmp(line, "Origin"))
            return 403;
        else if (!strcasecmp(line, "Expect")) {
            if (strcasecmp(value, "100-continue"))
                return 417;
            result.expect_continue = 1;
        } else if (!strcasecmp(line, "Connection")) {
            if (strcasecmp(value, "close"))
                return 400;
        } else if (!strcasecmp(line, "X-Request-ID")) {
            if (!identity(value))
                return 400;
            strcpy(result.request_id, value);
        } else if (!strcasecmp(line, "X-Sources-Format")) {
            if (strcmp(value, "text") && strcmp(value, "pdf") &&
                strcmp(value, "html") && strcmp(value, "docx") &&
                strcmp(value, "image") && strcmp(value, "wav") &&
                strcmp(value, "media"))
                return 400;
            strcpy(result.format, value);
        }
        line = end + 2;
    }
    if (!authorization || strncmp(authorization, "Bearer ", 7) ||
        !token_valid(authorization + 7))
        return 401;
    unsigned found = 0;
    for (size_t i = 0; i < auth->count; i++) {
        unsigned different = 0;
        for (size_t k = 0; k < 64; k++)
            different |= (unsigned char)authorization[7 + k] ^
                         (unsigned char)auth->principals[i].token[k];
        if (!different) {
            result.owner_index = i;
            found = 1;
        }
    }
    if (!found)
        return 401;
    if (!have_host)
        return 400;
    if (result.health) {
        if (length || result.expect_continue)
            return 400;
        *request = result;
        return 0;
    }
    if (!have_length || !content_type || !length)
        return 400;
    if (result.upload) {
        if (!*result.format)
            return 400;
        const char prefix[] = "multipart/form-data; boundary=";
        if (strncmp(content_type, prefix, sizeof(prefix) - 1))
            return 415;
        const char *boundary = content_type + sizeof(prefix) - 1;
        size_t b = strlen(boundary);
        if (!b || b > 70)
            return 400;
        for (size_t i = 0; i < b; i++)
            if (!(isalnum((unsigned char)boundary[i]) ||
                  strchr("'()+_,-./:=?", boundary[i])))
                return 400;
        strcpy(result.boundary, boundary);
        if (length > SOURCES_INPUT_MAX + 8192u)
            return 413;
    } else {
        if (strcmp(content_type, "application/json"))
            return 415;
        if (length > SOURCES_REQUEST_MAX)
            return 413;
    }
    result.content_length = length;
    *request = result;
    return 0;
}
