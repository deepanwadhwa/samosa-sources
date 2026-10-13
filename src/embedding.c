#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 8192
#include "embedding.h"
#include "control.h"
#include "semantic.h"
#include "sha256.h"
#include "uploads.h"

static int digest(const char *s) {
  if (!s || strlen(s) != 64)
    return 0;
  for (size_t k = 0; k < 64; k++)
    if (!strchr("0123456789abcdef", s[k]))
      return 0;
  return 1;
}
static int equal(const Json *j, int object, const char *key,
                 const char *expected) {
  char *value = json_string(j, json_get(j, object, key));
  int good = value && expected && !strcmp(value, expected);
  free(value);
  return good;
}
static int number(const Json *j, int object, const char *key, size_t expected) {
  size_t value = 0;
  return json_uint(j, json_get(j, object, key), &value) && value == expected;
}
static int fields(const Json *j, int object, const char *const *names,
                  size_t count) {
  if (object < 0 || j->tokens[object].type != JT_OBJECT ||
      (size_t)j->tokens[object].count != count)
    return 0;
  unsigned seen = 0;
  for (int t = object + 1; t < j->tokens[object].next;
       t = j->tokens[j->tokens[t].next].next) {
    char *key = json_string(j, t);
    size_t k = 0;
    for (; key && k < count; k++)
      if (!strcmp(key, names[k]))
        break;
    int good = key && k < count && !(seen & (1u << k));
    free(key);
    if (!good)
      return 0;
    seen |= 1u << k;
  }
  return 1;
}
static char *slice(const Json *j, int token) {
  return token < 0 ? NULL
                   : strndup(j->text + j->tokens[token].begin,
                             j->tokens[token].end - j->tokens[token].begin);
}
void sources_embedding_config_free(SourcesEmbeddingConfig *e) {
  free((void *)e->inference.provider_id);
  free((void *)e->inference.engine_sha256);
  free((void *)e->inference.model_sha256);
  free((void *)e->recipe);
  free((void *)e->query_prefix);
  free((void *)e->document_prefix);
  memset(e, 0, sizeof(*e));
}
int sources_embedding_config_parse(const char *body, size_t bytes,
                                   SourcesEmbeddingConfig *e) {
  memset(e, 0, sizeof(*e));
  Json j;
  const char *names[] = {"provider_id",   "engine_sha256", "model_sha256",
                         "recipe",        "query_prefix",  "document_prefix",
                         "context_tokens"};
  if (!body || !bytes || bytes > 4096 || !json_parse(&j, body, bytes) ||
      !fields(&j, 0, names, 7))
    return 0;
  e->inference.provider_id = json_string(&j, json_get(&j, 0, names[0]));
  e->inference.engine_sha256 = json_string(&j, json_get(&j, 0, names[1]));
  e->inference.model_sha256 = json_string(&j, json_get(&j, 0, names[2]));
  e->recipe = json_string(&j, json_get(&j, 0, names[3]));
  e->query_prefix = json_string(&j, json_get(&j, 0, names[4]));
  e->document_prefix = json_string(&j, json_get(&j, 0, names[5]));
  if (!e->inference.provider_id || !e->inference.engine_sha256 ||
      !e->inference.model_sha256 || !e->recipe || !e->query_prefix ||
      !e->document_prefix ||
      !json_uint(&j, json_get(&j, 0, names[6]), &e->context_tokens)) {
    sources_embedding_config_free(e);
    return 0;
  }
  return 1;
}
/* Use the same profile parser/normalizer as Chutni query dispatch. A temporary
 * unit vector validates configuration without model execution or ranking. */
static const char *profile(const SourcesConfig *cfg, char **result) {
  *result = NULL;
  Json p, unit;
  size_t dimensions = 0;
  const char *text = cfg ? cfg->chutni_semantic_profile : NULL;
  if (!text)
    return "semantic_unconfigured";
  if (strlen(text) > 4096 || !json_parse(&p, text, strlen(text)) ||
      !json_uint(&p, json_get(&p, 0, "dimensions"), &dimensions) ||
      !dimensions || dimensions > 1024)
    return "invalid_semantic_configuration";
  if (!equal(&p, 0, "normalization", "l2"))
    return "invalid_embedding_configuration";
  char vector[2050];
  size_t n = 0;
  vector[n++] = '[';
  vector[n++] = '1';
  for (size_t k = 1; k < dimensions; k++) {
    vector[n++] = ',';
    vector[n++] = '0';
  }
  vector[n++] = ']';
  vector[n] = 0;
  if (!json_parse(&unit, vector, n))
    return "invalid_semantic_configuration";
  char *arguments = NULL;
  const char *error = sources_semantic_arguments(text, &unit, 0, &arguments);
  if (!error) {
    Json a;
    if (!json_parse(&a, arguments, strlen(arguments)))
      error = "invalid_semantic_configuration";
    else if (!(*result = slice(&a, json_get(&a, 0, "profile"))))
      error = "resource_limit";
  }
  free(arguments);
  return error;
}
const char *sources_embedding_identity(const SourcesConfig *cfg,
                                       char identity[65]) {
  const SourcesEmbeddingConfig *e = cfg ? cfg->embedding : NULL;
  if (!e)
    return "embedding_unconfigured";
  const SourcesInferenceConfig *i = &e->inference;
  if (!i->provider_id || !*i->provider_id || strlen(i->provider_id) > 128 ||
      !digest(i->engine_sha256) || !digest(i->model_sha256) || !e->recipe ||
      !*e->recipe || strlen(e->recipe) > 256 || !e->query_prefix ||
      strlen(e->query_prefix) > 256 || !e->document_prefix ||
      strlen(e->document_prefix) > 256 || e->context_tokens < 128 ||
      e->context_tokens > 131072)
    return "invalid_embedding_configuration";
  for (const unsigned char *p = (const unsigned char *)i->provider_id; *p; p++)
    if (*p > 127 || (!isalnum(*p) && !strchr("-_:.", *p)))
      return "invalid_embedding_configuration";
  const char *strings[] = {e->recipe, e->query_prefix, e->document_prefix};
  for (size_t k = 0; k < 3; k++)
    if (!utf8_valid((const unsigned char *)strings[k], strlen(strings[k])))
      return "invalid_embedding_configuration";
  char *canonical = NULL;
  const char *error = profile(cfg, &canonical);
  if (error)
    return error;
  const char *parts[] = {"sources-public-embedding-v1",
                         i->provider_id,
                         i->engine_sha256,
                         i->model_sha256,
                         e->recipe,
                         e->query_prefix,
                         e->document_prefix,
                         canonical,
                         cfg->owner ? cfg->owner : "local-owner"};
  Sha256 hash;
  sha256_init(&hash);
  for (size_t k = 0; k < sizeof(parts) / sizeof(parts[0]); k++)
    sha256_update(&hash, parts[k], strlen(parts[k]) + 1);
  char context[32];
  snprintf(context, sizeof(context), "%zu", e->context_tokens);
  sha256_update(&hash, context, strlen(context) + 1);
  unsigned char sum[32];
  sha256_final(&hash, sum);
  sha256_hex(sum, identity);
  free(canonical);
  return NULL;
}
static const char *provider_error(const Json *j, int token) {
  char *code = json_string(j, json_get(j, token, "code"));
  const char *error =
      !code                                ? "inference_response_invalid"
      : !strcmp(code, "cancelled")         ? "cancelled"
      : !strcmp(code, "deadline_exceeded") ? "deadline_exceeded"
      : !strcmp(code, "busy") || !strcmp(code, "queue_full") ? "inference_busy"
      : !strcmp(code, "context_limit") ? "embedding_context_limit"
                                       : "inference_failed";
  free(code);
  return error;
}
static const char *embedding_execute(const SourcesConfig *cfg,
                                       const char *text, const char *role,
                                       const char *title, char **response,
                                       size_t *bytes,const char *saved,size_t saved_bytes) {
  *response = NULL;
  *bytes = 0;
  char binding[65];
  const char *error = sources_embedding_identity(cfg, binding);
  if (error)
    return error;
  if (!text || !*text || strlen(text) > 60000 ||
      !utf8_valid((const unsigned char *)text, strlen(text)) || !role ||
      (strcmp(role, "query") && strcmp(role, "document")) ||
      (title && (strcmp(role, "document") || strlen(title) > 1024 ||
                 !utf8_valid((const unsigned char *)title, strlen(title)))))
    return "invalid_request";
  const SourcesEmbeddingConfig *e = cfg->embedding;
  const SourcesInferenceConfig *i = &e->inference;
  char *prepared = NULL, *request = NULL;
  size_t prepared_bytes = 0, request_bytes = 0;
  FILE *out = open_memstream(&prepared, &prepared_bytes);
  if (!out)
    return "resource_limit";
  fputs(!strcmp(role, "query") ? e->query_prefix : e->document_prefix, out);
  if (!strcmp(role, "document"))
    fprintf(out, "title: %s | text: ", title ? title : "none");
  fputs(text, out);
  if (fclose(out)) {
    free(prepared);
    return "resource_limit";
  }
  char input_sha[65], prepared_sha[65], operation[80], id[65];
  hash_bytes(text, strlen(text), input_sha);
  hash_bytes(prepared, prepared_bytes, prepared_sha);
  Sha256 h;
  sha256_init(&h);
  const char *parts[] = {binding, role, input_sha, prepared_sha};
  for (size_t k = 0; k < 4; k++)
    sha256_update(&h, parts[k], strlen(parts[k]) + 1);
  unsigned char sum[32];
  sha256_final(&h, sum);
  sha256_hex(sum, id);
  snprintf(operation, sizeof(operation), "embed-%s", id);
  out = open_memstream(&request, &request_bytes);
  if (!out)
    error = "resource_limit";
  else {
    fprintf(out,
            "{\"contract_version\":1,\"op\":\"embed_text\",\"operation_id\":\"%"
            "s\",\"text\":",
            operation);
    json_quote(out, text, strlen(text));
    fputs(",\"role\":", out);
    json_quote(out, role, strlen(role));
    if (title) {
      fputs(",\"title\":", out);
      json_quote(out, title, strlen(title));
    }
    fputs(",\"timeout_ms\":60000}", out);
    if (fclose(out))
      error = "resource_limit";
  }
  if (!error)
    error = sources_interrupted(cfg);
  if (!error && !saved && !i->call)
    error = "inference_unavailable";
  if (!error && saved) {
    if(!saved_bytes||saved_bytes>65536||strlen(saved)!=saved_bytes)error="inference_response_invalid";
    else { *response=strdup(saved);if(!*response)error="resource_limit";else *bytes=saved_bytes; }
  } else if (!error) {
    SourcesInferenceControl control = {cfg->deadline_monotonic, cfg->cancelled,
                                       cfg->cancellation};
    error =
        i->call(i->context, &control, request, request_bytes, response, bytes);
    if (error && strcmp(error, "cancelled") &&
        strcmp(error, "deadline_exceeded"))
      error = "inference_unavailable";
  }
  if (!error)
    error = sources_interrupted(cfg);
  Json reply;
  size_t tokens = 0;
  if (!error &&
      (!*response || !*bytes || *bytes > 65536 || strlen(*response) != *bytes ||
       !json_parse(&reply, *response, *bytes) ||
       reply.tokens[0].type != JT_OBJECT))
    error = "inference_response_invalid";
  if (!error && json_get(&reply, 0, "error") >= 0)
    error = provider_error(&reply, json_get(&reply, 0, "error"));
  const char *names[] = {"contract_version",
                         "kind",
                         "artifact_type",
                         "provider_id",
                         "input_sha256",
                         "input_bytes",
                         "input_tokens",
                         "role",
                         "title",
                         "prepared_input_sha256",
                         "prepared_input_bytes",
                         "profile",
                         "vector",
                         "provenance",
                         "operation_id"};
  const char *pins[] = {"model_sha256", "engine_sha256", "recipe"};
  if (!error) {
    int provenance = json_get(&reply, 0, "provenance"),
        returned_title = json_get(&reply, 0, "title");
    if (!fields(&reply, 0, names, 15) ||
        !number(&reply, 0, "contract_version", 1) ||
        !equal(&reply, 0, "kind", "derived") ||
        !equal(&reply, 0, "artifact_type", "embedding") ||
        !equal(&reply, 0, "provider_id", i->provider_id) ||
        !equal(&reply, 0, "input_sha256", input_sha) ||
        !number(&reply, 0, "input_bytes", strlen(text)) ||
        !json_uint(&reply, json_get(&reply, 0, "input_tokens"), &tokens) ||
        !tokens || tokens > e->context_tokens ||
        !equal(&reply, 0, "role", role) ||
        (title ? !equal(&reply, 0, "title", title)
               : returned_title < 0 ||
                     reply.tokens[returned_title].type != JT_NULL) ||
        !equal(&reply, 0, "prepared_input_sha256", prepared_sha) ||
        !number(&reply, 0, "prepared_input_bytes", prepared_bytes) ||
        !equal(&reply, 0, "operation_id", operation) ||
        !fields(&reply, provenance, pins, 3) ||
        !equal(&reply, provenance, "model_sha256", i->model_sha256) ||
        !equal(&reply, provenance, "engine_sha256", i->engine_sha256) ||
        !equal(&reply, provenance, "recipe", e->recipe))
      error = "inference_response_invalid";
  }
  char *expected = NULL, *actual = NULL, *actual_profile = NULL;
  if (!error) {
    actual_profile = slice(&reply, json_get(&reply, 0, "profile"));
    if (!actual_profile)
      error = "inference_response_invalid";
    else if (sources_semantic_arguments(cfg->chutni_semantic_profile, &reply,
                                        json_get(&reply, 0, "vector"),
                                        &expected) ||
             sources_semantic_arguments(actual_profile, &reply,
                                        json_get(&reply, 0, "vector"), &actual))
      error = "inference_response_invalid";
    else if (strcmp(expected, actual))
      error = "inference_profile_mismatch";
  }
  free(expected);
  free(actual);
  free(actual_profile);
  free(prepared);
  free(request);
  if (error) {
    free(*response);
    *response = NULL;
    *bytes = 0;
  }
  return error;
}
const char *sources_embedding_generate(const SourcesConfig *cfg,const char *text,const char *role,const char *title,char **response,size_t *bytes){
  return embedding_execute(cfg,text,role,title,response,bytes,NULL,0);
}
const char *sources_embedding_capture_arguments(const SourcesConfig *cfg,const char *text,const char *title,const char *capture,size_t bytes,char **arguments){
  *arguments=NULL;if(!capture)return "inference_response_invalid";
  char *validated=NULL;size_t length=0;
  const char *error=embedding_execute(cfg,text,"document",title,&validated,&length,capture,bytes);
  if(!error){Json j;if(!json_parse(&j,validated,length))error="inference_response_invalid";
    else error=sources_semantic_arguments(cfg->chutni_semantic_profile,&j,json_get(&j,0,"vector"),arguments);
  }
  free(validated);if(error){free(*arguments);*arguments=NULL;}return error;
}
const char *sources_embedding_search_arguments(const SourcesConfig *cfg,
                                               const char *query,
                                               char **arguments,
                                               char **response, size_t *bytes) {
  *arguments = NULL;
  *response = NULL;
  *bytes = 0;
  if (!query || !*query || strlen(query) > 512)
    return "invalid_request";
  const char *error =
      sources_embedding_generate(cfg, query, "query", NULL, response, bytes);
  if (!error) {
    Json j;
    if (!json_parse(&j, *response, *bytes))
      error = "inference_response_invalid";
    else
      error = sources_semantic_arguments(cfg->chutni_semantic_profile, &j,
                                         json_get(&j, 0, "vector"), arguments);
  }
  if (error) {
    free(*arguments);
    *arguments = NULL;
    free(*response);
    *response = NULL;
    *bytes = 0;
  }
  return error;
}
