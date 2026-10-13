#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "json.h"
#include "semantic.h"
#include "sources.h"
#include <dlfcn.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static void symbol(void *lib, const char *name, void *target, size_t n) {
  void *p = dlsym(lib, name);
  CHECK(p && n == sizeof(p));
  memcpy(target, &p, n);
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
    fprintf(stderr, "request: %s\nresponse: %s\n", request, body);
  CHECK(rc == (error ? 1 : 0));
  Json j;
  CHECK(json_parse(&j, body, n));
  if (error) {
    char *code =
        json_string(&j, json_get(&j, json_get(&j, 0, "error"), "code"));
    if (!code || strcmp(code, error))
      fprintf(stderr, "response: %s\n", body);
    CHECK(code && !strcmp(code, error));
    free(code);
  }
  return body;
}
static char *field(const char *body, const char *key) {
  Json j;
  CHECK(json_parse(&j, body, strlen(body)));
  char *value = json_string(&j, json_get(&j, 0, key));
  CHECK(value);
  return value;
}
static char *slice(const Json *j, int token) {
  CHECK(token >= 0);
  char *s = strndup(j->text + j->tokens[token].begin,
                    j->tokens[token].end - j->tokens[token].begin);
  CHECK(s);
  return s;
}
static char *replace(const char *body, const char *from, const char *to) {
  const char *p = strstr(body, from);
  CHECK(p);
  size_t a = (size_t)(p - body), b = strlen(from), c = strlen(to),
         n = strlen(body);
  char *s = malloc(n - b + c + 1);
  CHECK(s);
  memcpy(s, body, a);
  memcpy(s + a, to, c);
  memcpy(s + a + c, p + b, n - a - b + 1);
  return s;
}
static int cancelled(void *context) {
  (void)context;
  return 1;
}
int main(int argc, char **argv) {
  CHECK(argc == 3);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(library);
  int (*create)(const char *, const char *, void **) = NULL;
  int (*open_store)(const char *, int, void **) = NULL;
  int (*sdk_call)(void *, const char *, const char *, char **) = NULL;
  void (*sdk_free)(void *) = NULL;
  void (*close_store)(void *) = NULL;
  symbol(library, "chutni_create", &create, sizeof(create));
  symbol(library, "chutni_open", &open_store, sizeof(open_store));
  symbol(library, "chutni_call", &sdk_call, sizeof(sdk_call));
  symbol(library, "chutni_free", &sdk_free, sizeof(sdk_free));
  symbol(library, "chutni_close", &close_store, sizeof(close_store));
  char root[] = "/tmp/samosa-semantic-XXXXXX";
  CHECK(mkdtemp(root));
  char store_path[512], state[512];
  snprintf(store_path, sizeof(store_path), "%s/memory.chutni", root);
  snprintf(state, sizeof(state), "%s/state", root);
  void *store = NULL;
  CHECK(!create(store_path, "Sources semantic fixture", &store));
  const char *profile =
      "{\"representation_kind\":\"text_embedding\",\"model_id\":\"fixture/"
      "embed\",\"model_revision\":\"1\",\"dimensions\":2,\"dtype\":\"f32\","
      "\"normalization\":\"none\"}";
  char *first_artifact = NULL, *first_source = NULL;
  for (size_t i = 0; i < 12; i++) {
    char request[1024], *response = NULL;
    snprintf(request, sizeof(request),
             "{\"memory_kind\":\"note\",\"text\":\"Semantic fixture note "
             "%zu.\",\"producer\":{\"producer_kind\":\"human\",\"name\":"
             "\"integration-fixture\"},\"operation\":\"test-semantic\"}",
             i);
    CHECK(!sdk_call(store, "put_memory", request, &response));
    CHECK(response);
    char *artifact = field(response, "artifact_id");
    if (!i) {
      first_artifact = strdup(artifact);
      first_source = field(response, "memory_id");
    }
    sdk_free(response);
    response = NULL;
    snprintf(request, sizeof(request),
             "{\"artifact_id\":\"%s\",\"profile\":%s,\"vector\":[%zu,%zu]}",
             artifact, profile, 12 - i, i);
    CHECK(!sdk_call(store, "put_representation", request, &response));
    CHECK(response);
    sdk_free(response);
    free(artifact);
  }
  char *info = NULL;
  CHECK(!sdk_call(store, "store_info", "{}", &info));
  char *id = field(info, "store_id");
  Json j;
  CHECK(json_parse(&j, info, strlen(info)));
  char *counts = slice(&j, json_get(&j, 0, "counts"));
  sdk_free(info);
  close_store(store);
  store = NULL;
  SourcesConfig cfg = {.state = state,
                       .owner = "alice",
                       .chutni_library = argv[1],
                       .chutni_store = store_path,
                       .chutni_helper = argv[2],
                       .chutni_store_id = id,
                       .chutni_semantic_profile = profile};
  const char *request =
      "{\"contract_version\":1,\"op\":\"memory_search_semantic\",\"vector\":[1,"
      "0],\"operation_id\":\"semantic-1\",\"timeout_ms\":10000}";
  char *accepted = call(&cfg, request, NULL);
  CHECK(!strstr(accepted, store_path));
  CHECK(json_parse(&j, accepted, strlen(accepted)));
  int result = json_get(&j, json_get(&j, 0, "memory"), "result"),
      items = json_get(&j, result, "items"),
      query = json_get(&j, result, "query");
  CHECK(items >= 0 && j.tokens[items].count == 8);
  char *normalized = slice(&j, result), *arguments = slice(&j, query);
  CHECK(!sources_chutni_semantic_validate(normalized, strlen(normalized),
                                          arguments, id));
  double previous = 2;
  for (int t = items + 1; t < j.tokens[items].next; t = j.tokens[t].next) {
    char *type = json_string(&j, json_get(&j, t, "score_type"));
    CHECK(type && !strcmp(type, "cosine_bruteforce"));
    free(type);
    int score = json_get(&j, t, "score");
    char *number = slice(&j, score);
    double x = strtod(number, NULL);
    free(number);
    CHECK(isfinite(x) && x <= previous && x > 0);
    previous = x;
  }
  char *top = json_string(&j, json_get(&j, items + 1, "external_artifact_id"));
  CHECK(top && !strcmp(top, first_artifact));
  free(top);
  char *bad = replace(normalized, "cosine_bruteforce", "bm25_fts5_negated");
  CHECK(sources_chutni_semantic_validate(bad, strlen(bad), arguments, id));
  free(bad);
  bad = replace(normalized, "\"score\":1", "\"score\":2");
  CHECK(sources_chutni_semantic_validate(bad, strlen(bad), arguments, id));
  free(bad);
  bad = replace(normalized, "\"score\":1", "\"score\":0");
  CHECK(sources_chutni_semantic_validate(bad, strlen(bad), arguments, id));
  free(bad);
  CHECK(sources_chutni_semantic_validate(
      normalized, strlen(normalized), arguments,
      "01234567-89ab-cdef-0123-456789abcdef"));
  free(normalized);
  free(arguments);
  const char *decimal_request =
      "{\"contract_version\":1,\"op\":\"memory_search_semantic\",\"vector\":[1."
      "5,0],\"operation_id\":\"semantic-decimal\"}";
  char *decimal = call(&cfg, decimal_request, NULL);
  char *decimal_replay =
      call(&cfg,
           "{\"operation_id\":\"semantic-decimal\",\"vector\":[1.500,0.000],"
           "\"op\":\"memory_search_semantic\",\"contract_version\":1}",
           NULL);
  CHECK(!strcmp(decimal, decimal_replay));
  free(decimal);
  free(decimal_replay);
  SourcesConfig offline = cfg;
  offline.chutni_library = NULL;
  offline.chutni_helper = NULL;
  offline.chutni_semantic_profile = NULL;
  char *body = call(&offline, request, NULL);
  CHECK(!strcmp(body, accepted));
  free(body);
  char *source = field(accepted, "source_id"),
       *version = field(accepted, "version");
  CHECK(json_parse(&j, accepted, strlen(accepted)));
  size_t captured_bytes = 0;
  CHECK(json_uint(&j, json_get(&j, 0, "bytes"), &captured_bytes) &&
        captured_bytes <= 65536);
  char read[1024];
  snprintf(read, sizeof(read),
           "{\"contract_version\":1,\"op\":\"read_text\",\"source_id\":\"%s\","
           "\"version\":\"%s\",\"start_byte\":0,\"end_byte\":%zu}",
           source, version, captured_bytes);
  body = call(&offline, read, NULL);
  CHECK(strstr(body, "cosine_bruteforce"));
  free(body);
  offline.owner = "bob";
  body = call(&offline, read, "unknown_source");
  free(body);
  body = call(&cfg,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[0,0]}",
              "invalid_semantic_vector");
  free(body);
  body = call(&cfg,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1]}",
              "invalid_semantic_vector");
  free(body);
  body = call(&cfg,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1e100,0]}",
              "invalid_semantic_vector");
  free(body);
  body = call(&cfg,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1,0],\"profile\":{}}",
              "unknown_field");
  free(body);
  body = call(&cfg,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[-1,0]}",
              NULL);
  CHECK(json_parse(&j, body, strlen(body)));
  items =
      json_get(&j, json_get(&j, json_get(&j, 0, "memory"), "result"), "items");
  CHECK(items >= 0 && j.tokens[items].count == 8);
  char *negative = slice(&j, json_get(&j, items + 1, "score"));
  CHECK(strtod(negative, NULL) < 0);
  free(negative);
  free(body);

  SourcesConfig missing = cfg;
  missing.chutni_semantic_profile = NULL;
  body = call(&missing,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1,0]}",
              "semantic_unconfigured");
  free(body);
  char *mismatch =
      replace(profile, "\"model_revision\":\"1\"", "\"model_revision\":\"2\"");
  missing = cfg;
  missing.chutni_semantic_profile = mismatch;
  body = call(&missing,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1,0]}",
              NULL);
  CHECK(json_parse(&j, body, strlen(body)));
  items =
      json_get(&j, json_get(&j, json_get(&j, 0, "memory"), "result"), "items");
  CHECK(items >= 0 && !j.tokens[items].count);
  free(body);
  free(mismatch);
  missing = cfg;
  missing.chutni_store_id = "01234567-89ab-cdef-0123-456789abcdef";
  body = call(&missing,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1,0]}",
              "chutni_store_changed");
  free(body);
  missing = cfg;
  missing.cancelled = cancelled;
  body = call(&missing,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1,0]}",
              "cancelled");
  free(body);
  missing = cfg;
  struct timespec now;
  CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
  missing.deadline_monotonic =
      (double)now.tv_sec + (double)now.tv_nsec / 1e9 - 1;
  body = call(&missing,
              "{\"contract_version\":1,\"op\":\"memory_search_semantic\","
              "\"vector\":[1,0]}",
              "deadline_exceeded");
  free(body);
  CHECK(!open_store(store_path, 1, &store));
  info = NULL;
  CHECK(!sdk_call(store, "store_info", "{}", &info));
  CHECK(json_parse(&j, info, strlen(info)));
  char *after = slice(&j, json_get(&j, 0, "counts"));
  CHECK(!strcmp(counts, after));
  free(after);
  sdk_free(info);
  close_store(store);
  CHECK(!dlclose(library));
  free(counts);
  free(id);
  free(first_artifact);
  free(first_source);
  free(source);
  free(version);
  free(accepted);
  printf("PASS actual SDK semantic retrieval: %u checks; fixed profile, cosine "
         "ranking, bounded read-only search, exact offline replay/owner, "
         "invalid vectors/profile drift/store pins and parent refusal; fixture "
         "representations, no embedding inference claim\n",
         checks);
  return 0;
}
