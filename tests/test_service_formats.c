#define main service_socket_corpus
#include "test_service.c"
#undef main
#include "docx_fixture.h"
#include "pdf_fixture.h"
#include "sha256.h"
static char *upload_format(unsigned port, const char *format, const void *data,
                           size_t bytes) {
    size_t length;
    char *body = multipart(data, bytes, &length);
    char extra[128];
    snprintf(extra, sizeof(extra),
             "X-Sources-Format: %s\r\nExpect: 100-continue\r\n", format);
    int fd = request_head(port, 0, "/v1/uploads", length, extra);
    interim(fd);
    send_all(fd, body, length);
    free(body);
    char *response = receive(fd);
    if (!strstr(response, "200"))
        fprintf(stderr, "format %s: %s\n", format, response);
    CHECK(strstr(response, "200"));
    char *source = reply_string(response, "source_id"),
         *version = reply_string(response, "version");
    char expected[65];
    hash_bytes(data, bytes, expected);
    CHECK(!strcmp(source, expected) && !strcmp(version, expected));
    free(version);
    free(response);
    return source;
}
static void little32(unsigned char *p, unsigned v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (i * 8));
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    alarm(180);
    signal(SIGPIPE, SIG_IGN);
    umask(077);
    char root[] = "/tmp/samosa-service-formats-XXXXXX";
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
    pid_t service;
    unsigned port = start(argv[1], root, state, auth, &service);
    const char text[] = "literal text upload";
    const char html[] =
        "<!DOCTYPE html><html><body><p>html upload</p></body></html>";
    char *ids[7];
    ids[0] = upload_format(port, "text", text, sizeof(text) - 1);
    ids[2] = upload_format(port, "html", html, sizeof(html) - 1);
    char pdf[512];
    snprintf(pdf, sizeof(pdf), "%s/test.pdf", root);
    CHECK(pdf_fixture(pdf, 1));
    f = fopen(pdf, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long size = ftell(f);
    CHECK(size > 0);
    CHECK(!fseek(f, 0, SEEK_SET));
    unsigned char *bytes = malloc((size_t)size);
    CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t)size, f) == (size_t)size);
    CHECK(!fclose(f));
    ids[1] = upload_format(port, "pdf", bytes, (size_t)size);
    free(bytes);
    size_t docx_length;
    unsigned char *docx =
        docx_package("<w:document "
                     "xmlns:w='http://schemas.openxmlformats.org/"
                     "wordprocessingml/2006/main'><w:body><w:p><w:r><w:t>docx "
                     "upload</w:t></w:r></w:p></w:body></w:document>",
                     NULL, 8, &docx_length);
    ids[3] = upload_format(port, "docx", docx, docx_length);
    free(docx);
    const unsigned char image[] = {'P', '6', '\n', '1',  ' ', '1', '\n',
                                   '2', '5', '5',  '\n', 255, 0,   0};
    ids[4] = upload_format(port, "image", image, sizeof(image));
    unsigned char *wave = calloc(32044, 1);
    CHECK(wave);
    memcpy(wave, "RIFF", 4);
    little32(wave + 4, 32036);
    memcpy(wave + 8, "WAVEfmt ", 8);
    little32(wave + 16, 16);
    wave[20] = wave[22] = 1;
    little32(wave + 24, 16000);
    little32(wave + 28, 32000);
    wave[32] = 2;
    wave[34] = 16;
    memcpy(wave + 36, "data", 4);
    little32(wave + 40, 32000);
    ids[5] = upload_format(port, "wav", wave, 32044);
    ids[6] = upload_format(port, "media", wave, 32044);
    free(wave);
    const char *operations[] = {"read_text", "read_html", "read_docx"};
    const unsigned indices[] = {0, 2, 3};
    for (unsigned i = 0; i < 3; i++) {
        char request[512];
        snprintf(request, sizeof(request),
                 "{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\","
                 "\"version\":\"%s\",\"start_byte\":0,\"end_byte\":%u}",
                 operations[i], ids[indices[i]], ids[indices[i]],
                 i ? 11u : 19u);
        char *response = call(port, 0, request);
        CHECK(strstr(response, "200") && strstr(response, "upload"));
        free(response);
        response = call(port, 1, request);
        CHECK(strstr(response, "400") && !strstr(response, " upload"));
        free(response);
    }
    stop(service);
    port = start(argv[1], root, state, auth, &service);
    for (unsigned i = 0; i < 3; i++) {
        char request[512];
        snprintf(request, sizeof(request),
                 "{\"contract_version\":1,\"op\":\"%s\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_byte\":0,\"end_byte\":%u}",
                 operations[i],ids[indices[i]],ids[indices[i]],i ? 11u : 19u);
        char *response = call(port, 0, request);
        CHECK(strstr(response, "200") && strstr(response,"upload"));
        free(response);
    }
    for (unsigned i=0;i<7;i++) free(ids[i]);
    stop(service);
    printf("PASS native socket upload formats: %u checks; all seven enums with "
           "real text/HTML/PDF/deflated DOCX/PPM/PCM WAV/container bytes, "
           "independent SHA256 identity, literal text/HTML/DOCX owner readback "
           "and restart; decoder/model quality is separately qualified; %s\n",
           checks, root);
    return 0;
}
