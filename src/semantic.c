#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "semantic.h"
#include "uploads.h"
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static int fields(const Json *j, int object, const char *const *names,
                  size_t count, unsigned required) {
  if (object < 0 || j->tokens[object].type != JT_OBJECT)
    return 0;
  unsigned seen = 0;
  for (int t = object + 1; t < j->tokens[object].next;
       t = j->tokens[j->tokens[t].next].next) {
    char *key = json_string(j, t);
    size_t k = 0;
    if (!key)
      return 0;
    for (; k < count; k++)
      if (!strcmp(key, names[k]))
        break;
    free(key);
    if (k == count || (seen & (1u << k)))
      return 0;
    seen |= 1u << k;
  }
  return (seen & required) == required;
}
static int number(const Json *j, int token, float *value) {
  if (token < 0 || j->tokens[token].type != JT_NUMBER)
    return 0;
  char text[128];
  size_t n = j->tokens[token].end - j->tokens[token].begin;
  if (!n || n >= sizeof(text))
    return 0;
  memcpy(text, j->text + j->tokens[token].begin, n);
  text[n] = 0;
  char *end;
  double x = strtod(text, &end);
  if (*end || !isfinite(x) || fabs(x) > FLT_MAX)
    return 0;
  *value = (float)x;
  return isfinite(*value);
}
const char *sources_semantic_vector_canonical(const Json *j, int vector,
                                              char **result) {
  *result = NULL;
  if (vector < 0 || j->tokens[vector].type != JT_ARRAY ||
      !j->tokens[vector].count || j->tokens[vector].count > 1024)
    return "invalid_semantic_vector";
  locale_t numeric = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric)
    return "resource_limit";
  locale_t previous = uselocale(numeric);
  size_t length = 0;
  FILE *out = open_memstream(result, &length);
  const char *error = out ? NULL : "resource_limit";
  int first = 1;
  if (out) {
    fputc('[', out);
    for (int t = vector + 1; t < j->tokens[vector].next;
         t = j->tokens[t].next) {
      float value;
      if (!number(j, t, &value)) {
        error = "invalid_semantic_vector";
        break;
      }
      fprintf(out, "%s%.9g", first ? "" : ",", (double)value);
      first = 0;
    }
    fputc(']', out);
    if (fclose(out))
      error = "resource_limit";
  }
  uselocale(previous);
  freelocale(numeric);
  if (error) {
    free(*result);
    *result = NULL;
  }
  return error;
}
const char *sources_semantic_arguments(const char *profile, const Json *request,
                                       int vector, char **result) {
  *result = NULL;
  if (!profile)
    return "semantic_unconfigured";
  locale_t numeric = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric)
    return "resource_limit";
  locale_t previous = uselocale(numeric);
  const char *names[] = {
      "representation_kind", "model_id",      "model_revision",
      "dimensions",          "dtype",         "normalization",
      "tokenizer_hash",      "projector_hash"};
  Json p;
  size_t dimensions = 0;
  char *values[8] = {0};
  const char *error = NULL;
  if (strlen(profile) > 4096 || !json_parse(&p, profile, strlen(profile)) ||
      !fields(&p, 0, names, 8, 63) ||
      !json_uint(&p, json_get(&p, 0, "dimensions"), &dimensions) ||
      !dimensions || dimensions > 1024)
    error = "invalid_semantic_configuration";
  for (size_t k = 0; !error && k < 8; k++) {
    if (k == 3)
      continue;
    int token = json_get(&p, 0, names[k]);
    if (k >= 6 && (token < 0 || p.tokens[token].type == JT_NULL))
      continue;
    values[k] = json_string(&p, token);
    if (!values[k] || !*values[k] || strlen(values[k]) > 256 ||
        !utf8_valid((const unsigned char *)values[k], strlen(values[k])))
      error = "invalid_semantic_configuration";
  }
  if (!error &&
      (strcmp(values[0], "text_embedding") || strcmp(values[4], "f32") ||
       (strcmp(values[5], "none") && strcmp(values[5], "l2"))))
    error = "invalid_semantic_configuration";
  if (!error && (vector < 0 || request->tokens[vector].type != JT_ARRAY ||
                 (size_t)request->tokens[vector].count != dimensions))
    error = "invalid_semantic_vector";
  float values_vector[1024];
  double norm = 0;
  size_t count = 0;
  if (!error)
    for (int t = vector + 1; t < request->tokens[vector].next;
         t = request->tokens[t].next) {
      if (count == dimensions || !number(request, t, &values_vector[count])) {
        error = "invalid_semantic_vector";
        break;
      }
      double x = values_vector[count++];
      norm += x * x;
    }
  if (!error && (count != dimensions || !isfinite(norm) || !(norm > 0) ||
                 (!strcmp(values[5], "l2") && fabs(norm - 1) > 0.001)))
    error = "invalid_semantic_vector";
  if (!error) {
    size_t length = 0;
    FILE *out = open_memstream(result, &length);
    if (!out)
      error = "resource_limit";
    else {
      fputs("{\"profile\":{", out);
      for (size_t k = 0; k < 8; k++) {
        if (k)
          fputc(',', out);
        json_quote(out, names[k], strlen(names[k]));
        fputc(':', out);
        if (k == 3)
          fprintf(out, "%zu", dimensions);
        else if (values[k])
          json_quote(out, values[k], strlen(values[k]));
        else
          fputs("null", out);
      }
      fputs("},\"vector\":[", out);
      for (size_t k = 0; k < count; k++)
        fprintf(out, "%s%.9g", k ? "," : "", (double)values_vector[k]);
      fputs("],\"limit\":8,\"include_stale\":false}", out);
      if (fclose(out))
        error = "resource_limit";
      else if (length > 16384)
        error = "invalid_semantic_vector";
    }
  }
  for (size_t k = 0; k < 8; k++)
    free(values[k]);
  uselocale(previous);
  freelocale(numeric);
  if (error) {
    free(*result);
    *result = NULL;
  }
  return error;
}
const char *sources_semantic_arguments_validate(const char *arguments) {
  Json j;
  const char *names[] = {"profile", "vector", "limit", "include_stale"};
  size_t limit = 0;
  if (!arguments || strlen(arguments) > 16384 ||
      !json_parse(&j, arguments, strlen(arguments)) ||
      !fields(&j, 0, names, 4, 15) ||
      !json_uint(&j, json_get(&j, 0, "limit"), &limit) || limit != 8)
    return "invalid_request";
  int stale = json_get(&j, 0, "include_stale"), p = json_get(&j, 0, "profile");
  if (stale < 0 || j.tokens[stale].type != JT_BOOL ||
      j.tokens[stale].end - j.tokens[stale].begin != 5 ||
      memcmp(j.text + j.tokens[stale].begin, "false", 5) || p < 0)
    return "invalid_request";
  char *profile = strndup(j.text + j.tokens[p].begin,
                          j.tokens[p].end - j.tokens[p].begin),
       *canonical = NULL;
  if (!profile)
    return "resource_limit";
  const char *error = sources_semantic_arguments(
      profile, &j, json_get(&j, 0, "vector"), &canonical);
  if (error && strcmp(error, "resource_limit"))
    error = "invalid_request";
  if (!error && strcmp(arguments, canonical))
    error = "invalid_request";
  free(profile);
  free(canonical);
  return error;
}
