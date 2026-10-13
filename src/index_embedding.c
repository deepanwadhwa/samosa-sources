#define _POSIX_C_SOURCE 200809L
#define JSON_TOKENS_MAX 8192
#include "chutni.h"
#include "control.h"
#include "index.h"
#include "index_embedding.h"
#include "inventory.h"
#include "native.h"
#include "semantic.h"
#include "sha256.h"
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
static int fail(FILE *out, const char *e) {
  fprintf(out, "{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n", e);
  return 1;
}
static int hex(const char *s) {
  if (!s || strlen(s) != 64)
    return 0;
  for (size_t i = 0; i < 64; i++)
    if (!strchr("0123456789abcdef", s[i]))
      return 0;
  return 1;
}
static int uuid(const char *s) {
  if (!s || strlen(s) != 36)
    return 0;
  for (size_t i = 0; i < 36; i++)
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-')
        return 0;
    } else if (!strchr("0123456789abcdef", s[i]))
      return 0;
  return 1;
}
static int sql(sqlite3 *db, const char *s) {
  return sqlite3_exec(db, s, NULL, NULL, NULL) == SQLITE_OK;
}
static int prepare(sqlite3 *db, sqlite3_stmt **s, const char *q) {
  return sqlite3_prepare_v2(db, q, -1, s, NULL) == SQLITE_OK;
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
static int equal(const Json *j, int o, const char *k, const char *v) {
  char *actual = json_string(j, json_get(j, o, k));
  int good = actual && v && !strcmp(actual, v);
  free(actual);
  return good;
}
static char *slice(const Json *j, int t) {
  return t >= 0 ? strndup(j->text + j->tokens[t].begin,
                          j->tokens[t].end - j->tokens[t].begin)
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
             "CREATE TABLE IF NOT EXISTS index_embeddings(key TEXT PRIMARY "
             "KEY,owner TEXT NOT NULL,scope TEXT NOT NULL,job TEXT NOT "
             "NULL,ordinal INTEGER NOT NULL,generation INTEGER NOT "
             "NULL,binding TEXT NOT NULL,store TEXT NOT NULL,artifact TEXT NOT "
             "NULL,evidence_digest TEXT NOT NULL,status TEXT NOT NULL,capture "
             "TEXT,capture_digest TEXT,receipt TEXT,receipt_digest "
             "TEXT,next_ms INTEGER NOT NULL DEFAULT 0,failures INTEGER NOT "
             "NULL DEFAULT 0,turn INTEGER NOT NULL DEFAULT 0,last_error "
             "TEXT,UNIQUE(owner,scope,job,ordinal,generation,binding,store))")
             ? NULL
             : "store_failed";
}
static int proof_limit(const char *body, const char *digest, size_t limit) {
  char actual[65];
  if (!body || strlen(body) > limit || !hex(digest))
    return 0;
  hash_bytes(body, strlen(body), actual);
  return !strcmp(actual, digest);
}
static int proof(const char *body, const char *digest) {
  return proof_limit(body, digest, 70000);
}
static void row_identity(const char *owner, const char *scope, const char *job,
                         const char *binding, const char *store,
                         const char *artifact, const char *digest,
                         sqlite3_int64 ordinal, sqlite3_int64 generation,
                         char identity[65]) {
  Sha256 h;
  sha256_init(&h);
  const char *parts[] = {owner, scope, job, binding, store, artifact, digest};
  for (size_t i = 0; i < 7; i++)
    sha256_update(&h, parts[i], strlen(parts[i]) + 1);
  char numbers[64];
  int n = snprintf(numbers, sizeof(numbers), "%lld:%lld", (long long)ordinal,
                   (long long)generation);
  sha256_update(&h, numbers, (size_t)n);
  unsigned char sum[32];
  sha256_final(&h, sum);
  sha256_hex(sum, identity);
}
static char *metadata(const SourcesConfig *, const char *, const char *,
                      const char *, const char *);
/* Archived rows may use a retired host profile. Validate their saved profile
 * and vector against the SDK receipt without loading any model or SDK store. */
static int archived_capture(const char *capture, const char *receipt,
                            const char *store, const char *artifact,
                            const char *literal, const char *binding) {
  Json c, e, b;
  char *bound = NULL, *embedded = NULL, *profile = NULL, *arguments = NULL,
       *payload = NULL, *request = NULL;
  int good = json_parse(&c, capture, strlen(capture)) &&
             c.tokens[0].type == JT_OBJECT && c.tokens[0].count == 2;
  if (good) {
    bound = slice(&c, json_get(&c, 0, "binding"));
    embedded = slice(&c, json_get(&c, 0, "embedding"));
    good = bound && embedded && json_parse(&e, embedded, strlen(embedded)) &&
           json_parse(&b, bound, strlen(bound));
  }
  if (good) {
    char sha[65];
    hash_bytes(literal, strlen(literal), sha);
    size_t n = 0, v = 0;
    int title = json_get(&e, 0, "title");
    good = e.tokens[0].type == JT_OBJECT && e.tokens[0].count == 15 &&
           json_uint(&e, json_get(&e, 0, "contract_version"), &v) && v == 1 &&
           equal(&e, 0, "kind", "derived") &&
           equal(&e, 0, "artifact_type", "embedding") &&
           equal(&e, 0, "input_sha256", sha) &&
           equal(&e, 0, "role", "document") && title >= 0 &&
           e.tokens[title].type == JT_NULL &&
           json_uint(&e, json_get(&e, 0, "input_bytes"), &n) &&
           n == strlen(literal);
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
    for (size_t k = 0; good && k < 15; k++)
      if (json_get(&e, 0, names[k]) < 0)
        good = 0;
    int pins = json_get(&e, 0, "provenance");
    char *prepared = json_string(&e, json_get(&e, 0, "prepared_input_sha256")),
         *provider = json_string(&e, json_get(&e, 0, "provider_id")),
         *model = json_string(&e, json_get(&e, pins, "model_sha256")),
         *engine = json_string(&e, json_get(&e, pins, "engine_sha256")),
         *recipe = json_string(&e, json_get(&e, pins, "recipe"));
    size_t prepared_bytes = 0, tokens = 0;
    good = good && hex(prepared) && provider && *provider &&
           strlen(provider) <= 128 && hex(model) && hex(engine) && recipe &&
           *recipe && strlen(recipe) <= 256 && pins >= 0 &&
           e.tokens[pins].type == JT_OBJECT && e.tokens[pins].count == 3 &&
           json_uint(&e, json_get(&e, 0, "input_tokens"), &tokens) && tokens &&
           tokens <= 131072 &&
           json_uint(&e, json_get(&e, 0, "prepared_input_bytes"),
                     &prepared_bytes) &&
           prepared_bytes >= strlen(literal) + strlen("title: none | text: ") &&
           prepared_bytes <=
               strlen(literal) + strlen("title: none | text: ") + 256;
    if (good) {
      for (const unsigned char *p = (const unsigned char *)provider; *p; p++)
        if (*p > 127 || (!isalnum(*p) && !strchr("-_:.", *p)))
          good = 0;
      Sha256 hash;
      sha256_init(&hash);
      const char *parts[] = {binding, "document", sha, prepared};
      for (size_t k = 0; k < 4; k++)
        sha256_update(&hash, parts[k], strlen(parts[k]) + 1);
      unsigned char digest[32];
      char id[65], operation[80];
      sha256_final(&hash, digest);
      sha256_hex(digest, id);
      snprintf(operation, sizeof(operation), "embed-%s", id);
      if (!equal(&e, 0, "operation_id", operation))
        good = 0;
    }
    free(prepared);
    free(provider);
    free(model);
    free(engine);
    free(recipe);
    profile = slice(&e, json_get(&e, 0, "profile"));
    if (good)
      good = profile &&
             equal(&e, json_get(&e, 0, "profile"), "normalization", "l2") &&
             !sources_semantic_arguments(profile, &e, json_get(&e, 0, "vector"),
                                         &arguments);
  }
  if (good) {
    SourcesConfig cfg = {.chutni_library = "/archived",
                         .chutni_store = "/archived",
                         .chutni_store_id = store};
    request = metadata(&cfg, artifact, literal, NULL, NULL);
    good = request &&
           !sources_chutni_embedding_validate(request, bound, strlen(bound));
    free(request);
    request = NULL;
    if (good && receipt) {
      payload = json_string(&b, json_get(&b, 0, "payload_hash"));
      request = metadata(&cfg, artifact, literal, payload, arguments);
      good = request && !sources_chutni_embedding_validate(request, receipt,
                                                           strlen(receipt));
    }
  }
  free(bound);
  free(embedded);
  free(profile);
  free(arguments);
  free(payload);
  free(request);
  return good;
}
static const char *integrity(sqlite3 *db, sqlite3_int64 *turn) {
  sqlite3_stmt *s = NULL;
  const char *error = NULL;
  *turn = 1;
  size_t rows = 0, bytes = 0;
  if (!prepare(db, &s,
               "SELECT "
               "e.key,e.owner,e.scope,e.job,e.ordinal,e.generation,e.binding,e."
               "store,e.artifact,"
               "e.evidence_digest,e.status,e.capture,e.capture_digest,e."
               "receipt,e.receipt_digest,"
               "e.next_ms,e.failures,e.turn,e.last_error,v.text,v.evidence,v."
               "digest,j.owner,r.scope "
               "FROM index_embeddings e LEFT JOIN (SELECT job,ordinal,0 AS "
               "generation,text,evidence,digest FROM index_items "
               "UNION ALL SELECT job,ordinal,generation,text,evidence,digest "
               "FROM index_versions) v "
               "ON v.job=e.job AND v.ordinal=e.ordinal AND "
               "v.generation=e.generation "
               "LEFT JOIN index_jobs j ON j.id=e.job LEFT JOIN "
               "index_root_scopes r ON r.job=e.job"))
    return "store_failed";
  while (!error) {
    int rc = sqlite3_step(s);
    if (rc == SQLITE_DONE)
      break;
    if (rc != SQLITE_ROW) {
      error = "store_failed";
      break;
    }
    if (++rows > 4096) {
      error = "corrupt_embedding";
      break;
    }
    int strings[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 18};
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
      int c = strings[i];
      if (sqlite3_column_type(s, c) == SQLITE_NULL) {
        if (c < 11)
          error = "corrupt_embedding";
        continue;
      }
      char *v = column(s, c);
      if (!v)
        error = "corrupt_embedding";
      free(v);
    }
    int hashes[] = {0, 2, 3, 6, 9};
    for (size_t i = 0; i < 5; i++)
      if (!hex((const char *)sqlite3_column_text(s, hashes[i])))
        error = "corrupt_embedding";
    const char *owner = (const char *)sqlite3_column_text(s, 1),
               *phase = (const char *)sqlite3_column_text(s, 10),
               *capture = (const char *)sqlite3_column_text(s, 11),
               *receipt = (const char *)sqlite3_column_text(s, 13);
    if (!owner || !*owner || strlen(owner) > 128 ||
        !uuid((const char *)sqlite3_column_text(s, 7)) ||
        !uuid((const char *)sqlite3_column_text(s, 8)) || !phase ||
        (strcmp(phase, "pending") && strcmp(phase, "captured") &&
         strcmp(phase, "published")))
      error = "corrupt_embedding";
    const char *last = (const char *)sqlite3_column_text(s, 18);
    if (last) {
      if (!*last || strlen(last) > 64)
        error = "corrupt_embedding";
      for (size_t i = 0; last[i]; i++)
        if (!strchr("abcdefghijklmnopqrstuvwxyz0123456789_", last[i]))
          error = "corrupt_embedding";
    }
    int ints[] = {4, 5, 15, 16, 17};
    for (size_t i = 0; i < 5; i++)
      if (sqlite3_column_type(s, ints[i]) != SQLITE_INTEGER ||
          sqlite3_column_int64(s, ints[i]) < 0)
        error = "corrupt_embedding";
    if (sqlite3_column_int64(s, 4) > 255 || sqlite3_column_int64(s, 5) > 256 ||
        sqlite3_column_int64(s, 16) > 7 || sqlite3_column_int64(s, 17) < 1)
      error = "corrupt_embedding";
    if (capture) {
      if (!proof(capture, (const char *)sqlite3_column_text(s, 12)))
        error = "corrupt_embedding";
      bytes += strlen(capture);
    } else if (sqlite3_column_type(s, 12) != SQLITE_NULL)
      error = "corrupt_embedding";
    if (receipt) {
      if (!proof(receipt, (const char *)sqlite3_column_text(s, 14)))
        error = "corrupt_embedding";
      bytes += strlen(receipt);
    } else if (sqlite3_column_type(s, 14) != SQLITE_NULL)
      error = "corrupt_embedding";
    if (phase && ((!strcmp(phase, "pending") && (capture || receipt)) ||
                  (!strcmp(phase, "captured") && (!capture || receipt)) ||
                  (!strcmp(phase, "published") && (!capture || !receipt))))
      error = "corrupt_embedding";
    if (!error) {
      const char *literal = (const char *)sqlite3_column_text(s, 19),
                 *evidence = (const char *)sqlite3_column_text(s, 20),
                 *digest = (const char *)sqlite3_column_text(s, 21),
                 *stored_owner = (const char *)sqlite3_column_text(s, 22),
                 *scope = (const char *)sqlite3_column_text(s, 2),
                 *stored_scope = (const char *)sqlite3_column_text(s, 23),
                 *store = (const char *)sqlite3_column_text(s, 7),
                 *artifact = (const char *)sqlite3_column_text(s, 8);
      char identity[65];
      for (int k = 19; k <= 23; k++) {
        char *value = column(s, k);
        if (!value)
          error = "corrupt_embedding";
        free(value);
      }
      if (!error &&
          (!literal || strlen(literal) > SOURCES_READ_MAX ||
           !proof_limit(evidence, digest, 8388608) ||
           strcmp(digest, (const char *)sqlite3_column_text(s, 9)) ||
           strcmp(stored_owner, owner) || strcmp(stored_scope, scope)))
        error = "corrupt_embedding";
      if (!error) {
        char *decoded = source_index_literal(evidence, strlen(evidence));
        if (!decoded || strcmp(decoded, literal))
          error = "corrupt_embedding";
        free(decoded);
        row_identity(owner, scope, (const char *)sqlite3_column_text(s, 3),
                     (const char *)sqlite3_column_text(s, 6), store, artifact,
                     digest, sqlite3_column_int64(s, 4),
                     sqlite3_column_int64(s, 5), identity);
        if (strcmp(identity, (const char *)sqlite3_column_text(s, 0)))
          error = "corrupt_embedding";
      }
      if (!error && capture &&
          !archived_capture(capture, receipt, store, artifact, literal,
                            (const char *)sqlite3_column_text(s, 6)))
        error = "corrupt_embedding";
    }
    if (phase && !strcmp(phase, "pending"))
      bytes += 140000;
    else if (phase && !strcmp(phase, "captured"))
      bytes += 70000;
    sqlite3_int64 saved = sqlite3_column_int64(s, 17);
    if (saved == INT64_MAX)
      error = "scheduler_limit";
    else if (saved >= *turn)
      *turn = saved + 1;
    if (bytes > 8388608)
      error = "corrupt_embedding";
  }
  sqlite3_finalize(s);
  return error;
}
static char *metadata(const SourcesConfig *cfg, const char *artifact,
                      const char *literal, const char *payload,
                      const char *arguments) {
  char *body = NULL;
  size_t n = 0;
  FILE *f = open_memstream(&body, &n);
  if (!f)
    return NULL;
  const char *keys[] = {"library", "store", "store_id", "artifact_id", "text"};
  const char *values[] = {cfg->chutni_library, cfg->chutni_store,
                          cfg->chutni_store_id, artifact, literal};
  fputc('{', f);
  for (size_t i = 0; i < 5; i++) {
    if (i)
      fputc(',', f);
    json_quote(f, keys[i], strlen(keys[i]));
    fputc(':', f);
    json_quote(f, values[i], strlen(values[i]));
  }
  fputs(",\"payload_hash\":", f);
  if (payload)
    json_quote(f, payload, strlen(payload));
  else
    fputs("null", f);
  fputs(",\"arguments\":", f);
  fputs(arguments ? arguments : "null", f);
  fputc('}', f);
  if (fclose(f)) {
    free(body);
    return NULL;
  }
  return body;
}
static const char *save(sqlite3 *db, const char *key, const char *phase,
                        const char *body, sqlite3_int64 time, int failures,
                        sqlite3_int64 turn, const char *last_error) {
  if (!sql(db, "BEGIN IMMEDIATE"))
    return "store_busy";
  const char *error = NULL;
  sqlite3_stmt *s = NULL;
  if (body) {
    if (!prepare(db, &s,
                 "SELECT coalesce(sum(length(CAST(capture AS "
                 "BLOB))+coalesce(length(CAST(receipt AS BLOB)),0)),0) FROM "
                 "index_embeddings"))
      error = "store_failed";
    if (!error &&
        (sqlite3_step(s) != SQLITE_ROW ||
         sqlite3_column_int64(s, 0) + (sqlite3_int64)strlen(body) > 8388608))
      error = "embedding_capacity";
    sqlite3_finalize(s);
    s = NULL;
  }
  const char *command =
      body ? (!strcmp(phase, "captured")
                  ? "UPDATE index_embeddings SET "
                    "status='captured',capture=?,capture_digest=?,next_ms=0,"
                    "failures=0,last_error=NULL,turn=? WHERE key=? AND "
                    "status='pending'"
                  : "UPDATE index_embeddings SET "
                    "status='published',receipt=?,receipt_digest=?,next_ms=0,"
                    "failures=0,last_error=NULL,turn=? WHERE key=? AND "
                    "status='captured'")
           : "UPDATE index_embeddings SET "
             "next_ms=?,failures=?,last_error=?,turn=? WHERE key=? AND "
             "status!='published'";
  if (!error && !prepare(db, &s, command))
    error = "store_failed";
  if (!error) {
    if (body) {
      char digest[65];
      hash_bytes(body, strlen(body), digest);
      text(s, 1, body);
      text(s, 2, digest);
      sqlite3_bind_int64(s, 3, turn);
      text(s, 4, key);
    } else {
      sqlite3_int64 delay = 1000ll << failures;
      if (delay > 60000)
        delay = 60000;
      sqlite3_bind_int64(s, 1, time + delay);
      sqlite3_bind_int(s, 2, failures);
      text(s, 3, last_error);
      sqlite3_bind_int64(s, 4, turn);
      text(s, 5, key);
    }
    if (sqlite3_step(s) != SQLITE_DONE || sqlite3_changes(db) != 1)
      error = "corrupt_embedding";
  }
  sqlite3_finalize(s);
  if (error)
    sql(db, "ROLLBACK");
  else if (!sql(db, "COMMIT")) {
    sql(db, "ROLLBACK");
    error = "store_failed";
  }
  return error;
}
#ifdef SAMOSA_SOURCES_TEST_FAULTS
static const char *checkpoint(const SourcesConfig *cfg, const char *name) {
  const char *path = getenv(name);
  if (!path || !*path)
    return NULL;
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    return "test_marker_failed";
  close(fd);
  while (!access(path, F_OK)) {
    const char *e = sources_interrupted(cfg);
    if (e)
      return e;
    struct timespec pause = {.tv_nsec = 10000000};
    nanosleep(&pause, NULL);
  }
  return NULL;
}
#endif
static int step(const SourcesConfig *cfg, const Json *request, sqlite3 *db,
                FILE *out, int automatic) {
  const char *owner = cfg->owner ? cfg->owner : "local-owner",
             *error = sources_interrupted(cfg);
  char scope[65], binding[65];
  char *requested =
      automatic ? NULL : json_string(request, json_get(request, 0, "job_id"));
  if (!automatic && !hex(requested))
    error = "invalid_request";
  if (!error &&
      (!cfg->chutni_publish || !cfg->chutni_library || !cfg->chutni_store ||
       !cfg->chutni_helper || !uuid(cfg->chutni_store_id)))
    error = "chutni_publish_disabled";
  if (!error)
    error = sources_embedding_identity(cfg, binding);
  if (!error)
    error = inventory_scope_identity(cfg, "", scope);
  sqlite3_int64 time = now_ms(), turn = 0;
  if (!error && time < 0)
    error = "state_unavailable";
  if (!error)
    error = integrity(db, &turn);
  sqlite3_stmt *s = NULL;
  if (!error && requested) {
    if (!prepare(db, &s,
                 "SELECT 1 FROM index_jobs j JOIN index_root_scopes r ON "
                 "r.job=j.id WHERE j.id=? AND j.owner=? AND r.scope=?"))
      error = "store_failed";
    if (!error) {
      text(s, 1, requested);
      text(s, 2, owner);
      text(s, 3, scope);
      if (sqlite3_step(s) != SQLITE_ROW)
        error = "unknown_index";
    }
    sqlite3_finalize(s);
    s = NULL;
  }
  int available = 0;
  if (!error) {
    if (!prepare(db, &s,
                 "SELECT 1 FROM sqlite_master WHERE type='table' AND "
                 "name='index_publications'"))
      error = "store_failed";
    else
      available = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    s = NULL;
  }
  const char *selection =
      "WITH versions AS (SELECT job,ordinal,0 AS "
      "generation,text,evidence,digest FROM index_items WHERE status='indexed' "
      "UNION ALL SELECT job,ordinal,generation,text,evidence,digest FROM "
      "index_versions) SELECT "
      "v.job,v.ordinal,v.generation,v.text,v.evidence,v.digest,p.response,p."
      "response_digest,e.key,e.status,e.capture,e.failures,e.artifact,e."
      "evidence_digest,p.publication_id,p.version,p.evidence_id FROM versions "
      "v "
      "JOIN index_jobs j ON j.id=v.job JOIN index_root_scopes r ON r.job=v.job "
      "JOIN index_publications p ON p.job=v.job AND p.ordinal=v.ordinal AND "
      "p.generation=v.generation AND p.store_id=? AND p.status='committed' "
      "LEFT JOIN index_embeddings e ON e.owner=j.owner AND e.scope=r.scope AND "
      "e.job=v.job AND e.ordinal=v.ordinal AND e.generation=v.generation AND "
      "e.binding=? AND e.store=p.store_id WHERE j.owner=? AND r.scope=? AND "
      "(?='' OR v.job=?) AND ((e.key IS NULL AND v.generation=coalesce((SELECT "
      "max(generation) FROM index_versions x WHERE x.job=v.job AND "
      "x.ordinal=v.ordinal),0)) OR (e.status!='published' AND e.next_ms<=?)) "
      "ORDER BY CASE WHEN e.status='captured' THEN 0 WHEN e.key IS NOT NULL "
      "THEN 1 ELSE 2 END,coalesce(e.turn,0),v.job,v.ordinal LIMIT 1";
  char *job = NULL, *literal = NULL, *evidence = NULL, *digest = NULL,
       *publication = NULL, *publication_digest = NULL, *key = NULL,
       *phase = NULL, *capture = NULL, *artifact = NULL, *read_metadata = NULL,
       *read_receipt = NULL, *embedding = NULL, *saved = NULL,
       *arguments = NULL, *write_metadata = NULL, *receipt = NULL,
       *stored_artifact = NULL, *stored_digest = NULL, *publication_id = NULL,
       *publication_version = NULL, *publication_evidence = NULL;
  sqlite3_int64 ordinal = 0, generation = 0;
  int found = 0, failures = 0, reserved = 0;
  size_t bytes = 0, embedding_bytes = 0;
  if (!error && available && !prepare(db, &s, selection))
    error = "store_failed";
  if (!error && available) {
    text(s, 1, cfg->chutni_store_id);
    text(s, 2, binding);
    text(s, 3, owner);
    text(s, 4, scope);
    text(s, 5, requested ? requested : "");
    text(s, 6, requested ? requested : "");
    sqlite3_bind_int64(s, 7, time);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
      found = 1;
      job = column(s, 0);
      ordinal = sqlite3_column_int64(s, 1);
      generation = sqlite3_column_int64(s, 2);
      literal = column(s, 3);
      evidence = column(s, 4);
      digest = column(s, 5);
      publication = column(s, 6);
      publication_digest = column(s, 7);
      key = column(s, 8);
      phase = column(s, 9);
      capture = column(s, 10);
      failures = sqlite3_column_int(s, 11);
      stored_artifact = column(s, 12);
      stored_digest = column(s, 13);
      publication_id = column(s, 14);
      publication_version = column(s, 15);
      publication_evidence = column(s, 16);
    } else if (rc != SQLITE_DONE)
      error = "store_failed";
  }
  sqlite3_finalize(s);
  s = NULL;
  Json published;
  char identity[65];
  if (!error && found) {
    char actual[65];
    char *decoded =
        evidence ? source_index_literal(evidence, strlen(evidence)) : NULL;
    if (!hex(job) || !literal ||
        strlen(literal) > SOURCES_READ_MAX || !evidence || !hex(digest) ||
        !decoded || strcmp(decoded, literal) ||
        !proof(publication, publication_digest) ||
        !json_parse(&published, publication, strlen(publication)))
      error = "corrupt_embedding";
    free(decoded);
    if (!error) {
      hash_bytes(evidence, strlen(evidence), actual);
      if (strcmp(actual, digest))
        error = "corrupt_embedding";
    }
    int p = !error ? json_get(&published, 0, "publication") : -1;
    if (!error) {
      artifact = json_string(&published,
                             json_get(&published, p, "external_artifact_id"));
      if (!uuid(artifact) ||
          !equal(&published, p, "store_id", cfg->chutni_store_id) ||
          !equal(&published, 0, "job_id", job))
        error = "corrupt_publication";
    }
    if (!error) {
      Json source;
      size_t a = 0, b = 0;
      char validation[512];
      char *body = slice(&published, p);
      if (!hex(publication_id) || !hex(publication_version) ||
          !hex(publication_evidence) ||
          !json_parse(&source, evidence, strlen(evidence)) ||
          !equal(&source, 0, "version", publication_version) ||
          !equal(&source, 0, "evidence_id", publication_evidence) ||
          !json_uint(&published, json_get(&published, 0, "ordinal"), &a) ||
          a != (size_t)ordinal ||
          !json_uint(&published, json_get(&published, 0, "generation"), &b) ||
          b != (size_t)generation ||
          (key && (!stored_artifact || strcmp(stored_artifact, artifact) ||
                   !stored_digest || strcmp(stored_digest, digest))))
        error = "corrupt_publication";
      if (!error) {
        snprintf(validation, sizeof(validation),
                 "{\"store_id\":\"%s\",\"publication_id\":\"%s\",\"version\":"
                 "\"%s\",\"evidence_id\":\"%s\"}",
                 cfg->chutni_store_id, publication_id, publication_version,
                 publication_evidence);
        if (!body ||
            sources_chutni_publish_validate(validation, body, strlen(body)))
          error = "corrupt_publication";
      }
      free(body);
    }
    if (!error) {
      row_identity(owner, scope, job, binding, cfg->chutni_store_id, artifact,
                   digest, ordinal, generation, identity);
      if (key && strcmp(key, identity))
        error = "corrupt_embedding";
    }
    if (!error && !key) {
      if (!sql(db, "BEGIN IMMEDIATE"))
        error = "store_busy";
      else {
        if (!prepare(
                db, &s,
                "INSERT INTO "
                "index_embeddings(key,owner,scope,job,ordinal,generation,"
                "binding,store,artifact,evidence_digest,status,turn) "
                "SELECT ?,?,?,?,?,?,?,?,?,?,'pending',? WHERE (SELECT "
                "count(*) FROM index_embeddings)<4096 AND (SELECT coalesce(sum("
                "coalesce(length(CAST(capture AS "
                "BLOB)),0)+coalesce(length(CAST(receipt AS BLOB)),0)+"
                "CASE status WHEN 'pending' THEN 140000 WHEN 'captured' THEN "
                "70000 ELSE 0 END),0) "
                "FROM index_embeddings)<=8248608"))
          error = "store_failed";
        if (!error) {
          text(s, 1, identity);
          text(s, 2, owner);
          text(s, 3, scope);
          text(s, 4, job);
          sqlite3_bind_int64(s, 5, ordinal);
          sqlite3_bind_int64(s, 6, generation);
          text(s, 7, binding);
          text(s, 8, cfg->chutni_store_id);
          text(s, 9, artifact);
          text(s, 10, digest);
          sqlite3_bind_int64(s, 11, turn);
          if (sqlite3_step(s) != SQLITE_DONE)
            error = "store_failed";
          else if (sqlite3_changes(db) != 1)
            error = "embedding_capacity";
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
      if (!error) {
        key = strdup(identity);
        phase = strdup("pending");
        if (!key || !phase)
          error = "resource_limit";
      }
    }
    if (!error)
      reserved = 1;
    if (!error && (!*literal || strlen(literal) > 60000))
      error = "embedding_input_limit";
    if (!error) {
      read_metadata = metadata(cfg, artifact, literal, NULL, NULL);
      if (!read_metadata)
        error = "resource_limit";
    }
    if (!error && !strcmp(phase, "pending")) {
      error = sources_native_chutni_embedding(cfg, read_metadata, &read_receipt,
                                              &bytes);
      if (!error)
        error = sources_embedding_generate(cfg, literal, "document", NULL,
                                           &embedding, &embedding_bytes);
      if (!error) {
        FILE *f = open_memstream(&saved, &bytes);
        if (!f)
          error = "resource_limit";
        else {
          fprintf(f, "{\"binding\":%s,\"embedding\":%s}", read_receipt,
                  embedding);
          if (fclose(f) || bytes > 70000)
            error = "resource_limit";
        }
      }
      if (!error)
        error = sources_interrupted(cfg);
      if (!error) {
        char current[65];
        error = inventory_scope_identity(cfg, "", current);
        if (!error && strcmp(current, scope))
          error = "scope_unavailable";
      }
      if (!error)
        error = save(db, key, "captured", saved, time, 0, turn, NULL);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
      if (!error)
        error = checkpoint(cfg, "SAMOSA_EMBEDDING_POST_CAPTURE_MARKER");
#endif
    } else if (!error) {
      Json c;
      if (!capture || !json_parse(&c, capture, strlen(capture)) ||
          c.tokens[0].count != 2)
        error = "corrupt_embedding";
      if (!error) {
        read_receipt = slice(&c, json_get(&c, 0, "binding"));
        embedding = slice(&c, json_get(&c, 0, "embedding"));
        if (!read_receipt || !embedding ||
            sources_chutni_embedding_validate(read_metadata, read_receipt,
                                              strlen(read_receipt)))
          error = "corrupt_embedding";
      }
      if (!error)
        error = sources_embedding_capture_arguments(
            cfg, literal, NULL, embedding, strlen(embedding), &arguments);
      if (!error) {
        Json r;
        if (!json_parse(&r, read_receipt, strlen(read_receipt)))
          error = "corrupt_embedding";
        else {
          char *payload = json_string(&r, json_get(&r, 0, "payload_hash"));
          write_metadata = metadata(cfg, artifact, literal, payload, arguments);
          free(payload);
          if (!write_metadata)
            error = "resource_limit";
        }
      }
      if (!error)
        error = sources_native_chutni_embedding(cfg, write_metadata, &receipt,
                                                &bytes);
#ifdef SAMOSA_SOURCES_TEST_FAULTS
      if (!error)
        error = checkpoint(cfg, "SAMOSA_EMBEDDING_POST_SDK_MARKER");
#endif
      if (!error) {
        char current[65];
        error = inventory_scope_identity(cfg, "", current);
        if (!error && strcmp(current, scope))
          error = "scope_unavailable";
      }
      if (!error)
        error = sources_interrupted(cfg);
      if (!error)
        error = save(db, key, "published", receipt, time, 0, turn, NULL);
    }
    if (error && reserved) {
      const char *stored = save(db, key, NULL, NULL, now_ms(),
                                failures < 7 ? failures + 1 : 7, turn, error);
      if (stored)
        error = stored;
    }
  }
  if (!error) {
    fprintf(out, "{\"contract_version\":1,\"processed_items\":%d",
            found ? 1 : 0);
    if (found)
      fprintf(out,
              ",\"embedding_id\":\"%s\",\"job_id\":\"%s\",\"ordinal\":%lld,"
              "\"generation\":%lld,\"phase\":\"%s\"",
              identity, job, (long long)ordinal, (long long)generation,
              !strcmp(phase, "pending") ? "captured" : "published");
    fputs("}\n", out);
  }
  free(stored_artifact);
  free(stored_digest);
  free(publication_id);
  free(publication_version);
  free(publication_evidence);
  free(requested);
  free(job);
  free(literal);
  free(evidence);
  free(digest);
  free(publication);
  free(publication_digest);
  free(key);
  free(phase);
  free(capture);
  free(artifact);
  free(read_metadata);
  free(read_receipt);
  free(embedding);
  free(saved);
  free(arguments);
  free(write_metadata);
  free(receipt);
  return error ? fail(out, error) : 0;
}
static int status(const SourcesConfig *cfg, const Json *request, sqlite3 *db,
                  FILE *out) {
  const char *owner = cfg->owner ? cfg->owner : "local-owner";
  char scope[65];
  sqlite3_int64 turn = 0;
  char *job = json_string(request, json_get(request, 0, "job_id"));
  size_t after = 0;
  int a = json_get(request, 0, "after");
  const char *error =
      !hex(job) ||
              (a >= 0 && (!json_uint(request, a, &after) || after > INT64_MAX))
          ? "invalid_request"
          : sources_interrupted(cfg);
  if (!error)
    error = inventory_scope_identity(cfg, "", scope);
  if (!error)
    error = integrity(db, &turn);
  sqlite3_stmt *s = NULL;
  if (!error &&
      !prepare(db, &s,
               "SELECT 1 FROM index_jobs j JOIN index_root_scopes r ON "
               "r.job=j.id WHERE j.id=? AND j.owner=? AND r.scope=?"))
    error = "store_failed";
  if (!error) {
    text(s, 1, job);
    text(s, 2, owner);
    text(s, 3, scope);
    if (sqlite3_step(s) != SQLITE_ROW)
      error = "unknown_index";
  }
  sqlite3_finalize(s);
  s = NULL;
  char *body = NULL;
  size_t bytes = 0;
  FILE *f = NULL;
  if (!error) {
    f = open_memstream(&body, &bytes);
    if (!f)
      error = "resource_limit";
  }
  if (!error &&
      !prepare(db, &s,
               "SELECT "
               "rowid,key,ordinal,generation,binding,store,artifact,status,"
               "next_ms,failures,last_error,capture_digest,receipt_digest,"
               "receipt FROM index_embeddings WHERE job=? AND owner=? AND "
               "scope=? AND rowid>? ORDER BY rowid LIMIT 17"))
    error = "store_failed";
  size_t count = 0;
  sqlite3_int64 cursor = (sqlite3_int64)after;
  int more = 0;
  if (!error) {
    text(s, 1, job);
    text(s, 2, owner);
    text(s, 3, scope);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)after);
    fprintf(f, "{\"contract_version\":1,\"job_id\":\"%s\",\"items\":[", job);
    for (;;) {
      int rc = sqlite3_step(s);
      if (rc == SQLITE_DONE)
        break;
      if (rc != SQLITE_ROW) {
        error = "store_failed";
        break;
      }
      if (count == 16) {
        more = 1;
        break;
      }
      if (count++)
        fputc(',', f);
      cursor = sqlite3_column_int64(s, 0);
      fprintf(f,
              "{\"embedding_id\":\"%s\",\"ordinal\":%lld,\"generation\":%lld,"
              "\"binding\":\"%s\",\"store_id\":\"%s\",\"artifact_id\":\"%s\","
              "\"phase\":\"%s\",\"next_ms\":%lld,\"failures\":%lld",
              sqlite3_column_text(s, 1), (long long)sqlite3_column_int64(s, 2),
              (long long)sqlite3_column_int64(s, 3), sqlite3_column_text(s, 4),
              sqlite3_column_text(s, 5), sqlite3_column_text(s, 6),
              sqlite3_column_text(s, 7), (long long)sqlite3_column_int64(s, 8),
              (long long)sqlite3_column_int64(s, 9));
      const char *keys[] = {"last_error", "capture_digest", "receipt_digest"};
      for (size_t i = 0; i < 3; i++) {
        const char *value = (const char *)sqlite3_column_text(s, (int)i + 10);
        fprintf(f, ",\"%s\":", keys[i]);
        if (value)
          json_quote(f, value, strlen(value));
        else
          fputs("null", f);
      }
      const char *receipt = (const char *)sqlite3_column_text(s, 13);
      char *representation = NULL;
      Json r;
      if (receipt && json_parse(&r, receipt, strlen(receipt)))
        representation = json_string(&r, json_get(&r, 0, "representation_id"));
      fputs(",\"representation_id\":", f);
      if (representation)
        json_quote(f, representation, strlen(representation));
      else
        fputs("null", f);
      free(representation);
      fputc('}', f);
    }
    fprintf(f, "],\"next_after\":%lld,\"has_more\":%s}\n", (long long)cursor,
            more ? "true" : "false");
  }
  sqlite3_finalize(s);
  if (f && fclose(f) && !error)
    error = "resource_limit";
  if (!error)
    error = sources_interrupted(cfg);
  if (!error)
    fwrite(body, 1, bytes, out);
  free(job);
  free(body);
  return error ? fail(out, error) : 0;
}
int sources_index_embed(const SourcesConfig *cfg, const Json *request,
                        sqlite3 *db, int directory, FILE *out) {
  int lease = -1;
  const char *error = source_index_lease(directory, &lease);
  if (!error)
    error = schema(db);
  char *op = json_string(request, json_get(request, 0, "op"));
  int rc = error ? fail(out, error)
           : op && !strcmp(op, "index_embedding_status")
               ? status(cfg, request, db, out)
               : step(cfg, request, db, out,
                      op && !strcmp(op, "index_embed_queue_step"));
  free(op);
  if (lease >= 0)
    close(lease);
  return rc;
}
