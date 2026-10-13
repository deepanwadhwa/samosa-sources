#define _POSIX_C_SOURCE 200809L
#include "cache.h"
#include "chutni.h"
#include "control.h"
#include "enrichment.h"
#include "index.h"
#include "inventory.h"
#include "sha256.h"
#include "summary.h"
#include <time.h>
#include <unistd.h>
static int fail(FILE *out, const char *error) {
  fprintf(out, "{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n", error);
  return 1;
}
static int sha(const char *s) {
  if (!s || strlen(s) != 64)
    return 0;
  for (size_t i = 0; i < 64; i++)
    if (!strchr("0123456789abcdef", s[i]))
      return 0;
  return 1;
}
static int sql(sqlite3 *db, const char *s) {
  return sqlite3_exec(db, s, NULL, NULL, NULL) == SQLITE_OK;
}
static int prepare(sqlite3 *db, sqlite3_stmt **s, const char *query) {
  return sqlite3_prepare_v2(db, query, -1, s, NULL) == SQLITE_OK;
}
static void text(sqlite3_stmt *s, int i, const char *v) {
  sqlite3_bind_text(s, i, v, -1, SQLITE_TRANSIENT);
}
static char *column(sqlite3_stmt *s, int i) {
  const char *v = (const char *)sqlite3_column_text(s, i);
  return sqlite3_column_type(s, i) == SQLITE_TEXT && v &&
                 strlen(v) == (size_t)sqlite3_column_bytes(s, i)
             ? strdup(v)
             : NULL;
}
static sqlite3_int64 now_ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_REALTIME, &t) || t.tv_sec < 0 ||
      t.tv_sec > INT64_MAX / 1000 - 61)
    return -1;
  return (sqlite3_int64)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static const char *schema(sqlite3 *db) {
  return sql(db,
             "CREATE TABLE IF NOT EXISTS index_enrichments(key TEXT PRIMARY "
             "KEY,owner TEXT NOT NULL,scope TEXT NOT NULL,job TEXT NOT "
             "NULL,ordinal INTEGER NOT NULL,generation INTEGER NOT NULL,recipe "
             "TEXT NOT NULL,store TEXT NOT NULL,evidence_digest TEXT NOT "
             "NULL,status TEXT NOT NULL,capture TEXT,capture_digest "
             "TEXT,receipt TEXT,receipt_digest TEXT,next_ms INTEGER NOT NULL "
             "DEFAULT 0,failures INTEGER NOT NULL DEFAULT 0,turn INTEGER NOT "
             "NULL DEFAULT 0,last_error TEXT)")
             ? NULL
             : "store_failed";
}
static int equal(const Json *j, int object, const char *key,
                 const char *value) {
  char *s = json_string(j, json_get(j, object, key));
  int same = s && value && !strcmp(s, value);
  free(s);
  return same;
}
static const char *response_error(const char *body, size_t bytes,
                                  char code[65]) {
  Json j;
  char *s = NULL;
  if (body && json_parse(&j, body, bytes))
    s = json_string(&j, json_get(&j, json_get(&j, 0, "error"), "code"));
  int valid = s && *s && strlen(s) <= 64;
  for (size_t i = 0; valid && s[i]; i++)
    if (!strchr("abcdefghijklmnopqrstuvwxyz0123456789_", s[i]))
      valid = 0;
  if (valid)
    memcpy(code, s, strlen(s) + 1);
  free(s);
  return valid ? code : "invalid_response";
}
static void identity(const SourcesConfig *cfg, const char *scope,
                     const char *job, sqlite3_int64 ordinal,
                     sqlite3_int64 generation, const char *recipe,
                     const char *store, const char *digest, char key[65]) {
  Sha256 h;
  unsigned char bytes[32];
  char coordinates[64];
  snprintf(coordinates, sizeof(coordinates), "%lld:%lld", (long long)ordinal,
           (long long)generation);
  const char *parts[] = {"sources-index-enrichment-v1",
                         cfg->owner ? cfg->owner : "local-owner",
                         scope,
                         job,
                         coordinates,
                         recipe,
                         store,
                         digest};
  sha256_init(&h);
  for (size_t i = 0; i < 8; i++)
    sha256_update(&h, parts[i], strlen(parts[i]) + 1);
  sha256_final(&h, bytes);
  sha256_hex(bytes, key);
}
static const char *digest_check(const char *body, const char *proof) {
  char actual[65];
  if (!body || !sha(proof))
    return "corrupt_enrichment";
  hash_bytes(body, strlen(body), actual);
  return strcmp(actual, proof) ? "corrupt_enrichment" : NULL;
}
static const char *bind_capture(const SourcesConfig *cfg, const char *literal,
                                const char *capture) {
  const char *error =
      sources_summary_capture_validate(cfg, capture, strlen(capture));
  if (error)
    return error;
  Json l, c;
  char digest[65];
  if (!json_parse(&l, literal, strlen(literal)) ||
      !json_parse(&c, capture, strlen(capture)))
    return "corrupt_enrichment";
  int p = json_get(&c, 0, "provenance");
  char *source = json_string(&l, json_get(&l, 0, "source_id")),
       *eid = json_string(&l, json_get(&l, 0, "evidence_id"));
  hash_bytes(literal, strlen(literal), digest);
  if (!equal(&c, 0, "source_id", source) ||
      !equal(&c, p, "input_evidence_id", eid) ||
      !equal(&c, p, "input_evidence_sha256", digest))
    error = "corrupt_enrichment";
  free(source);
  free(eid);
  return error;
}
static const char *saved_capture(const SourcesConfig *cfg, const char *scope,
                                 const char *store, const char *recipe,
                                 const char *evidence_digest,
                                 const char *capture, const char *receipt) {
  Json j;
  if (!json_parse(&j, capture, strlen(capture)))
    return "corrupt_enrichment";
  int p = json_get(&j, 0, "provenance");
  char *provider = json_string(&j, json_get(&j, p, "provider_id")),
       *engine = json_string(&j, json_get(&j, p, "engine_sha256")),
       *model = json_string(&j, json_get(&j, p, "model_sha256")),
       *source = json_string(&j, json_get(&j, 0, "source_id")),
       *artifact = json_string(&j, json_get(&j, 0, "artifact_id"));
  SourcesInferenceConfig id = {
      .provider_id = provider, .engine_sha256 = engine, .model_sha256 = model};
  SourcesConfig pinned = *cfg;
  pinned.summary_inference = &id;
  char expected[65];
  const char *error =
      sources_summary_capture_validate(&pinned, capture, strlen(capture));
  if (!error)
    error = sources_summary_opening_recipe(&pinned, expected);
  if (!error && (strcmp(expected, recipe) ||
                 !equal(&j, p, "input_evidence_sha256", evidence_digest)))
    error = "corrupt_enrichment";
  if (!error && receipt) {
    Json r;
    size_t version = 0, items = 0;
    int publication = -1;
    if (strlen(receipt) > 8192 || !json_parse(&r, receipt, strlen(receipt)) ||
        r.tokens[0].count != 4 || !equal(&r, 0, "receipt_state", "committed") ||
        !json_uint(&r, json_get(&r, 0, "contract_version"), &version) ||
        version != 1 ||
        !json_uint(&r, json_get(&r, 0, "published_items"), &items) ||
        items != 1 || (publication = json_get(&r, 0, "publication")) < 0)
      error = "corrupt_enrichment";
    else {
      char capture_digest[65], key[65], material[512], metadata[600];
      hash_bytes(capture + j.tokens[0].begin,
                 j.tokens[0].end - j.tokens[0].begin, capture_digest);
      int n = snprintf(material, sizeof(material),
                       "chutni-summary-publication-v1:%s:%s:%s:%s:%s",
                       cfg->owner ? cfg->owner : "local-owner", scope, store,
                       source, capture_digest);
      if (n < 0 || (size_t)n >= sizeof(material))
        error = "corrupt_enrichment";
      else {
        hash_bytes(material, (size_t)n, key);
        snprintf(
            metadata, sizeof(metadata),
            "{\"store_id\":\"%s\",\"publication_id\":\"%s\",\"version\":\"%s\","
            "\"evidence_id\":\"%s\",\"evidence\":{\"kind\":\"derived\"}}",
            store, key, source, artifact);
        char *body =
            strndup(r.text + r.tokens[publication].begin,
                    r.tokens[publication].end - r.tokens[publication].begin);
        if (!body)
          error = "resource_limit";
        else {
          if (sources_chutni_publish_validate(metadata, body, strlen(body)))
            error = "corrupt_enrichment";
          free(body);
        }
      }
    }
  }
  free(provider);
  free(engine);
  free(model);
  free(source);
  free(artifact);
  return error && !strcmp(error, "resource_limit") ? error
         : error                                   ? "corrupt_enrichment"
                                                   : NULL;
}
static const char *save(sqlite3 *db, const char *key, const char *status,
                        const char *body, int receipt, sqlite3_int64 time,
                        const char *error, int failures) {
  if (!sql(db, "BEGIN IMMEDIATE"))
    return "store_busy";
  sqlite3_stmt *s = NULL;
  const char *failure = NULL;
  const char *query =
      error ? "UPDATE index_enrichments SET next_ms=?,failures=?,last_error=? "
              "WHERE key=?"
      : receipt ? "UPDATE index_enrichments SET "
                  "status=?,receipt=?,receipt_digest=?,next_ms=0,failures=0,"
                  "last_error=NULL WHERE key=? AND status='captured'"
                : "UPDATE index_enrichments SET "
                  "status=?,capture=?,capture_digest=?,next_ms=0,failures=0,"
                  "last_error=NULL WHERE key=? AND status='pending'";
  if (!prepare(db, &s, query))
    failure = "store_failed";
  if (!failure) {
    if (error) {
      int level = failures < 7 ? failures + 1 : 7;
      sqlite3_int64 delay = 1000ll << level;
      if (delay > 60000)
        delay = 60000;
      text(s, 3, error);
      sqlite3_bind_int64(s, 1, time + delay);
      sqlite3_bind_int(s, 2, level);
    } else {
      char digest[65];
      text(s, 1, status);
      if (body) {
        hash_bytes(body, strlen(body), digest);
        text(s, 2, body);
        text(s, 3, digest);
      } else {
        sqlite3_bind_null(s, 2);
        sqlite3_bind_null(s, 3);
      }
    }
    text(s, 4, key);
    if (sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(db) != 1)
      failure = "corrupt_enrichment";
  }
  sqlite3_finalize(s);
  if (failure)
    sql(db, "ROLLBACK");
  else if (!sql(db, "COMMIT")) {
    sql(db, "ROLLBACK");
    failure = "store_failed";
  }
  return failure;
}
static const char *validate_rows(sqlite3 *db, const SourcesConfig *cfg,
                                 const char *scope) {
  sqlite3_stmt *s = NULL;
  if (!prepare(
          db, &s,
          "SELECT "
          "key,job,ordinal,generation,recipe,store,evidence_digest,status,"
          "capture,capture_digest,receipt,receipt_digest,next_ms,failures,turn,"
          "last_error FROM index_enrichments WHERE owner=? AND scope=?"))
    return "store_failed";
  text(s, 1, cfg->owner ? cfg->owner : "local-owner");
  text(s, 2, scope);
  const char *error = NULL;
  size_t rows = 0, bytes = 0;
  while (!error) {
    int rc = sqlite3_step(s);
    if (rc == SQLITE_DONE)
      break;
    if (rc != SQLITE_ROW) {
      error = "store_failed";
      break;
    }
    char *key = column(s, 0), *job = column(s, 1), *recipe = column(s, 4),
         *store = column(s, 5), *digest = column(s, 6), *status = column(s, 7),
         *capture = column(s, 8), *proof = column(s, 9),
         *receipt = column(s, 10), *rproof = column(s, 11),
         *last = column(s, 15);
    sqlite3_int64 ordinal = sqlite3_column_int64(s, 2),
                  generation = sqlite3_column_int64(s, 3);
    char actual[65];
    if (++rows > 4096 || !sha(key) || !sha(job) || !sha(recipe) ||
        !sha(digest) || !store || strlen(store) > 36 || !status ||
        ordinal < 0 || ordinal > 255 || generation < 0 || generation > 256)
      error = "corrupt_enrichment";
    const int integers[] = {2, 3, 12, 13, 14};
    for (size_t i = 0; !error && i < 5; i++)
      if (sqlite3_column_type(s, integers[i]) != SQLITE_INTEGER ||
          sqlite3_column_int64(s, integers[i]) < 0)
        error = "corrupt_enrichment";
    if (!error && (sqlite3_column_int64(s, 13) > 7 ||
                   (sqlite3_column_int64(s, 13) > 0) != !!last))
      error = "corrupt_enrichment";
    if (!error) {
      identity(cfg, scope, job, ordinal, generation, recipe, store, digest,
               actual);
      if (strcmp(actual, key))
        error = "corrupt_enrichment";
    }
    if (!error) {
      int pending = !strcmp(status, "pending"),
          captured = !strcmp(status, "captured"),
          completed = !strcmp(status, "completed"),
          empty = !strcmp(status, "empty");
      if (!(pending || captured || completed || empty) ||
          (pending || empty ? capture || proof || receipt || rproof
                            : !capture || !proof) ||
          (captured && (receipt || rproof)) ||
          (completed && (*store ? !receipt || !rproof : receipt || rproof)))
        error = "corrupt_enrichment";
      if (!error && capture) {
        bytes += strlen(capture);
        if (bytes > 8388608 || strlen(capture) > 65536)
          error = "corrupt_enrichment";
        else
          error = digest_check(capture, proof);
      }
      if (!error && receipt)
        error = digest_check(receipt, rproof);
      if (!error && capture)
        error =
            saved_capture(cfg, scope, store, recipe, digest, capture, receipt);
    }
    free(key);
    free(job);
    free(recipe);
    free(store);
    free(digest);
    free(status);
    free(capture);
    free(proof);
    free(receipt);
    free(rproof);
    free(last);
  }
  sqlite3_finalize(s);
  return error;
}
static int step(const SourcesConfig *cfg, const Json *request, sqlite3 *db,
                FILE *out, int automatic) {
  if (!cfg->summary_inference) {
    fputs("{\"contract_version\":1,\"processed_items\":0,\"coverage\":"
          "\"enrichment_disabled\"}\n",
          out);
    return 0;
  }
  const char *owner = cfg->owner ? cfg->owner : "local-owner",
             *store = cfg->chutni_publish ? cfg->chutni_store_id : "";
  char recipe[65], scope[65];
  const char *error = sources_summary_opening_recipe(cfg, recipe);
  char *requested =
      automatic ? NULL : json_string(request, json_get(request, 0, "job_id"));
  if (!automatic && !sha(requested))
    error = "invalid_request";
  if (!error && (!store || (cfg->chutni_publish && strlen(store) != 36)))
    error = "invalid_chutni_configuration";
  sqlite3_int64 time = now_ms();
  if (!error && time < 0)
    error = "state_unavailable";
  if (!error)
    error = inventory_scope_identity(cfg, "", scope);
  if (!error)
    error = validate_rows(db, cfg, scope);
  sqlite3_stmt *s = NULL;
  if (!error && requested) {
    if (!prepare(db, &s,
                 "SELECT 1 FROM index_jobs j JOIN index_root_scopes r ON "
                 "r.job=j.id WHERE j.id=? AND j.owner=? AND r.scope=?"))
      error = "store_failed";
    else {
      text(s, 1, requested);
      text(s, 2, owner);
      text(s, 3, scope);
      int rc = sqlite3_step(s);
      if (rc != SQLITE_ROW)
        error = rc == SQLITE_DONE ? "unknown_index" : "store_failed";
    }
    sqlite3_finalize(s);
    s = NULL;
  }
  const char *selection =
      "WITH versions AS (SELECT i.job,i.ordinal,0 AS "
      "generation,i.evidence,i.digest FROM index_items i WHERE "
      "i.status='indexed' UNION ALL SELECT "
      "v.job,v.ordinal,v.generation,v.evidence,v.digest FROM index_versions v) "
      "SELECT "
      "v.job,v.ordinal,v.generation,v.evidence,v.digest,e.key,e.status,e."
      "capture,e.receipt,e.failures FROM versions v JOIN index_jobs j ON "
      "j.id=v.job JOIN index_root_scopes r ON r.job=v.job LEFT JOIN "
      "index_enrichments e ON e.job=v.job AND e.ordinal=v.ordinal AND "
      "e.generation=v.generation AND e.recipe=? AND e.store=? AND e.owner=? "
      "AND e.scope=? WHERE j.owner=? AND r.scope=? AND (?='' OR v.job=?) AND "
      "((e.status IN ('pending','captured') AND e.next_ms<=?) OR (e.key IS "
      "NULL AND v.generation=coalesce((SELECT max(generation) FROM "
      "index_versions x WHERE x.job=v.job AND x.ordinal=v.ordinal),0))) ORDER "
      "BY CASE WHEN e.status='captured' THEN 0 WHEN e.status='pending' THEN 1 "
      "ELSE 2 END,coalesce(e.turn,0),v.job,v.ordinal LIMIT 1";
  char *job = NULL, *literal = NULL, *digest = NULL, *saved = NULL,
       *status = NULL, *capture = NULL, *receipt = NULL;
  sqlite3_int64 ordinal = 0, generation = 0;
  int failures = 0, found = 0;
  char key[65];
  if (!error && !prepare(db, &s, selection))
    error = "store_failed";
  if (!error) {
    text(s, 1, recipe);
    text(s, 2, store);
    text(s, 3, owner);
    text(s, 4, scope);
    text(s, 5, owner);
    text(s, 6, scope);
    text(s, 7, requested ? requested : "");
    text(s, 8, requested ? requested : "");
    sqlite3_bind_int64(s, 9, time);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
      found = 1;
      job = column(s, 0);
      ordinal = sqlite3_column_int64(s, 1);
      generation = sqlite3_column_int64(s, 2);
      literal = column(s, 3);
      digest = column(s, 4);
      saved = column(s, 5);
      status = column(s, 6);
      capture = column(s, 7);
      receipt = column(s, 8);
      failures = sqlite3_column_int(s, 9);
      if (!sha(job) || ordinal < 0 || ordinal > 255 || generation < 0 ||
          generation > 256 || !literal || strlen(literal) > 524288)
        error = "corrupt_index";
      else
        error = digest_check(literal, digest);
    } else if (rc != SQLITE_DONE)
      error = "store_failed";
  }
  sqlite3_finalize(s);
  s = NULL;
  if (!error && found) {
    identity(cfg, scope, job, ordinal, generation, recipe, store, digest, key);
    if (saved && strcmp(saved, key))
      error = "corrupt_enrichment";
    if (!error)
      error =
          sources_literal_retained_validate(db, cfg, literal, strlen(literal));
  }
  if (!error && found && !saved) {
    if (!sql(db, "BEGIN IMMEDIATE"))
      error = "store_busy";
    else {
      if (!prepare(db, &s,
                   "INSERT INTO "
                   "index_enrichments(key,owner,scope,job,ordinal,generation,"
                   "recipe,store,evidence_digest,status,next_ms,turn) SELECT "
                   "?,?,?,?,?,?,?,?,?,'pending',?,coalesce((SELECT max(turn) "
                   "FROM index_enrichments),0)+1 WHERE (SELECT count(*) FROM "
                   "index_enrichments)<4096 AND (SELECT coalesce(max(turn),0) "
                   "FROM index_enrichments)<9223372036854775807"))
        error = "store_failed";
      else {
        text(s, 1, key);
        text(s, 2, owner);
        text(s, 3, scope);
        text(s, 4, job);
        sqlite3_bind_int64(s, 5, ordinal);
        sqlite3_bind_int64(s, 6, generation);
        text(s, 7, recipe);
        text(s, 8, store);
        text(s, 9, digest);
        sqlite3_bind_int64(s, 10, time + 1000);
        if (sqlite3_step(s) != SQLITE_DONE)
          error = "store_failed";
        else if (sqlite3_changes(db) != 1)
          error = "enrichment_limit";
      }
      sqlite3_finalize(s);
      s = NULL;
      if (error)
        sql(db, "ROLLBACK");
      else if (!sql(db, "COMMIT")) {
        sql(db, "ROLLBACK");
        error = "store_failed";
      }
    }
  }
  /* Reserve the maximum summary DTO before an expensive model call. The shared
   * index lease excludes competing captures; publication does not consume it.
   */
  if (!error && found && !capture) {
    if (!prepare(db, &s,
                 "SELECT coalesce(sum(length(CAST(capture AS BLOB))),0) FROM "
                 "index_enrichments"))
      error = "store_failed";
    else if (sqlite3_step(s) != SQLITE_ROW)
      error = "store_failed";
    else if (sqlite3_column_int64(s, 0) > 8388608 - 65536)
      error = "enrichment_limit";
    sqlite3_finalize(s);
    s = NULL;
  }
  int publishing = capture != NULL;
  char *body = NULL;
  size_t bytes = 0;
  char cause[65];
  if (!error && found) {
    FILE *stream = open_memstream(&body, &bytes);
    if (!stream)
      error = "resource_limit";
    else {
      int rc;
      if (publishing) {
        error = bind_capture(cfg, literal, capture);
        rc = error ? 1
                   : source_summary_publish_capture(
                         cfg, capture, strlen(capture), db, stream);
      } else
        rc = sources_summary_opening_retained(cfg, db, literal, strlen(literal),
                                              stream);
      if (fclose(stream))
        error = "resource_limit";
      else if (!error && rc)
        error = response_error(body, bytes, cause);
    }
    if (error && !strcmp(error, "empty_evidence")) {
      error = save(db, key, "empty", NULL, 0, time, NULL, 0);
    } else if (!error) {
      if (!publishing) {
        error = bind_capture(cfg, literal, body);
        if (!error) {
          if (!prepare(db, &s,
                       "SELECT coalesce(sum(length(CAST(capture AS BLOB))),0) "
                       "FROM index_enrichments"))
            error = "store_failed";
          else if (sqlite3_step(s) != SQLITE_ROW)
            error = "store_failed";
          else if (sqlite3_column_int64(s, 0) > 8388608 - (sqlite3_int64)bytes)
            error = "enrichment_limit";
          sqlite3_finalize(s);
          s = NULL;
        }
      }
      if (!error)
        error = save(db, key, publishing || !*store ? "completed" : "captured",
                     body, publishing, time, NULL, 0);
    }
    if (error) {
      sqlite3_int64 finished = now_ms();
      if (finished >= 0) {
        const char *saved_error =
            save(db, key, NULL, NULL, 0, finished, error, failures);
        if (saved_error)
          error = saved_error;
      }
    }
  }
  if (!error) {
    if (found)
      fprintf(out,
              "{\"contract_version\":1,\"processed_items\":1,\"job_id\":\"%s\","
              "\"ordinal\":%lld,\"generation\":%lld,\"phase\":\"%s\"}\n",
              job, (long long)ordinal, (long long)generation,
              publishing                               ? "published"
              : body && strstr(body, "empty_evidence") ? "empty"
                                                       : "captured");
    else
      fputs("{\"contract_version\":1,\"processed_items\":0,\"coverage\":\"no_"
            "due_enrichments\"}\n",
            out);
  }
  free(requested);
  free(job);
  free(literal);
  free(digest);
  free(saved);
  free(status);
  free(capture);
  free(receipt);
  free(body);
  return error ? fail(out, error) : 0;
}
static int status(const SourcesConfig *cfg, const Json *request, sqlite3 *db,
                  FILE *out) {
  char *job = json_string(request, json_get(request, 0, "job_id"));
  size_t after = 0;
  int token = json_get(request, 0, "after");
  const char *error =
      !sha(job) || (token >= 0 &&
                    (!json_uint(request, token, &after) || after > INT64_MAX))
          ? "invalid_request"
          : sources_interrupted(cfg);
  char scope[65];
  if (!error)
    error = inventory_scope_identity(cfg, "", scope);
  if (!error)
    error = validate_rows(db, cfg, scope);
  sqlite3_stmt *s = NULL;
  if (!error &&
      !prepare(db, &s,
               "SELECT 1 FROM index_jobs j JOIN index_root_scopes r ON "
               "r.job=j.id WHERE j.id=? AND j.owner=? AND r.scope=?"))
    error = "store_failed";
  if (!error) {
    text(s, 1, job);
    text(s, 2, cfg->owner ? cfg->owner : "local-owner");
    text(s, 3, scope);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_ROW)
      error = rc == SQLITE_DONE ? "unknown_index" : "store_failed";
  }
  sqlite3_finalize(s);
  s = NULL;
  char *body = NULL;
  size_t bytes = 0;
  FILE *stream = NULL;
  if (!error) {
    stream = open_memstream(&body, &bytes);
    if (!stream)
      error = "resource_limit";
  }
  if (!error &&
      !prepare(
          db, &s,
          "SELECT "
          "rowid,ordinal,generation,recipe,store,status,capture,receipt,next_"
          "ms,failures,last_error FROM index_enrichments WHERE owner=? AND "
          "scope=? AND job=? AND rowid>? ORDER BY rowid LIMIT 4"))
    error = "store_failed";
  size_t count = 0, last = after;
  if (!error) {
    text(s, 1, cfg->owner ? cfg->owner : "local-owner");
    text(s, 2, scope);
    text(s, 3, job);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)after);
    fprintf(stream, "{\"contract_version\":1,\"job_id\":\"%s\",\"entries\":[",
            job);
    while (!error) {
      int rc = sqlite3_step(s);
      if (rc == SQLITE_DONE)
        break;
      if (rc != SQLITE_ROW) {
        error = "store_failed";
        break;
      }
      sqlite3_int64 sequence = sqlite3_column_int64(s, 0);
      if (sequence <= 0) {
        error = "corrupt_enrichment";
        break;
      }
      last = (size_t)sequence;
      if (count++)
        fputc(',', stream);
      fprintf(stream,
              "{\"sequence\":%lld,\"ordinal\":%lld,\"generation\":%lld,"
              "\"recipe_sha256\":",
              (long long)sequence, (long long)sqlite3_column_int64(s, 1),
              (long long)sqlite3_column_int64(s, 2));
      const int columns[] = {3, 4, 5, 6, 7, 10};
      const char *names[] = {NULL,      "store_id", "status",
                             "summary", "receipt",  "last_error"};
      for (size_t i = 0; i < 6; i++) {
        if (i)
          fprintf(stream, ",\"%s\":", names[i]);
        const char *value = (const char *)sqlite3_column_text(s, columns[i]);
        if (!value)
          fputs("null", stream);
        else if (i == 3 || i == 4)
          fputs(value, stream);
        else
          json_quote(stream, value, strlen(value));
      }
      fprintf(stream, ",\"retry_at_unix_ms\":%lld,\"failures\":%lld}",
              (long long)sqlite3_column_int64(s, 8),
              (long long)sqlite3_column_int64(s, 9));
    }
    if (!error)
      fprintf(stream, "],\"next_after\":%zu}\n", last);
  }
  sqlite3_finalize(s);
  if (stream && fclose(stream))
    error = "resource_limit";
  if (!error)
    error = sources_interrupted(cfg);
  if (!error && bytes > 524288)
    error = "response_too_large";
  if (!error)
    fwrite(body, 1, bytes, out);
  free(body);
  free(job);
  return error ? fail(out, error) : 0;
}
int sources_index_enrich(const SourcesConfig *cfg, const Json *request,
                         sqlite3 *db, int directory, FILE *out) {
  int lease = -1;
  const char *error = source_index_lease(directory, &lease);
  if (!error)
    error = schema(db);
  char *op = json_string(request, json_get(request, 0, "op"));
  int rc = error ? 1
                 : op && !strcmp(op, "index_enrichment_status")
                       ? status(cfg, request, db, out)
                       : step(cfg, request, db, out,
                        op && !strcmp(op, "index_enrich_queue_step"));
  free(op);
  if (lease >= 0)
    close(lease);
  return error ? fail(out, error) : rc;
}
