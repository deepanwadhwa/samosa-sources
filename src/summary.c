#define _POSIX_C_SOURCE 200809L
#include "cache.h"
#include "control.h"
#include "sha256.h"
#include "summary.h"
#include <time.h>
#define RESULT_MAX 65536u
#define SAMPLE_MAX 1400u
static int sha(const char *s) {
  if (!s || strlen(s) != 64)
    return 0;
  for (size_t i = 0; i < 64; i++)
    if (!strchr("0123456789abcdef", s[i]))
      return 0;
  return 1;
}
static int equal(const Json *j, int object, const char *name,
                 const char *expected) {
  char *s = json_string(j, json_get(j, object, name));
  int match = s && expected && !strcmp(s, expected);
  free(s);
  return match;
}
static int number(const Json *j, int object, const char *name,
                  size_t expected) {
  size_t n = 0;
  return json_uint(j, json_get(j, object, name), &n) && n == expected;
}
#define OPENING_MAX 60000u
typedef struct {
  size_t mapped, rounds;
  int truncated;
} SummaryStats;
static const char *statistics(const Json *j, int p, const char *text,
                              SummaryStats *s) {
  int b = json_get(j, p, "output_truncated");
  if (!text || !json_uint(j, json_get(j, p, "map_output_bytes"), &s->mapped) ||
      !s->mapped || s->mapped > 8192 ||
      !json_uint(j, json_get(j, p, "reduction_rounds"), &s->rounds) ||
      s->rounds > 4 || !number(j, p, "max_output_bytes", 1800) ||
      !number(j, p, "chunk_max_output_tokens", 64) ||
      !equal(j, p, "input_partitioning", "native_token_chunks") || b < 0 ||
      j->tokens[b].type != JT_BOOL)
    return "inference_response_invalid";
  s->truncated = j->tokens[b].end - j->tokens[b].begin == 4 &&
                 !memcmp(j->text + j->tokens[b].begin, "true", 4);
  size_t n = strlen(text);
  if (!n || n > 1800 || (s->rounds && s->mapped <= 1800) ||
      (!s->rounds && !s->truncated && n != s->mapped) ||
      (s->rounds && n >= s->mapped) ||
      (s->truncated && (n < 1797 || s->mapped <= 1800)))
    return "inference_response_invalid";
  return NULL;
}
static int configured(const SourcesConfig *cfg) {
  const SourcesInferenceConfig *i = cfg ? cfg->summary_inference : NULL;
  if (!i || !i->provider_id || !*i->provider_id ||
      strlen(i->provider_id) > 128 || !sha(i->engine_sha256) ||
      !sha(i->model_sha256))
    return 0;
  for (const unsigned char *p = (const unsigned char *)i->provider_id; *p; p++)
    if (*p > 127 || (!isalnum(*p) && !strchr("-_:.", *p)))
      return 0;
  return 1;
}
static void identity(const char *prefix, const char *const *parts, size_t count,
                     char out[65]) {
  Sha256 h;
  unsigned char bytes[32];
  sha256_init(&h);
  sha256_update(&h, prefix, strlen(prefix) + 1);
  for (size_t i = 0; i < count; i++)
    sha256_update(&h, parts[i], strlen(parts[i]) + 1);
  sha256_final(&h, bytes);
  sha256_hex(bytes, out);
}
static const char *recipe_mode(const SourcesConfig *cfg, char out[65],
                               int opening) {
  if (!configured(cfg))
    return "inference_unconfigured";
  const SourcesInferenceConfig *i = cfg->summary_inference;
  const char *parts[] = {
      i->provider_id, i->engine_sha256, i->model_sha256,
      opening
          ? "opening-60000-utf8-bytes:map-reduce:output-128:chunk-64:encoder-"
            "512:cap-1800:uncertain"
          : "opening-1400-utf8-bytes:scalar:output-128:encoder-512:uncertain"};
  identity(opening ? "samosa-native-summary-opening-map-reduce-v1"
                   : "samosa-native-summary-opening-chunk-v1",
           parts, 4, out);
  return NULL;
}
const char *sources_summary_recipe(const SourcesConfig *cfg, char out[65]) {
  return recipe_mode(cfg, out, 0);
}
const char *sources_summary_opening_recipe(const SourcesConfig *cfg,
                                           char out[65]) {
  return recipe_mode(cfg, out, 1);
}
/* Preserve the monolith opening-sample/chunk budget. The selected range may
 * already be partial; no claim is made about unseen pages or clipped tokens. */
static const char *sample(const Json *j, char *out, size_t limit,
                          size_t *bytes) {
  *bytes = 0;
  int text_token = json_get(j, 0, "text"), pages = json_get(j, 0, "pages");
  if (text_token >= 0 && pages >= 0)
    return "corrupt_cache";
  int first = text_token >= 0 ? text_token : pages + 1;
  if (text_token < 0 &&
      (pages < 0 || j->tokens[pages].type != JT_ARRAY ||
       j->tokens[pages].count < 1 || j->tokens[pages].count > 4))
    return "corrupt_cache";
  for (int t = first; text_token >= 0 ? t == first : t < j->tokens[pages].next;
       t = j->tokens[t].next) {
    char *text = json_string(j, text_token >= 0 ? t : json_get(j, t, "text"));
    if (!text)
      return "corrupt_cache";
    size_t n = strlen(text);
    if (n && *bytes && *bytes < limit)
      out[(*bytes)++] = '\n';
    size_t take = n;
    if (take > limit - *bytes)
      take = limit - *bytes;
    while (take < n && take && ((unsigned char)text[take] & 192) == 128)
      take--;
    memcpy(out + *bytes, text, take);
    *bytes += take;
    free(text);
    if (*bytes == limit || take < n || text_token >= 0)
      break;
  }
  out[*bytes] = 0;
  return *bytes ? NULL : "empty_evidence";
}
static const char *
project(const SourcesConfig *cfg, const Json *evidence, const char *source,
        const char *eid, const char *capture_sha, const char *input_sha,
        size_t input_bytes, const char *artifact, const char *text, int opening,
        const SummaryStats *stats, char **body, size_t *bytes) {
  if (!text || !*text || strlen(text) > 8192 ||
      !utf8_valid((const unsigned char *)text, strlen(text)))
    return "inference_response_invalid";
  int range = json_get(evidence, 0, "range"),
      coverage = json_get(evidence, 0, "coverage"),
      p = json_get(evidence, 0, "provenance");
  char *reader = json_string(evidence, json_get(evidence, p, "reader")),
       *fingerprint = json_string(
           evidence, json_get(evidence, p, "extractor_fingerprint"));
  if (range < 0 || coverage < 0 || evidence->tokens[range].type != JT_OBJECT ||
      evidence->tokens[coverage].type != JT_OBJECT || !reader ||
      !sha(fingerprint)) {
    free(reader);
    free(fingerprint);
    return "corrupt_cache";
  }
  FILE *out = open_memstream(body, bytes);
  if (!out) {
    free(reader);
    free(fingerprint);
    return "resource_limit";
  }
  const SourcesInferenceConfig *i = cfg->summary_inference;
  fprintf(out,
          "{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\","
          "\"artifact_id\":\"%s\",\"kind\":\"derived\",\"artifact_type\":"
          "\"summary\",\"text\":",
          source, source, artifact);
  json_quote(out, text, strlen(text));
  fputs(",\"coverage\":{\"status\":\"partial\",\"reason\":\"opening_sample_and_"
        "generated_summary_uncertain\"},\"input_selector\":",
        out);
  fwrite(evidence->text + evidence->tokens[range].begin, 1,
         evidence->tokens[range].end - evidence->tokens[range].begin, out);
  fputs(",\"input_coverage\":", out);
  fwrite(evidence->text + evidence->tokens[coverage].begin, 1,
         evidence->tokens[coverage].end - evidence->tokens[coverage].begin,
         out);
  fprintf(out,
          ",\"provenance\":{\"reader\":\"public-inference-summary-v1\","
          "\"recipe\":\"%s\",\"input_"
          "evidence_id\":\"%s\",\"input_evidence_sha256\":\"%s\",\"input_"
          "sha256\":\"%s\",\"sampled_bytes\":%zu,\"input_reader\":",
          opening ? "samosa-native-summary-opening-map-reduce-v1"
                  : "samosa-native-summary-opening-chunk-v1",
          eid, capture_sha, input_sha, input_bytes);
  json_quote(out, reader, strlen(reader));
  fprintf(out,
          ",\"extractor_fingerprint\":\"%s\",\"provider_id\":\"%s\",\"engine\":"
          "\"samosa-native-t5\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%"
          "s\",\"max_output_tokens\":128,\"encoder_context_tokens\":512,"
          "\"processed_input_tokens\":null",
          fingerprint, i->provider_id, i->engine_sha256, i->model_sha256);
  if (opening)
    fprintf(out,
            ",\"chunk_max_output_tokens\":64,\"input_partitioning\":\"native_"
            "token_chunks\",\"map_output_bytes\":%zu,\"reduction_rounds\":%zu,"
            "\"max_output_bytes\":1800,\"output_truncated\":%s",
            stats->mapped, stats->rounds, stats->truncated ? "true" : "false");
  fputs("}}\n", out);
  free(reader);
  free(fingerprint);
  return fclose(out) ? "resource_limit" : NULL;
}
static const char *provider_response(const SourcesConfig *cfg, const char *body,
                                     size_t bytes, const char *input_sha,
                                     size_t input_bytes, int opening,
                                     SummaryStats *stats, char **text) {
  if (!body || !bytes || bytes > RESULT_MAX || strlen(body) != bytes)
    return "inference_response_invalid";
  Json j;
  if (!json_parse(&j, body, bytes) || j.tokens[0].type != JT_OBJECT)
    return "inference_response_invalid";
  int error = json_get(&j, 0, "error");
  if (error >= 0) {
    char *code = json_string(&j, json_get(&j, error, "code"));
    const char *result = !code ? "inference_response_invalid"
                         : !strcmp(code, "cancelled") ? "cancelled"
                         : !strcmp(code, "deadline_exceeded")
                             ? "deadline_exceeded"
                         : !strcmp(code, "busy") || !strcmp(code, "queue_full")
                             ? "inference_busy"
                             : "inference_failed";
    free(code);
    return result;
  }
  int p = json_get(&j, 0, "provenance"), coverage = json_get(&j, 0, "coverage"),
      consumed = json_get(&j, p, "processed_input_tokens");
  const SourcesInferenceConfig *i = cfg->summary_inference;
  if (j.tokens[0].count != 9 || !number(&j, 0, "contract_version", 1) ||
      !equal(&j, 0, "provider_id", i->provider_id) ||
      !equal(&j, 0, "kind", "derived") ||
      !equal(&j, 0, "artifact_type", "summary") ||
      !equal(&j, 0, "input_sha256", input_sha) ||
      !number(&j, 0, "input_bytes", input_bytes) || p < 0 ||
      j.tokens[p].type != JT_OBJECT ||
      j.tokens[p].count != (opening ? 13 : 7) || coverage < 0 ||
      j.tokens[coverage].type != JT_OBJECT || j.tokens[coverage].count != 2 ||
      !equal(&j, coverage, "status", "partial") ||
      !equal(&j, coverage, "reason",
             opening ? "native_partitioning_and_generated_summary_uncertain"
                     : "bounded_encoder_and_generated_summary_uncertain") ||
      !equal(&j, p, "engine", "samosa-native-t5") ||
      !equal(&j, p, "engine_sha256", i->engine_sha256) ||
      !equal(&j, p, "model_sha256", i->model_sha256) ||
      !equal(&j, p, "recipe",
             opening ? "samosa-native-summary-opening-map-reduce-v1"
                     : "samosa-native-summary-scalar-v1") ||
      !number(&j, p, "max_output_tokens", 128) ||
      !number(&j, p, "encoder_context_tokens", 512) || consumed < 0 ||
      j.tokens[consumed].type != JT_NULL)
    return "inference_response_invalid";
  *text = json_string(&j, json_get(&j, 0, "text"));
  if (!*text || !**text || strlen(*text) > 8192) {
    free(*text);
    *text = NULL;
    return "inference_response_invalid";
  }
  if (opening) {
    const char *error = statistics(&j, p, *text, stats);
    if (error) {
      free(*text);
      *text = NULL;
      return error;
    }
  }
  return NULL;
}
static const char *cache_get(sqlite3 *db, const char *key, char **body,
                             size_t *bytes) {
  *body = NULL;
  *bytes = 0;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(
          db, "SELECT response,digest FROM prepared_artifacts WHERE key=?", -1,
          &stmt, NULL) != SQLITE_OK)
    return "store_failed";
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  const char *error = NULL;
  if (rc == SQLITE_ROW) {
    int n = sqlite3_column_bytes(stmt, 0);
    const char *data = (const char *)sqlite3_column_text(stmt, 0),
               *digest = (const char *)sqlite3_column_text(stmt, 1);
    char actual[65];
    if (!data || !digest || n <= 0 || (size_t)n > RESULT_MAX ||
        strlen(data) != (size_t)n)
      error = "corrupt_cache";
    else {
      hash_bytes(data, (size_t)n, actual);
      if (strcmp(actual, digest))
        error = "corrupt_cache";
      else {
        *body = strdup(data);
        if (!*body)
          error = "resource_limit";
        else
          *bytes = (size_t)n;
      }
    }
  } else if (rc != SQLITE_DONE)
    error = "store_failed";
  sqlite3_finalize(stmt);
  return error;
}
static int fail(FILE *out, const char *error) {
  fprintf(out, "{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n", error);
  return 1;
}
static int summary_dispatch(const SourcesConfig *cfg, const Json *request,
                            sqlite3 *db, FILE *out, int opening, const char *retained, size_t retained_bytes) {
  if (!configured(cfg))
    return fail(out, "inference_unconfigured");
  SourcesConfig scoped = *cfg;
  if (!scoped.deadline_monotonic) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
      return fail(out, "state_unavailable");
    scoped.deadline_monotonic =
        (double)now.tv_sec + (double)now.tv_nsec / 1e9 + 60;
  }
  cfg = &scoped;
  char *source = json_string(request, json_get(request, 0, "source_id")),
       *version = json_string(request, json_get(request, 0, "version")),
       *eid = json_string(request, json_get(request, 0, "evidence_id"));
  const char *error = !sha(source) || !sha(version) || !sha(eid)
                          ? "invalid_request"
                      : strcmp(source, version) ? "stale_source"
                                                : NULL;
  char *capture = NULL;
  size_t capture_bytes = 0;
  if (!error && retained) {
    error = sources_literal_retained_validate(db, cfg, retained, retained_bytes);
    if (!error) {
      capture = strndup(retained, retained_bytes);
      capture_bytes = retained_bytes;
      if (!capture) error = "resource_limit";
    }
  } else if (!error)
    error = sources_literal_capture(db, cfg, source, eid, &capture, &capture_bytes);
  Json evidence;
  SummaryStats stats = {0};
  char input[OPENING_MAX + 1], capture_sha[65], input_sha[65], recipe[65],
      artifact[65], key[65];
  size_t input_bytes = 0;
  if (!error && !json_parse(&evidence, capture, capture_bytes))
    error = "corrupt_cache";
  if (!error)
    error = sample(&evidence, input, opening ? OPENING_MAX : SAMPLE_MAX,
                   &input_bytes);
  if (!error) {
    hash_bytes(capture, capture_bytes, capture_sha);
    hash_bytes(input, input_bytes, input_sha);
    error = recipe_mode(cfg, recipe, opening);
  }
  if (!error) {
    const char *parts[] = {source, eid, capture_sha, input_sha, recipe};
    identity("sources-summary-artifact-v1", parts, 5, artifact);
    const char *owner = cfg->owner ? cfg->owner : "local-owner";
    const char *owned[] = {owner, artifact};
    identity("sources-summary-owner-cache-v1", owned, 2, key);
  }
  char *body = NULL;
  size_t body_bytes = 0;
  if (!error && !retained)
    error = cache_get(db, key, &body, &body_bytes);
  int cached = body != NULL;
  if (!error && cached) {
    Json j;
    char *text = NULL, *expected = NULL;
    size_t expected_bytes = 0;
    if (!json_parse(&j, body, body_bytes))
      error = "corrupt_cache";
    else
      text = json_string(&j, json_get(&j, 0, "text"));
    if (!error && opening)
      error = statistics(&j, json_get(&j, 0, "provenance"), text, &stats);
    if (!error)
      error = project(cfg, &evidence, source, eid, capture_sha, input_sha,
                      input_bytes, artifact, text, opening, &stats, &expected,
                      &expected_bytes);
    if (!error &&
        (expected_bytes != body_bytes || memcmp(expected, body, body_bytes)))
      error = "corrupt_cache";
    if (error && strcmp(error, "resource_limit"))
      error = "corrupt_cache";
    free(text);
    free(expected);
  }
  char *invocation = NULL, *response = NULL, *text = NULL;
  size_t invocation_bytes = 0, response_bytes = 0;
  const SourcesInferenceConfig *i = cfg->summary_inference;
  if (!error && !cached && !retained)
    error = sources_cache_capacity(db, cfg, 0);
  if (!error && !cached && !i->call)
    error = "inference_unavailable";
  if (!error && !cached) {
    FILE *f = open_memstream(&invocation, &invocation_bytes);
    if (!f)
      error = "resource_limit";
    else {
      fprintf(f,
              "{\"contract_version\":1,\"op\":\"%s\",\"provider_id\":\"%s\","
              "\"text\":",
              opening ? "summarize_opening" : "summarize", i->provider_id);
      json_quote(f, input, input_bytes);
      fputs(",\"max_output_tokens\":128,\"timeout_ms\":60000}", f);
      if (fclose(f))
        error = "resource_limit";
    }
  }
  if (!error && !cached) {
    error = sources_interrupted(cfg);
    SourcesInferenceControl control = {cfg->deadline_monotonic, cfg->cancelled,
                                       cfg->cancellation};
    if (!error)
      error = i->call(i->context, &control, invocation, invocation_bytes,
                      &response, &response_bytes);
    if (error && strcmp(error, "cancelled") &&
        strcmp(error, "deadline_exceeded"))
      error = "inference_unavailable";
    if (!error)
      error = sources_interrupted(cfg);
    if (!error)
      error = provider_response(cfg, response, response_bytes, input_sha,
                                input_bytes, opening, &stats, &text);
    if (!error)
      error = project(cfg, &evidence, source, eid, capture_sha, input_sha,
                      input_bytes, artifact, text, opening, &stats, &body,
                      &body_bytes);
  }
  if (!error && !cached && !retained) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
      error = "store_busy";
    else {
      char *old = NULL;
      size_t old_bytes = 0;
      error = cache_get(db, key, &old, &old_bytes);
      if (!error && old) {
        if (old_bytes != body_bytes || memcmp(old, body, body_bytes))
          error = "cache_conflict";
      } else if (!error) {
        error = sources_cache_capacity(db, cfg, body_bytes);
        sqlite3_stmt *stmt = NULL;
        if (!error && sqlite3_prepare_v2(
                          db, "INSERT INTO prepared_artifacts VALUES(?,?,?)",
                          -1, &stmt, NULL) != SQLITE_OK)
          error = "store_failed";
        if (!error) {
          char digest[65];
          hash_bytes(body, body_bytes, digest);
          sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt, 2, body, (int)body_bytes, SQLITE_TRANSIENT);
          sqlite3_bind_text(stmt, 3, digest, -1, SQLITE_TRANSIENT);
          if (sqlite3_step(stmt) != SQLITE_DONE)
            error = "store_failed";
        }
        sqlite3_finalize(stmt);
      }
      free(old);
      if (!error)
        error = sources_interrupted(cfg);
      if (error)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
      else if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        error = "store_failed";
      }
    }
  }
  if (!error)
    error = sources_interrupted(cfg);
  free(source);
  free(version);
  free(eid);
  free(capture);
  free(invocation);
  free(response);
  free(text);
  if (error) {
    free(body);
    return fail(out, error);
  }
  fwrite(body, 1, body_bytes, out);
  free(body);
  return 0;
}

int sources_summary_dispatch(const SourcesConfig *cfg, const Json *j,
                             sqlite3 *db, FILE *out) {
  return summary_dispatch(cfg, j, db, out, 0, NULL, 0);
}
int sources_summary_opening_dispatch(const SourcesConfig *cfg, const Json *j,
                                     sqlite3 *db, FILE *out) {
  return summary_dispatch(cfg, j, db, out, 1, NULL, 0);
}
/* Internal index path: caller retains the immutable literal envelope and owns
 * its queue admission. Avoid duplicating it in the public prepared cache. */
int sources_summary_opening_retained(const SourcesConfig *cfg, sqlite3 *db,
                                    const char *capture, size_t bytes, FILE *out) {
  Json j, request;
  if (!capture || !json_parse(&j, capture, bytes)) return fail(out,"corrupt_index");
  char *source=json_string(&j,json_get(&j,0,"source_id")),
       *eid=json_string(&j,json_get(&j,0,"evidence_id"));
  char body[320];int n=source&&eid?snprintf(body,sizeof(body),
      "{\"source_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"}",source,source,eid):-1;
  free(source);free(eid);
  if(n<0||(size_t)n>=sizeof(body)||!json_parse(&request,body,(size_t)n))return fail(out,"corrupt_index");
  return summary_dispatch(cfg,&request,db,out,1,capture,bytes);
}
const char *sources_summary_capture_validate(const SourcesConfig *cfg,
                                             const char *body, size_t bytes) {
  if (!configured(cfg) || !body || bytes > RESULT_MAX)
    return "corrupt_cache";
  Json j;
  if (!json_parse(&j, body, bytes) || j.tokens[0].type != JT_OBJECT)
    return "corrupt_cache";
  int p = json_get(&j, 0, "provenance"),
      range = json_get(&j, 0, "input_selector"),
      coverage = json_get(&j, 0, "input_coverage");
  char *source = json_string(&j, json_get(&j, 0, "source_id")),
       *eid = json_string(&j, json_get(&j, p, "input_evidence_id")),
       *capture_sha = json_string(&j, json_get(&j, p, "input_evidence_sha256")),
       *input_sha = json_string(&j, json_get(&j, p, "input_sha256")),
       *reader = json_string(&j, json_get(&j, p, "input_reader")),
       *fingerprint = json_string(&j, json_get(&j, p, "extractor_fingerprint")),
       *text = json_string(&j, json_get(&j, 0, "text"));
  int opening =
      equal(&j, p, "recipe", "samosa-native-summary-opening-map-reduce-v1");
  size_t sampled = 0, start = 0, end = 0;
  SummaryStats stats = {0};
  const char *error = NULL;
  int pdf = reader && (!strcmp(reader, "pdfium-text-v1") ||
                       !strcmp(reader, "pdfium-ocr-v1") || !strcmp(reader, "pdfium-index-fallback-v1"));
  int known = reader && (!strcmp(reader, "text-utf8-v1") ||
                         !strcmp(reader, "html-readable-v1") ||
                         !strcmp(reader, "docx-readable-v1") ||
                         !strcmp(reader, "image-ocr-v1") || pdf);
  if (!sha(source) || !sha(eid) || !sha(capture_sha) || !sha(input_sha) ||
      !sha(fingerprint) || !known || range < 0 || coverage < 0 ||
      !json_uint(&j, json_get(&j, p, "sampled_bytes"), &sampled) || !sampled ||
      sampled > (opening ? OPENING_MAX : SAMPLE_MAX) ||
      !json_uint(&j, json_get(&j, range, pdf ? "start_page" : "start_byte"),
                 &start) ||
      !json_uint(&j, json_get(&j, range, pdf ? "end_page" : "end_byte"),
                 &end) ||
      end < start ||
      (pdf ? (start < 1 || end - start >= 4) : end - start > SOURCES_READ_MAX))
    error = "corrupt_cache";
  if (!error) {
    char selector[256], expected[65];
    int n = snprintf(selector, sizeof(selector), "%s:%s:%s:%zu:%zu", reader,
                     fingerprint, source, start, end);
    if (n < 0 || (size_t)n >= sizeof(selector))
      error = "corrupt_cache";
    else {
      hash_bytes(selector, (size_t)n, expected);
      if (!strcmp(reader,"pdfium-index-fallback-v1") ? strcmp(fingerprint,eid) : strcmp(expected, eid))
        error = "corrupt_cache";
    }
  }
  if (!error && opening)
    error = statistics(&j, p, text, &stats);
  char *stub = NULL, *canonical = NULL;
  size_t stub_bytes = 0, canonical_bytes = 0;
  if (!error) {
    FILE *out = open_memstream(&stub, &stub_bytes);
    if (!out)
      error = "resource_limit";
    else {
      fputs("{\"range\":", out);
      fwrite(j.text + j.tokens[range].begin, 1,
             j.tokens[range].end - j.tokens[range].begin, out);
      fputs(",\"coverage\":", out);
      fwrite(j.text + j.tokens[coverage].begin, 1,
             j.tokens[coverage].end - j.tokens[coverage].begin, out);
      fputs(",\"provenance\":{\"reader\":", out);
      json_quote(out, reader, strlen(reader));
      fprintf(out, ",\"extractor_fingerprint\":\"%s\"}}", fingerprint);
      if (fclose(out))
        error = "resource_limit";
    }
  }
  if (!error) {
    Json evidence;
    char recipe[65], artifact[65];
    if (!json_parse(&evidence, stub, stub_bytes))
      error = "corrupt_cache";
    else {
      error = recipe_mode(cfg, recipe, opening);
      const char *parts[] = {source, eid, capture_sha, input_sha, recipe};
      if (!error) {
        identity("sources-summary-artifact-v1", parts, 5, artifact);
        error = project(cfg, &evidence, source, eid, capture_sha, input_sha,
                        sampled, artifact, text, opening, &stats, &canonical,
                        &canonical_bytes);
      }
    }
  }
  if (!error) {
    Json expected;
    if (!json_parse(&expected, canonical, canonical_bytes) ||
        j.tokens[0].count != expected.tokens[0].count)
      error = "corrupt_cache";
    else {
      /* Public worker extracts an envelope without its terminal newline. */
      size_t a = j.tokens[0].end - j.tokens[0].begin,
             b = expected.tokens[0].end - expected.tokens[0].begin;
      if (a != b || memcmp(j.text + j.tokens[0].begin,
                           expected.text + expected.tokens[0].begin, a))
        error = "corrupt_cache";
    }
  }
  free(source);
  free(eid);
  free(capture_sha);
  free(input_sha);
  free(reader);
  free(fingerprint);
  free(text);
  free(stub);
  free(canonical);
  return error && !strcmp(error, "resource_limit") ? error
         : error                                   ? "corrupt_cache"
                                                   : NULL;
}
