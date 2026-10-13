#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "json.h"
#include "ocr.h"
#include "ocr_pdf_fixture.h"
#include "pdf_fixture.h"
#include "sha256.h"
#include "sources.h"
#include "watch.h"
#include <dlfcn.h>
#include <ftw.h>
#include <signal.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static unsigned checks, calls;
#define CHECK(x)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
#include "docx_fixture.h"
static char *field(const char *body, const char *key) {
  Json j;
  CHECK(json_parse(&j, body, strlen(body)));
  char *s = json_string(&j, json_get(&j, 0, key));
  CHECK(s);
  return s;
}
static char *call(const SourcesConfig *cfg, const char *request,
                  const char *error) {
  char *body = NULL;
  size_t n;
  FILE *out = open_memstream(&body, &n);
  CHECK(out);
  int rc = sources_dispatch(cfg, request, strlen(request), out);
  CHECK(!fclose(out));
  if (rc != (error ? 1 : 0))
    fprintf(stderr, "request %s response %s\n", request, body);
  CHECK(rc == (error ? 1 : 0));
  if (error) {
    Json j;
    CHECK(json_parse(&j, body, n));
    char *code =
        json_string(&j, json_get(&j, json_get(&j, 0, "error"), "code"));
    if (!code || strcmp(code, error))
      fprintf(stderr, "response %s\n", body);
    CHECK(code && !strcmp(code, error));
    free(code);
  }
  return body;
}
static const char *blocked_transport(void *context,
                                     const SourcesInferenceControl *control,
                                     const char *input, size_t n,
                                     char **response, size_t *bytes) {
  (void)input;
  (void)n;
  (void)response;
  (void)bytes;
  FILE *marker = fopen(context, "w");
  CHECK(marker && !fclose(marker));
  CHECK(control->cancelled);
  for (unsigned i = 0; i < 500; i++) {
    if (control->cancelled(control->cancellation))
      return "cancelled";
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  return "deadline_exceeded";
}
static const char *transport(void *context,
                             const SourcesInferenceControl *control,
                             const char *input, size_t n, char **response,
                             size_t *bytes) {
  SourcesInferenceConfig *cfg = context;
  calls++;
  CHECK(control->deadline_monotonic > 0);
  Json j;
  CHECK(json_parse(&j, input, n));
  char *op = json_string(&j, json_get(&j, 0, "op")),
       *text = json_string(&j, json_get(&j, 0, "text"));
  CHECK(op && !strcmp(op, "summarize_opening") && text && *text);
  CHECK(strlen(text) <= 60000);
  CHECK(!strstr(text, "POISON"));
  char digest[65];
  hash_bytes(text, strlen(text), digest);
  size_t length = strlen(text);
  free(op);
  free(text);
  FILE *f = open_memstream(response, bytes);
  CHECK(f);
  fprintf(f,
          "{\"contract_version\":1,\"provider_id\":\"%s\",\"kind\":\"derived\","
          "\"artifact_type\":\"summary\",\"input_sha256\":\"%s\",\"input_"
          "bytes\":%zu,\"text\":\"retained "
          "summary\",\"coverage\":{\"status\":\"partial\",\"reason\":\"native_"
          "partitioning_and_generated_summary_uncertain\"},\"provenance\":{"
          "\"engine\":\"samosa-native-t5\",\"engine_sha256\":\"%s\",\"model_"
          "sha256\":\"%s\",\"recipe\":\"samosa-native-summary-opening-map-"
          "reduce-v1\",\"max_output_tokens\":128,\"encoder_context_tokens\":"
          "512,\"processed_input_tokens\":null,\"chunk_max_output_tokens\":64,"
          "\"input_partitioning\":\"native_token_chunks\",\"map_output_bytes\":"
          "16,\"reduction_rounds\":0,\"max_output_bytes\":1800,\"output_"
          "truncated\":false}}",
          cfg->provider_id, digest, length, cfg->engine_sha256,
          cfg->model_sha256);
  CHECK(!fclose(f));
  return NULL;
}
static int remove_file(const char *p, const struct stat *s, int kind,
                       struct FTW *walk) {
  (void)s;
  (void)kind;
  (void)walk;
  return remove(p);
}
static void exec(const char *path, const char *sql) {
  sqlite3 *db = NULL;
  CHECK(sqlite3_open(path, &db) == SQLITE_OK);
  CHECK(sqlite3_busy_timeout(db, 5000) == SQLITE_OK);
  char *error = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
  if (rc != SQLITE_OK)
    fprintf(stderr, "SQL %s: %s\n", sql, error);
  sqlite3_free(error);
  CHECK(rc == SQLITE_OK);
  CHECK(sqlite3_close(db) == SQLITE_OK);
}
static sqlite3_int64 count(const char *path, const char *sql) {
  sqlite3 *db = NULL;
  sqlite3_stmt *s = NULL;
  CHECK(sqlite3_open(path, &db) == SQLITE_OK);
  CHECK(sqlite3_busy_timeout(db, 5000) == SQLITE_OK);
  CHECK(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(s) == SQLITE_ROW);
  sqlite3_int64 n = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  CHECK(sqlite3_close(db) == SQLITE_OK);
  return n;
}
int main(int argc, char **argv) {
  CHECK(argc == 3 && argv[1][0] == '/' && argv[2][0] == '/');
  char root[] = "/tmp/samosa-enrichment-XXXXXX";
  CHECK(mkdtemp(root));
  char files[512], state[512], path[600], storepath[512], database[600],
      marker[512];
  snprintf(files, sizeof(files), "%s/files", root);
  snprintf(state, sizeof(state), "%s/state", root);
  snprintf(path, sizeof(path), "%s/article.txt", files);
  snprintf(storepath, sizeof(storepath), "%s/memory.chutni", root);
  snprintf(database, sizeof(database), "%s/sources.sqlite3", state);
  snprintf(marker, sizeof(marker), "%s/post-sdk", root);
  CHECK(!mkdir(files, 0700));
  FILE *f = fopen(path, "w");
  CHECK(f);
  for (size_t i = 0; i < 4000; i++)
    CHECK(fputs("Retained indexing evidence. ", f) >= 0);
  CHECK(!fclose(f));
  void *lib = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  CHECK(lib);
  int (*create)(const char *, const char *, void **) = NULL;
  int (*sdkcall)(void *, const char *, const char *, char **) = NULL;
  void (*release)(void *) = NULL;
  void (*close_store)(void *) = NULL;
#define SYMBOL(name, target)                                                   \
  do {                                                                         \
    void *p = dlsym(lib, name);                                                \
    CHECK(p && sizeof(p) == sizeof(target));                                   \
    memcpy(&target, &p, sizeof(target));                                       \
  } while (0)
  SYMBOL("chutni_create", create);
  SYMBOL("chutni_call", sdkcall);
  SYMBOL("chutni_free", release);
  SYMBOL("chutni_close", close_store);
  void *store = NULL;
  CHECK(!create(storepath, "Enrichment fixture", &store));
  char *info = NULL;
  CHECK(!sdkcall(store, "store_info", "{}", &info));
  char *storeid = field(info, "store_id");
  release(info);
  close_store(store);
  SourcesInferenceConfig inference = {
      .provider_id = "native-t5-fixture",
      .engine_sha256 =
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      .model_sha256 =
          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      .call = transport};
  inference.context = &inference;
  SourcesConfig cfg = {.root = files,
                       .state = state,
                       .owner = "alice",
                       .summary_inference = &inference,
                       .chutni_publish = 1,
                       .chutni_store_id = storeid,
                       .chutni_store = storepath,
                       .chutni_library = argv[2],
                       .chutni_helper = argv[1]};
  char *body = call(&cfg,
                    "{\"contract_version\":1,\"op\":\"index_create\",\"path\":"
                    "\"\",\"operation_id\":\"create-index\"}",
                    NULL),
       *job = field(body, "job_id");
  free(body);
  char request[512];
  snprintf(request, sizeof(request),
           "{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\","
           "\"operation_id\":\"index-article\",\"max_items\":1}",
           job);
  body = call(&cfg, request, NULL);
  CHECK(strstr(body, "indexed") && !calls);
  free(body);
  SourcesConfig disabled = cfg;
  disabled.summary_inference = NULL;
  body =
      call(&disabled,
           "{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}", NULL);
  CHECK(strstr(body, "enrichment_disabled") && !calls);
  free(body);
  sqlite3_int64 prepared =
      count(database, "SELECT count(*) FROM prepared_artifacts");
  cfg.cache_limit = 1;
  cfg.cache_byte_limit = 1;
  body =
      call(&cfg, "{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}",
           NULL);
  CHECK(strstr(body, "\"phase\":\"captured\"") && calls == 1);
  free(body);
  CHECK(count(database, "SELECT count(*) FROM prepared_artifacts") == prepared);
  CHECK(
      count(database,
            "SELECT count(*) FROM index_enrichments WHERE status='captured'") ==
      1);
  CHECK(!setenv("SAMOSA_CHUTNI_PUBLICATION_POST_SDK_MARKER", marker, 1));
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    char *r = call(
        &cfg, "{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}",
        NULL);
    free(r);
    _exit(0);
  }
  CHECK(!unsetenv("SAMOSA_CHUTNI_PUBLICATION_POST_SDK_MARKER"));
  struct timespec pause = {.tv_nsec = 10000000};
  int observed = 0;
  for (size_t i = 0; i < 1000; i++) {
    if (!access(marker, F_OK)) {
      observed = 1;
      break;
    }
    int status = 0;
    CHECK(waitpid(child, &status, WNOHANG) == 0);
    nanosleep(&pause, NULL);
  }
  CHECK(observed);
  CHECK(!kill(child, SIGKILL));
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
        WTERMSIG(status) == SIGKILL);
  CHECK(!unlink(marker));
  CHECK(!unlink(path));
  inference.call = NULL;
  body =
      call(&cfg, "{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}",
           NULL);
  CHECK(strstr(body, "\"phase\":\"published\"") && calls == 1);
  free(body);
  CHECK(count(database,
              "SELECT count(*) FROM index_enrichments WHERE status='completed' "
              "AND instr(receipt,'recovered')>0") == 1);
  body =
      call(&cfg, "{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}",
           NULL);
  CHECK(strstr(body, "no_due_enrichments") && calls == 1);
  free(body);
  exec(database, "DELETE FROM transcript_publications;UPDATE index_enrichments "
                 "SET status='captured',receipt=NULL,receipt_digest=NULL");
  body =
      call(&cfg, "{\"contract_version\":1,\"op\":\"index_enrich_queue_step\"}",
           NULL);
  CHECK(strstr(body, "published") && calls == 1);
  free(body);
  snprintf(
      request, sizeof(request),
      "{\"contract_version\":1,\"op\":\"index_enrich_step\",\"job_id\":\"%s\"}",
      job);
  SourcesConfig foreign = cfg;
  foreign.owner = "bob";
  body = call(&foreign, request, "unknown_index");
  free(body);
  char report[512];
  snprintf(report, sizeof(report),
           "{\"contract_version\":1,\"op\":\"index_enrichment_status\",\"job_"
           "id\":\"%s\"}",
           job);
  body = call(&cfg, report, NULL);
  CHECK(strstr(body, "retained summary") && strstr(body, "completed") &&
        strstr(body, "recovered"));
  Json history;
  CHECK(json_parse(&history, body, strlen(body)));
  int entries = json_get(&history, 0, "entries"),
      summary = json_get(&history, entries + 1, "summary");
  char *old_source =
      json_string(&history, json_get(&history, summary, "source_id"));
  CHECK(old_source);
  free(body);
  SourcesConfig expired = cfg;
  expired.deadline_monotonic = 1;
  body = call(&expired, request, "deadline_exceeded");
  free(body);
  CHECK(calls == 1);
  inference.model_sha256 =
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  body = call(&cfg, request, "inference_unavailable");
  free(body);
  CHECK(calls == 1);
  inference.call = transport;
  exec(database, "UPDATE index_enrichments SET next_ms=0");
  body = call(&cfg, request, NULL);
  CHECK(strstr(body, "captured") && calls == 2);
  free(body);
  CHECK(count(database, "SELECT count(*) FROM index_enrichments") == 2);
  body = call(&cfg, report, NULL);
  CHECK(strstr(body, "bbbbbbbb") && strstr(body, "cccccccc"));
  free(body);
  /* Existing watch/reindex flow creates a new retained generation; enrichment
   * discovers it without a manual evidence-cache preparation operation. */
  inference.model_sha256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  f = fopen(path, "w");
  CHECK(f &&
        fputs("New retained source version for semantic search.", f) >= 0 &&
        !fclose(f));
  SourcesConfig indexing = cfg;
  indexing.cache_limit = 128;
  indexing.cache_byte_limit = 8388608;
  char watch_request[512];
  snprintf(watch_request, sizeof(watch_request),
           "{\"contract_version\":1,\"op\":\"locator_watch_create\","
           "\"operation_id\":\"enrichment-watch\",\"locators\":[{\"path\":"
           "\"article.txt\",\"expected_version\":\"%s\"}]}",
           old_source);
  body = call(&indexing, watch_request, NULL);
  char *watch = field(body, "watch_id");
  free(body);
  snprintf(watch_request, sizeof(watch_request),
           "{\"contract_version\":1,\"op\":\"locator_watch_step\",\"watch_id\":"
           "\"%s\",\"expected_cycle\":0}",
           watch);
  body = call(&indexing, watch_request, NULL);
  free(body);
  free(watch);
  free(old_source);
  char reindex[512];
  snprintf(reindex, sizeof(reindex),
           "{\"contract_version\":1,\"op\":\"index_reindex_step\",\"job_id\":"
           "\"%s\",\"operation_id\":\"enrichment-reindex\",\"max_items\":1}",
           job);
  body = call(&indexing, reindex, NULL);
  free(body);
  CHECK(calls == 2);
  body = call(&cfg, request, NULL);
  CHECK(strstr(body, "\"generation\":1") && strstr(body, "captured") &&
        calls == 3);
  free(body);
  body = call(&cfg, request, NULL);
  CHECK(strstr(body, "published") && calls == 3);
  free(body);
  body = call(&cfg, report, NULL);
  CHECK(strstr(body, "\"generation\":0") && strstr(body, "\"generation\":1") &&
        strstr(body, "New retained") == NULL);
  free(body);
  char documents[600], html_path[700], docx_path[700];
  snprintf(documents, sizeof(documents), "%s/documents", files);
  CHECK(!mkdir(documents, 0700));
  snprintf(html_path, sizeof(html_path), "%s/a.html", documents);
  snprintf(docx_path, sizeof(docx_path), "%s/b.docx", documents);
  f = fopen(html_path, "w");
  CHECK(f &&
        fputs("<html><body><p>Document evidence &amp; "
              "café.</p><script>POISON</script></body></html>",
              f) >= 0 &&
        !fclose(f));
  size_t zip_bytes = 0;
  unsigned char *zip = docx_package(
      "<w:document xmlns:w='x'><w:body><w:p><w:r><w:t>Document evidence &amp; "
      "café.</w:t></w:r><w:del><w:r><w:t>POISON</w:t></w:r></w:del></w:p></"
      "w:body></w:document>",
      NULL, 8, &zip_bytes);
  f = fopen(docx_path, "wb");
  CHECK(f && fwrite(zip, 1, zip_bytes, f) == zip_bytes && !fclose(f));
  free(zip);
  body = call(&indexing,
              "{\"contract_version\":1,\"op\":\"index_create\",\"path\":"
              "\"documents\",\"operation_id\":\"document-enrichment-index\"}",
              NULL);
  char *document_job = field(body, "job_id");
  free(body);
  char document_request[512];
  snprintf(document_request, sizeof(document_request),
           "{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\","
           "\"operation_id\":\"document-enrichment-read\",\"max_items\":4}",
           document_job);
  body = call(&indexing, document_request, NULL);
  free(body);
  CHECK(calls == 3);
  snprintf(
      document_request, sizeof(document_request),
      "{\"contract_version\":1,\"op\":\"index_enrich_step\",\"job_id\":\"%s\"}",
      document_job);
  for (unsigned i = 0; i < 2; i++) {
    body = call(&cfg, document_request, NULL);
    CHECK(strstr(body, "captured") && calls == 4 + i);
    free(body);
    body = call(&cfg, document_request, NULL);
    CHECK(strstr(body, "published") && calls == 4 + i);
    free(body);
  }
  snprintf(document_request, sizeof(document_request),
           "{\"contract_version\":1,\"op\":\"index_enrichment_status\",\"job_"
           "id\":\"%s\"}",
           document_job);
  body = call(&cfg, document_request, NULL);
  CHECK(strstr(body, "html-readable-v1") && strstr(body, "docx-readable-v1") &&
        strstr(body, "extracted_utf8"));
  free(body);
  free(document_job);
  unsigned expected_calls = 5;
  const char *ppm_path = getenv("ENRICHMENT_OCR_PPM");
  /* Background queue checks below retain cfg.ocr after reader qualification. */
  SourcesOcrConfig ocr = {.tesseract_library = getenv("ENRICHMENT_TESSERACT"),
                          .leptonica_library = getenv("ENRICHMENT_LEPTONICA"),
                          .traineddata_file = getenv("ENRICHMENT_TESSDATA"),
                          .language = "eng"};
  if (ppm_path) {
    indexing.ocr = &ocr;
    indexing.pdfium_library = getenv("ENRICHMENT_PDFIUM");
    indexing.native_helper = getenv("ENRICHMENT_NATIVE_HELPER");
    CHECK(ocr.tesseract_library && ocr.leptonica_library &&
          ocr.traineddata_file && indexing.pdfium_library &&
          indexing.native_helper);
    cfg.ocr = &ocr;
    cfg.pdfium_library = indexing.pdfium_library;
    cfg.native_helper = indexing.native_helper;
    char media[600], target[700];
    snprintf(media, sizeof(media), "%s/ocr-documents", files);
    CHECK(!mkdir(media, 0700));
    snprintf(target, sizeof(target), "%s/a.pdf", media);
    CHECK(pdf_fixture(target, 1));
    f = fopen(ppm_path, "rb");
    CHECK(f);
    size_t ppm_bytes = 306015;
    unsigned char *ppm = malloc(ppm_bytes);
    CHECK(ppm && fread(ppm, 1, ppm_bytes, f) == ppm_bytes && fgetc(f) == EOF &&
          !fclose(f));
    snprintf(target, sizeof(target), "%s/b.pdf", media);
    CHECK(ocr_pdf_fixture_mixed(target, ppm, ppm_bytes, 0, 1));
    snprintf(target, sizeof(target), "%s/c.ppm", media);
    f = fopen(target, "wb");
    CHECK(f && fwrite(ppm, 1, ppm_bytes, f) == ppm_bytes && !fclose(f));
    free(ppm);
    body = call(&indexing,
                "{\"contract_version\":1,\"op\":\"index_create\",\"path\":"
                "\"ocr-documents\",\"operation_id\":\"ocr-enrichment-index\"}",
                NULL);
    char *media_job = field(body, "job_id");
    free(body);
    snprintf(document_request, sizeof(document_request),
             "{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\","
             "\"operation_id\":\"ocr-enrichment-read\",\"max_items\":4}",
             media_job);
    body = call(&indexing, document_request, NULL);
    free(body);
    CHECK(calls == 5);
    snprintf(document_request, sizeof(document_request),
             "{\"contract_version\":1,\"op\":\"index_enrich_step\",\"job_id\":"
             "\"%s\"}",
             media_job);
    for (unsigned i = 0; i < 3; i++) {
      body = call(&cfg, document_request, NULL);
      CHECK(strstr(body, "captured") && calls == 6 + i);
      free(body);
      body = call(&cfg, document_request, NULL);
      CHECK(strstr(body, "published") && calls == 6 + i);
      free(body);
    }
    snprintf(document_request, sizeof(document_request),
             "{\"contract_version\":1,\"op\":\"index_enrichment_status\",\"job_"
             "id\":\"%s\"}",
             media_job);
    body = call(&cfg, document_request, NULL);
    CHECK(strstr(body, "pdfium-text-v1") &&
          strstr(body, "pdfium-index-fallback-v1") &&
          strstr(body, "extracted_utf8"));
    free(body);
    free(media_job);
    expected_calls = 8;
  }
  /* The existing runtime must discover and complete a new model profile without
   * explicit enrichment stepping. Child-local transport counters remain
   * private. */
  inference.model_sha256 =
      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
  pid_t runtime = fork();
  CHECK(runtime >= 0);
  if (!runtime) {
    FILE *sink = fopen("/dev/null", "w");
    if (!sink)
      _exit(91);
    int rc = sources_queue_run(&cfg, 100, sink);
    fclose(sink);
    _exit(rc);
  }
  int runtime_completed = 0;
  for (unsigned i = 0; i < 500; i++) {
    if (count(database,
              "SELECT count(*) FROM index_enrichments WHERE status='completed' "
              "AND capture LIKE '%dddddddddddddddd%'") > 0) {
      runtime_completed = 1;
      break;
    }
    int status;
    CHECK(waitpid(runtime, &status, WNOHANG) == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(runtime_completed);
  CHECK(!kill(runtime, SIGTERM));
  int runtime_status = 0, runtime_stopped = 0;
  for (unsigned i = 0; i < 500; i++) {
    pid_t result = waitpid(runtime, &runtime_status, WNOHANG);
    CHECK(result >= 0);
    if (result == runtime) {
      runtime_stopped = 1;
      break;
    }
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(runtime_stopped && WIFEXITED(runtime_status) &&
        WEXITSTATUS(runtime_status) == 0);
  CHECK(calls == expected_calls);
  inference.model_sha256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  char active_marker[600];
  snprintf(active_marker, sizeof(active_marker), "%s/active-model", root);
  inference.model_sha256 =
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  inference.call = blocked_transport;
  inference.context = active_marker;
  runtime = fork();
  CHECK(runtime >= 0);
  if (!runtime) {
    FILE *sink = fopen("/dev/null", "w");
    if (!sink)
      _exit(92);
    int rc = sources_queue_run(&cfg, 100, sink);
    fclose(sink);
    _exit(rc);
  }
  int entered = 0;
  for (unsigned i = 0; i < 500; i++) {
    if (!access(active_marker, F_OK)) {
      entered = 1;
      break;
    }
    int status;
    CHECK(waitpid(runtime, &status, WNOHANG) == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(entered && !kill(runtime, SIGTERM));
  runtime_stopped = 0;
  for (unsigned i = 0; i < 500; i++) {
    pid_t result = waitpid(runtime, &runtime_status, WNOHANG);
    CHECK(result >= 0);
    if (result == runtime) {
      runtime_stopped = 1;
      break;
    }
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(runtime_stopped && WIFEXITED(runtime_status) &&
        WEXITSTATUS(runtime_status) == 0);
  CHECK(count(database, "SELECT count(*) FROM index_enrichments WHERE "
                        "status='pending' AND last_error='cancelled'") > 0);
  inference.call = transport;
  inference.context = &inference;
  exec(database, "UPDATE index_enrichments SET next_ms=0 WHERE "
                 "status='pending' AND last_error='cancelled'");
  body = call(&cfg, request, NULL);
  CHECK(strstr(body, "captured"));
  free(body);
  expected_calls++;
  CHECK(calls == expected_calls);
  body = call(&cfg, request, NULL);
  CHECK(strstr(body, "published") && calls == expected_calls);
  free(body);
  inference.model_sha256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  CHECK(!unlink(active_marker));
  inference.model_sha256 =
      "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  inference.call = blocked_transport;
  inference.context = active_marker;
  runtime = fork();
  CHECK(runtime >= 0);
  if (!runtime) {
    FILE *sink = fopen("/dev/null", "w");
    if (!sink)
      _exit(93);
    int rc = sources_queue_run(&cfg, 100, sink);
    fclose(sink);
    _exit(rc);
  }
  entered = 0;
  for (unsigned i = 0; i < 500; i++) {
    if (!access(active_marker, F_OK)) {
      entered = 1;
      break;
    }
    int status;
    CHECK(waitpid(runtime, &status, WNOHANG) == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(entered && !kill(runtime, SIGKILL));
  CHECK(waitpid(runtime, &runtime_status, 0) == runtime &&
        WIFSIGNALED(runtime_status) && WTERMSIG(runtime_status) == SIGKILL);
  CHECK(count(database, "SELECT count(*) FROM index_enrichments WHERE "
                        "status='pending' AND capture IS NULL") > 0);
  inference.call = transport;
  inference.context = &inference;
  runtime = fork();
  CHECK(runtime >= 0);
  if (!runtime) {
    FILE *sink = fopen("/dev/null", "w");
    if (!sink)
      _exit(94);
    int rc = sources_queue_run(&cfg, 100, sink);
    fclose(sink);
    _exit(rc);
  }
  runtime_completed = 0;
  for (unsigned i = 0; i < 750; i++) {
    if (count(database,
              "SELECT count(*) FROM index_enrichments WHERE status='completed' "
              "AND capture LIKE '%ffffffffffffffff%'") > 0) {
      runtime_completed = 1;
      break;
    }
    int status;
    CHECK(waitpid(runtime, &status, WNOHANG) == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(runtime_completed && !kill(runtime, SIGTERM));
  runtime_stopped = 0;
  for (unsigned i = 0; i < 500; i++) {
    pid_t result = waitpid(runtime, &runtime_status, WNOHANG);
    CHECK(result >= 0);
    if (result == runtime) {
      runtime_stopped = 1;
      break;
    }
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(runtime_stopped && WIFEXITED(runtime_status) &&
        WEXITSTATUS(runtime_status) == 0);
  CHECK(calls == expected_calls);
  inference.model_sha256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  inference.model_sha256 =
      "9999999999999999999999999999999999999999999999999999999999999999";
  inference.call = NULL;
  runtime = fork();
  CHECK(runtime >= 0);
  if (!runtime) {
    FILE *sink = fopen("/dev/null", "w");
    if (!sink)
      _exit(95);
    int rc = sources_queue_run(&cfg, 100, sink);
    fclose(sink);
    _exit(rc);
  }
  int dependency_failed = 0;
  for (unsigned i = 0; i < 500; i++) {
    if (count(
            database,
            "SELECT count(*) FROM index_enrichments WHERE status='pending' AND "
            "last_error='inference_unavailable' AND failures>0 AND next_ms>0") >
        0) {
      dependency_failed = 1;
      break;
    }
    int status;
    CHECK(waitpid(runtime, &status, WNOHANG) == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(dependency_failed);
  struct timespec recovery_pause = {0, 200000000};
  nanosleep(&recovery_pause, NULL);
  CHECK(waitpid(runtime, &runtime_status, WNOHANG) == 0);
  CHECK(!kill(runtime, SIGTERM));
  runtime_stopped = 0;
  for (unsigned i = 0; i < 500; i++) {
    pid_t result = waitpid(runtime, &runtime_status, WNOHANG);
    CHECK(result >= 0);
    if (result == runtime) {
      runtime_stopped = 1;
      break;
    }
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
  }
  CHECK(runtime_stopped && WIFEXITED(runtime_status) &&
        WEXITSTATUS(runtime_status) == 0);
  CHECK(calls == expected_calls);
  inference.call = transport;
  inference.model_sha256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  exec(database, "UPDATE index_enrichments SET capture_digest='damaged' WHERE "
                 "status='captured'");
  body = call(&cfg, request, "corrupt_enrichment");
  free(body);
  CHECK(calls == expected_calls);
  CHECK(count(database, "SELECT count(*) FROM prepared_artifacts") == prepared);
  free(storeid);
  free(job);
  CHECK(!dlclose(lib));
  CHECK(!nftw(root, remove_file, 32, FTW_DEPTH | FTW_PHYS));
  printf("PASS automatic retained enrichment: %u checks; actual SDK, durable "
         "capture/publication, SIGKILL recovery, cache quota independence, "
         "owner/model isolation and corruption\n",
         checks);
  return 0;
}
