/* Native CLI/SDK publication acceptance. Catalog mutation is fixture-only. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "json.h"
#include <dlfcn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
static pid_t owned_children[4];
static void forget_child(pid_t p) {
    for (size_t i = 0; i < 4; i++)
        if (owned_children[i] == p)
            owned_children[i] = 0;
}
static void cleanup_children(void) {
    for (size_t i = 0; i < 4; i++)
        if (owned_children[i] > 0) {
            kill(owned_children[i], SIGKILL);
            while (waitpid(owned_children[i], NULL, 0) < 0 && errno == EINTR) {
            }
        }
}
#define CHECK(x)                                                               \
    do {                                                                       \
        checks++;                                                              \
        if (!(x)) {                                                            \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
static void own_child(pid_t p) {
    for (size_t i = 0; i < 4; i++)
        if (!owned_children[i]) {
            owned_children[i] = p;
            return;
        }
    kill(p, SIGKILL);
    while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {
    }
    CHECK(0);
}
typedef struct {
    int (*create)(const char *, const char *, void **);
    int (*open)(const char *, int, void **);
    int (*call)(void *, const char *, const char *, char **);
    void (*free)(void *);
    void (*close)(void *);
    int (*sql_open)(const char *, void **);
    int (*prepare)(void *, const char *, int, void **, const char **);
    int (*bind)(void *, int, const char *, int, void (*)(void *));
    int (*step)(void *);
    const unsigned char *(*column)(void *, int);
    int (*finalize)(void *);
    int (*sql_close)(void *);
} Sdk;
typedef struct {
    const char *cli, *worker, *library, *fault;
    char files[512], state[512], store[512];
    char *store_id;
    const char *pdfium, *tesseract, *leptonica, *traineddata;
} Host;
static double now(void) {
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void pause_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) < 0)
        CHECK(errno == EINTR);
}
static void symbol(void *lib, const char *name, void *dst, size_t n) {
    void *p = dlsym(lib, name);
    CHECK(p && n == sizeof(p));
    memcpy(dst, &p, n);
}
static void parse(const char *body, Json *j) {
    CHECK(body && json_parse(j, body, strlen(body)) &&
          j->tokens[0].type == JT_OBJECT);
}
static int at(const Json *j, int parent, const char *key) {
    int t = json_get(j, parent, key);
    CHECK(t >= 0);
    return t;
}
static char *str(const Json *j, int parent, const char *key) {
    char *s = json_string(j, at(j, parent, key));
    CHECK(s);
    return s;
}
static int eq(const Json *j, int parent, const char *key, const char *v) {
    char *s = str(j, parent, key);
    int yes = !strcmp(s, v);
    free(s);
    return yes;
}
static size_t num(const Json *j, int parent, const char *key) {
    size_t n = 0;
    CHECK(json_uint(j, at(j, parent, key), &n));
    return n;
}
static int truth(const Json *j, int parent, const char *key) {
    int t = at(j, parent, key);
    CHECK(j->tokens[t].type == JT_BOOL);
    return j->text[j->tokens[t].begin] == 't';
}
static char *slice(const Json *j, int t) {
    CHECK(t >= 0);
    char *s = strndup(j->text + j->tokens[t].begin,
                      j->tokens[t].end - j->tokens[t].begin);
    CHECK(s);
    return s;
}
static char *path_request(const char *path) {
    char *s = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&s, &n);
    CHECK(f);
    fputs("{\"source_path\":", f);
    json_quote(f, path, strlen(path));
    fputs(",\"include_stale\":true}", f);
    CHECK(!fclose(f));
    return s;
}
static char *inspect_pending(Sdk *sdk, const Host *h, const char *op,
                             const char *args, int pending) {
    void *store = NULL;
    CHECK(!sdk->open(h->store, 1, &store));
    char *raw = NULL;
    int rc = sdk->call(store, op, args, &raw);
    if (rc && pending) {
        Json e;
        parse(raw, &e);
        CHECK(eq(&e, at(&e, 0, "error"), "code", "invalid argument"));
        sdk->free(raw);
        sdk->close(store);
        return NULL;
    }
    if (rc)
        fprintf(stderr, "SDK %s: %s\n", op, raw ? raw : "");
    CHECK(!rc && raw);
    char *s = strdup(raw);
    CHECK(s);
    sdk->free(raw);
    sdk->close(store);
    Json j;
    parse(s, &j);
    return s;
}
static char *inspect(Sdk *sdk, const Host *h, const char *op,
                     const char *args) {
    return inspect_pending(sdk, h, op, args, 0);
}
static size_t artifacts(Sdk *sdk, const Host *h) {
    char *s = inspect(sdk, h, "store_info", "{}");
    Json j;
    parse(s, &j);
    size_t n = num(&j, at(&j, 0, "counts"), "artifacts");
    free(s);
    return n;
}
static char *context(Sdk *sdk, const Host *h, const char *path) {
    char *q = path_request(path), *s = inspect(sdk, h, "source_context", q);
    free(q);
    return s;
}
static int artifact(const Json *j, const char *id, const char *content) {
    int a = at(j, 0, "artifacts");
    CHECK(j->tokens[a].type == JT_ARRAY);
    for (int t = a + 1; t < j->tokens[a].next; t = j->tokens[t].next) {
        if (id && eq(j, t, "artifact_id", id))
            return t;
        if (content && eq(j, t, "content", content))
            return t;
        if (!id && !content &&
            eq(j, t, "producer_name", "samosa-sources-text-v1"))
            return t;
    }
    return -1;
}
static int proof(const Json *j, int a) {
    return at(j,
              at(j, at(j, at(j, a, "provenance"), "derivation"), "parameters"),
              "sources_evidence");
}
static void same(const Json *a, int x, const Json *b, int y) {
    char *s = slice(a, x), *t = slice(b, y);
    CHECK(!strcmp(s, t));
    free(s);
    free(t);
}
static void write_bytes(const char *path, const void *bytes, size_t n) {
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(bytes, 1, n, f) == n && !fclose(f));
}
static char *file_bytes(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    CHECK(f && !fseek(f, 0, SEEK_END));
    long z = ftell(f);
    CHECK(z >= 0 && z <= 8388608);
    rewind(f);
    char *s = malloc((size_t)z + 1);
    CHECK(s && fread(s, 1, (size_t)z, f) == (size_t)z && !fclose(f));
    s[z] = 0;
    *n = (size_t)z;
    return s;
}
static pid_t launch(const Host *h, const char *exe, const char *request,
                    int offline, const char *owner, int grant,
                    const char *store_id, int runtime, FILE **output) {
    FILE *input = tmpfile();
    *output = tmpfile();
    CHECK(input && *output);
    if (request)
        CHECK(fwrite(request, 1, strlen(request), input) == strlen(request));
    CHECK(!fflush(input));
    rewind(input);
    const char *args[48];
    size_t n = 0;
    args[n++] = exe;
    args[n++] = "--state";
    args[n++] = h->state;
    args[n++] = "--owner";
    args[n++] = owner ? owner : "publication-owner";
    if (!offline) {
        args[n++] = "--root";
        args[n++] = h->files;
        args[n++] = "--chutni-library";
        args[n++] = h->library;
        args[n++] = "--chutni-store";
        args[n++] = h->store;
        args[n++] = "--chutni-helper";
        args[n++] = h->worker;
        args[n++] = "--chutni-store-id";
        args[n++] = store_id ? store_id : h->store_id;
        if (grant)
            args[n++] = "--chutni-publish";
        if (h->pdfium) {
            args[n++] = "--pdfium-library";
            args[n++] = h->pdfium;
        }
        if (h->tesseract) {
            args[n++] = "--tesseract-library";
            args[n++] = h->tesseract;
            args[n++] = "--leptonica-library";
            args[n++] = h->leptonica;
            args[n++] = "--ocr-traineddata";
            args[n++] = h->traineddata;
        }
    }
    if (runtime) {
        args[n++] = "--index-run";
        args[n++] = "--index-interval-ms";
        args[n++] = "100";
        args[n++] = "--discovery-interval-ms";
        args[n++] = "100";
    }
    args[n] = NULL;
    CHECK(n < 48);
    pid_t p = fork();
    CHECK(p >= 0);
    if (!p) {
        if (dup2(fileno(input), 0) < 0 || dup2(fileno(*output), 1) < 0)
            _exit(2);
        for (int fd = 3; fd < 256; fd++)
            close(fd);
        execv(exe, (char *const *)args);
        _exit(2);
    }
    own_child(p);
    CHECK(!fclose(input));
    return p;
}
static int wait_child(pid_t p, unsigned seconds) {
    int status = 0;
    double end = now() + seconds;
    for (;;) {
        pid_t got = waitpid(p, &status, WNOHANG);
        if (got == p) {
            forget_child(p);
            return status;
        }
        CHECK(got == 0 || (got < 0 && errno == EINTR));
        if (now() >= end) {
            CHECK(!kill(p, SIGKILL));
            while (waitpid(p, &status, 0) < 0)
                CHECK(errno == EINTR);
            forget_child(p);
            CHECK(0);
        }
        pause_ms(10);
    }
}
static char *output_body(FILE *f) {
    CHECK(!fseek(f, 0, SEEK_END));
    long z = ftell(f);
    CHECK(z > 0 && z <= 8388608);
    rewind(f);
    char *s = malloc((size_t)z + 1);
    CHECK(s && fread(s, 1, (size_t)z, f) == (size_t)z && !fclose(f));
    s[z] = 0;
    return s;
}
static char *run_options(const Host *h, const char *q, int offline,
                         const char *owner, int grant, const char *id,
                         const char *error) {
    FILE *out = NULL;
    pid_t p = launch(h, h->cli, q, offline, owner, grant, id, 0, &out);
    int status = wait_child(p, 30);
    char *s = output_body(out);
    int either = error && !strcmp(error, "*");
    if (!WIFEXITED(status) ||
        (!either && WEXITSTATUS(status) != (error ? 1 : 0)))
        fprintf(stderr, "CLI response %s\n", s);
    CHECK(WIFEXITED(status) &&
          (either ? WEXITSTATUS(status) <= 1
                  : WEXITSTATUS(status) == (error ? 1 : 0)));
    CHECK(!strstr(s, h->store));
    Json j;
    parse(s, &j);
    if (error && !either) {
        if (!eq(&j, at(&j, 0, "error"), "code", error))
            fprintf(stderr, "response %s\n", s);
        CHECK(eq(&j, at(&j, 0, "error"), "code", error));
    }
    return s;
}
static char *run(const Host *h, const char *q, int offline, const char *error) {
    return run_options(h, q, offline, NULL, 1, NULL, error);
}
static char *job_request(const char *op, const char *job, const char *id) {
    char *s = NULL;
    size_t n = 0;
    FILE *f = open_memstream(&s, &n);
    CHECK(f);
    fprintf(f, "{\"contract_version\":1,\"op\":\"%s\",\"job_id\":\"%s\"", op,
            job);
    if (id)
        fprintf(f, ",\"operation_id\":\"%s\"", id);
    if (!strcmp(op, "index_step") || !strcmp(op, "index_reindex_step"))
        fputs(",\"max_items\":4", f);
    fputc(125, f);
    CHECK(!fclose(f));
    return s;
}
static char *job_call(const Host *h, const char *op, const char *job,
                      const char *id, int offline, const char *error) {
    char *q = job_request(op, job, id), *s = run(h, q, offline, error);
    free(q);
    return s;
}
static char *index_job(const Host *h, const char *id) {
    char q[512];
    snprintf(q, sizeof(q),
             "{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\","
             "\"operation_id\":\"%s\"}",
             id);
    char *s = run(h, q, 0, NULL);
    Json j;
    parse(s, &j);
    char *job = str(&j, 0, "job_id");
    free(s);
    char step[256];
    snprintf(step, sizeof(step), "%s-step", id);
    free(job_call(h, "index_step", job, step, 0, NULL));
    return job;
}
static char *publish_request(const char *job, const char *id) {
    char *s = NULL;
    size_t n;
    FILE *f = open_memstream(&s, &n);
    CHECK(f);
    fprintf(f,
            "{\"contract_version\":1,\"op\":\"index_publish_step\",\"job_id\":"
            "\"%s\",\"operation_id\":\"%s\",\"timeout_ms\":20000",
            job, id);
    if (!strcmp(id, "publish-first"))
        fputs(",\"parent_operation_id\":\"publication-parent\"", f);
    fputc('}', f);
    CHECK(!fclose(f));
    return s;
}
static char *publish(const Host *h, const char *job, const char *id,
                     const char *error) {
    char *q = publish_request(job, id), *s = run(h, q, 0, error);
    free(q);
    return s;
}
static void publication_status(const Host *h, const char *job,
                               const char *state, const char *accepted) {
    char *s = job_call(h, "index_publication_status", job, NULL, 1, NULL);
    Json j;
    parse(s, &j);
    int a = at(&j, 0, "publications");
    CHECK(j.tokens[a].count == 1);
    int t = a + 1;
    CHECK(eq(&j, t, "state", state));
    int receipt = at(&j, t, "receipt");
    if (accepted) {
        Json p;
        parse(accepted, &p);
        same(&j, at(&j, receipt, "publication"), &p, at(&p, 0, "publication"));
    } else
        CHECK(j.tokens[receipt].type == JT_NULL);
    free(s);
}
static char *catalog(Sdk *sdk, const Host *h, const char *sql, const char *v1,
                     const char *v2) {
    char path[600];
    snprintf(path, sizeof(path), "%s/catalog.sqlite", h->store);
    void *db = NULL, *stmt = NULL;
    CHECK(!sdk->sql_open(path, &db));
    CHECK(!sdk->prepare(db, sql, -1, &stmt, NULL));
    CHECK(!sdk->bind(stmt, 1, v1, -1, NULL));
    if (v2)
        CHECK(!sdk->bind(stmt, 2, v2, -1, NULL));
    int rc = sdk->step(stmt);
    CHECK(rc == 100 || rc == 101);
    char *result = NULL;
    if (rc == 100) {
        const unsigned char *v = sdk->column(stmt, 0);
        CHECK(v);
        result = strdup((const char *)v);
        CHECK(result);
    }
    CHECK(!sdk->finalize(stmt) && !sdk->sql_close(db));
    return result;
}
#include "publication_readers.h"
int main(int argc, char **argv) {
    CHECK(!atexit(cleanup_children));
    CHECK(argc == 5);
    for (int i = 1; i < argc; i++)
        CHECK(argv[i][0] == '/');
    void *lib = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
    CHECK(lib);
    Sdk sdk = {0};
#define SYM(field, name) symbol(lib, name, &sdk.field, sizeof(sdk.field))
    SYM(create, "chutni_create");
    SYM(open, "chutni_open");
    SYM(call, "chutni_call");
    SYM(free, "chutni_free");
    SYM(close, "chutni_close");
    SYM(sql_open, "sqlite3_open");
    SYM(prepare, "sqlite3_prepare_v2");
    SYM(bind, "sqlite3_bind_text");
    SYM(step, "sqlite3_step");
    SYM(column, "sqlite3_column_text");
    SYM(finalize, "sqlite3_finalize");
    SYM(sql_close, "sqlite3_close");
    char root[] = "/tmp/samosa-publication-native-XXXXXX";
    CHECK(mkdtemp(root));
    Host h = {0};
    h.cli = argv[1];
    h.worker = argv[2];
    h.library = argv[3];
    h.fault = argv[4];
    snprintf(h.files, sizeof(h.files), "%s/files", root);
    snprintf(h.state, sizeof(h.state), "%s/state", root);
    snprintf(h.store, sizeof(h.store), "%s/store.chutni", root);
    CHECK(!mkdir(h.files, 0700));
    char source[600], marker[600];
    snprintf(source, sizeof(source), "%s/evidence.txt", h.files);
    snprintf(marker, sizeof(marker), "%s/post-sdk", root);
    const char *first = "Coriander publication proof.\n",
               *second = "Coriander second version.\n",
               *third = "Coriander third version.\n";
    write_bytes(source, first, strlen(first));
    void *store = NULL;
    CHECK(!sdk.create(h.store, "Publication fixture", &store));
    sdk.close(store);
    char *info = inspect(&sdk, &h, "store_info", "{}");
    Json j;
    parse(info, &j);
    h.store_id = str(&j, 0, "store_id");
    size_t initial = num(&j, at(&j, 0, "counts"), "artifacts");
    free(info);
    char *job = index_job(&h, "first-index"),
         *q = publish_request(job, "publish-first"),
         *accepted = run(&h, q, 0, NULL);
    parse(accepted, &j);
    CHECK(num(&j, 0, "published_items") == 1 &&
          !truth(&j, at(&j, 0, "publication"), "recovered") &&
          eq(&j, 0, "receipt_state", "committed"));
    size_t count = artifacts(&sdk, &h);
    CHECK(count == initial + 1);
    char *replay = run(&h, q, 1, NULL);
    CHECK(!strcmp(replay, accepted));
    free(replay);
    publication_status(&h, job, "committed", accepted);
    char *idle = publish(&h, job, "publish-idle", NULL);
    parse(idle, &j);
    CHECK(!num(&j, 0, "published_items") && artifacts(&sdk, &h) == count);
    free(idle);
    parse(accepted, &j);
    char *aid = str(&j, at(&j, 0, "publication"), "external_artifact_id"),
         *sid = str(&j, at(&j, 0, "publication"), "external_source_id");
    char *ctx = context(&sdk, &h, source);
    Json cj;
    parse(ctx, &cj);
    int a = artifact(&cj, aid, NULL);
    CHECK(a >= 0 && eq(&cj, a, "content", first));
    int evidence = proof(&cj, a);
    CHECK(eq(&cj, evidence, "kind", "literal") &&
          eq(&cj, at(&cj, evidence, "coverage"), "status", "complete"));
    char *found = run(&h,
                      "{\"contract_version\":1,\"op\":\"memory_search\","
                      "\"query\":\"Coriander\"}",
                      0, NULL);
    parse(found, &j);
    int items = at(&j, at(&j, at(&j, 0, "memory"), "result"), "items");
    CHECK(eq(&j, items + 1, "external_artifact_id", aid));
    free(found);
    char read[1024];
    snprintf(read, sizeof(read),
             "{\"contract_version\":1,\"op\":\"memory_read\",\"store_id\":\"%"
             "s\",\"external_source_id\":\"%s\",\"external_artifact_id\":\"%"
             "s\",\"operation_id\":\"read-published\"}",
             h.store_id, sid, aid);
    char *retrieved = run(&h, read, 0, NULL);
    parse(retrieved, &j);
    int result = at(&j, at(&j, 0, "memory"), "result");
    CHECK(eq(&j, result, "text", first));
    same(&j, proof(&j, result), &cj, evidence);
    free(retrieved);
    free(ctx);
    free(aid);
    free(sid);
    free(q);
    q = job_request("index_publish_step", job, NULL);
    free(run_options(&h, q, 0, "other-owner", 1, NULL, "unknown_index"));
    free(run_options(&h, q, 0, NULL, 0, NULL, "chutni_publish_disabled"));
    free(run_options(&h, q, 0, NULL, 1, "01234567-89ab-cdef-0123-456789abcdef",
                     "chutni_store_changed"));
    CHECK(artifacts(&sdk, &h) == count);
    free(q);
    free(accepted);
    free(job);
    write_bytes(source, second, strlen(second));
    job = index_job(&h, "second-index");
    q = publish_request(job, "publish-lost");
    CHECK(!setenv("SAMOSA_CHUTNI_PUBLICATION_POST_SDK_MARKER", marker, 1));
    FILE *out = NULL;
    pid_t p = launch(&h, h.fault, q, 0, NULL, 1, NULL, 0, &out);
    CHECK(!unsetenv("SAMOSA_CHUTNI_PUBLICATION_POST_SDK_MARKER"));
    double end = now() + 15;
    while (access(marker, F_OK) && now() < end) {
        int status;
        CHECK(waitpid(p, &status, WNOHANG) == 0);
        pause_ms(10);
    }
    CHECK(!access(marker, F_OK));
    publication_status(&h, job, "pending", NULL);
    CHECK(!kill(p, SIGKILL));
    int status = wait_child(p, 10);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(!fclose(out));
    size_t committed = artifacts(&sdk, &h);
    CHECK(committed == count + 1);
    write_bytes(source, third, strlen(third));
    free(run(&h, q, 1, "operation_interrupted"));
    pause_ms(1100);
    ctx = context(&sdk, &h, source);
    parse(ctx, &cj);
    a = artifact(&cj, NULL, second);
    CHECK(a >= 0);
    aid = str(&cj, a, "artifact_id");
    free(ctx);
    free(catalog(&sdk, &h,
                 "UPDATE artifacts SET inline_text=? WHERE artifact_id=?",
                 "Forged text with intact provenance.", aid));
    free(publish(&h, job, "publish-corrupt-content",
                 "chutni_publication_conflict"));
    publication_status(&h, job, "pending", NULL);
    free(catalog(&sdk, &h,
                 "UPDATE artifacts SET inline_text=? WHERE artifact_id=?",
                 second, aid));
    pause_ms(2100);
    char *recovered = publish(&h, job, "publish-recovered", NULL);
    parse(recovered, &j);
    CHECK(num(&j, 0, "published_items") == 1 &&
          truth(&j, at(&j, 0, "publication"), "recovered"));
    publication_status(&h, job, "committed", recovered);
    CHECK(artifacts(&sdk, &h) == committed);
    free(recovered);
    free(aid);
    free(job);
    free(q);
    job = index_job(&h, "third-index");
    char *latest = publish(&h, job, "publish-third", NULL);
    parse(latest, &j);
    CHECK(num(&j, 0, "published_items") == 1 &&
          artifacts(&sdk, &h) == committed + 1);
    free(latest);
    free(job);
    char partial[600];
    snprintf(partial, sizeof(partial), "%s/partial.txt", h.files);
    char *large = malloc(80011);
    CHECK(large);
    memcpy(large, "Coriander ", 10);
    for (size_t i = 10; i < 80010; i += 2) {
        large[i] = (char)0xc3;
        large[i + 1] = (char)0xa9;
    }
    large[80010] = 0;
    write_bytes(partial, large, 80010);
    job = index_job(&h, "partial-index");
    free(job);
    p = launch(&h, h.cli, NULL, 0, NULL, 1, NULL, 1, &out);
    ctx = NULL;
    end = now() + 30;
    while (now() < end) {
        int st;
        CHECK(waitpid(p, &st, WNOHANG) == 0);
        free(ctx);
        char *pending_path = path_request(partial);
        ctx = inspect_pending(&sdk, &h, "source_context", pending_path, 1);
        free(pending_path);
        if (!ctx) {
            pause_ms(50);
            continue;
        }
        parse(ctx, &cj);
        a = artifact(&cj, NULL, NULL);
        if (a >= 0)
            break;
        pause_ms(50);
    }
    CHECK(a >= 0);
    evidence = proof(&cj, a);
    int cov = at(&cj, evidence, "coverage");
    CHECK(eq(&cj, cov, "status", "partial") &&
          num(&cj, cov, "total_bytes") == 80010);
    size_t inspected = num(&cj, cov, "inspected_bytes");
    CHECK(inspected <= 65536);
    char *text = str(&cj, evidence, "text");
    CHECK(strlen(text) == inspected && !memcmp(text, large, inspected));
    size_t file_n;
    char *input = file_bytes(partial, &file_n);
    CHECK(file_n == 80010 && !memcmp(input, large, file_n));
    free(input);
    free(text);
    free(ctx);
    free(large);
    CHECK(!kill(p, SIGTERM));
    status = wait_child(p, 15);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    char *stopped = output_body(out);
    parse(stopped, &j);
    CHECK(eq(&j, 0, "index_runtime", "stopped"));
    free(stopped);
    CHECK(artifacts(&sdk, &h) == committed + 2);
    reader_acceptance(&sdk, &h);
    ocr_acceptance(&sdk, &h);
    CHECK(!dlclose(lib));
    free(h.store_id);
    fprintf(stdout,
            "PASS native Sources publication core: %u checks; SDK "
            "provenance/retrieval, grants, exact offline replay, content "
            "conflict, runtime UTF-8 range and post-SDK SIGKILL recovery; %s\n",
            checks, root);
    return 0;
}
