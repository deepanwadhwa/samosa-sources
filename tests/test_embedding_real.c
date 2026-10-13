#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "embedding.h"
#include "json.h"
#include "sha256.h"
#include "sources.h"
#include "native.h"
#include "chutni.h"
#include "watch.h"
#include <sqlite3.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
static unsigned checks;
#define CHECK(x)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
static char *load(const char *path, size_t ceiling) {
  FILE *f = fopen(path, "rb");
  CHECK(f);
  char *body = calloc(ceiling + 1, 1);
  CHECK(body);
  size_t n = fread(body, 1, ceiling + 1, f);
  CHECK(n && n <= ceiling && !ferror(f) && !memchr(body, 0, n));
  CHECK(!fclose(f));
  return body;
}
static char *slice(const Json *j, int token) {
  CHECK(token >= 0);
  char *s = strndup(j->text + j->tokens[token].begin,
                    j->tokens[token].end - j->tokens[token].begin);
  CHECK(s);
  return s;
}
static char *field(const Json *j, int object, const char *key) {
  char *s = json_string(j, json_get(j, object, key));
  CHECK(s);
  return s;
}
static void symbol(void *library, const char *name, void *target, size_t n) {
  void *s = dlsym(library, name);
  CHECK(s && n == sizeof(s));
  memcpy(target, &s, n);
}
typedef struct {
  SourcesInferenceProcess process;
  const char *configuration, *config_path, *state;
  unsigned revision, calls;
} Transport;
static void configure(Transport *t) {
  Json j;
  CHECK(json_parse(&j, t->configuration, strlen(t->configuration)));
  FILE *out = fopen(t->config_path, "w");
  CHECK(out);
  CHECK(!chmod(t->config_path, 0600));
  fputc('{', out);
  int first = 1;
  for (int key = 1; key < j.tokens[0].next;
       key = j.tokens[j.tokens[key].next].next) {
    char *name = json_string(&j, key);
    CHECK(name);
    int value = j.tokens[key].next;
    if (strcmp(name, "state") && strcmp(name, "expected_revision")) {
      if (!first)
        fputc(',', out);
      first = 0;
      json_quote(out, name, strlen(name));
      fputc(':', out);
      fwrite(j.text + j.tokens[value].begin, 1,
             j.tokens[value].end - j.tokens[value].begin, out);
    }
    free(name);
  }
  fputs(",\"state\":", out);
  json_quote(out, t->state, strlen(t->state));
  fprintf(out, ",\"expected_revision\":%u}", t->revision);
  CHECK(!fclose(out));
}
static const char *actual(void *context, const SourcesInferenceControl *control,
                          const char *request, size_t bytes, char **reply,
                          size_t *length) {
  Transport *t = context;
  if (!t->calls) configure(t);
  t->calls++;
  const char *error = sources_inference_process_call(
      &t->process, control, request, bytes, reply, length);
  if (error)
    fprintf(stderr, "actual transport: %s\n", error);
  else
    t->revision++;
  return error;
}
static char *call(const SourcesConfig *cfg, const char *request) {
  char *body = NULL;
  size_t n = 0;
  FILE *out = open_memstream(&body, &n);
  CHECK(out);
  int rc = sources_dispatch(cfg, request, strlen(request), out);
  CHECK(!fclose(out));
  if (rc)
    fprintf(stderr, "response: %s\n", body);
  CHECK(!rc);
  Json j;
  CHECK(json_parse(&j, body, n));
  return body;
}
typedef struct { sqlite3 *db; sqlite3_stmt *count; double until; size_t documents; int failed; } QueueStop;
static int queue_complete(void *context){
  QueueStop *stop=context;struct timespec now;
  if(clock_gettime(CLOCK_MONOTONIC,&now)){stop->failed=1;return 1;}
  if((double)now.tv_sec+(double)now.tv_nsec/1e9>=stop->until){stop->failed=1;return 1;}
  int rc=sqlite3_step(stop->count);int done=rc==SQLITE_ROW&&sqlite3_column_int64(stop->count,0)==(sqlite3_int64)stop->documents;
  if(rc!=SQLITE_ROW&&rc!=SQLITE_BUSY)stop->failed=1;
  sqlite3_reset(stop->count);return done||stop->failed;
}
typedef struct { unsigned port; double until; int observed, expired; } RuntimeStop;
static double seconds_now(void) {
  struct timespec now; CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));
  return (double)now.tv_sec+(double)now.tv_nsec/1e9;
}
static int listener_open(unsigned port) {
  int fd=socket(AF_INET,SOCK_STREAM,0); CHECK(fd>=0);
  struct sockaddr_in address={0}; address.sin_family=AF_INET;
  address.sin_port=htons((unsigned short)port);
  address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  int active=!connect(fd,(struct sockaddr *)&address,sizeof(address));
  CHECK(!close(fd)); return active;
}
static void listener_reaped(unsigned port) {
  double until=seconds_now()+10;
  while(listener_open(port)) {
    CHECK(seconds_now()<until);
    struct timespec delay={0,20000000};nanosleep(&delay,NULL);
  }
}
static int runtime_active(void *context) {
  RuntimeStop *stop=context;
  if(stop->observed||stop->expired)return 1;
  if(listener_open(stop->port)){stop->observed=1;return 1;}
  if(seconds_now()>=stop->until){stop->expired=1;return 1;}
  return 0;
}
int main(int argc, char **argv) {
  /* SDK library, Sources native worker, Chutni worker, owned Inference binary,
   * managed host config, actual document receipts from Inference qualification,
   * output directory. */
  CHECK(argc == 8 || (argc == 9 && !strcmp(argv[8],"--controls")));
  int controls=argc==9;
  const char *output_directory = argv[7], *receipts_path = argv[6],
             *configuration_path = argv[5], *inference_binary = argv[4];
  CHECK(!mkdir(output_directory, 0700));
  char *configuration = load(configuration_path, 16384),
       *receipts = load(receipts_path, 262144);
  Json c, r;
  CHECK(json_parse(&c, configuration, strlen(configuration)));
  CHECK(json_parse(&r, receipts, strlen(receipts)) &&
        r.tokens[0].type == JT_ARRAY && r.tokens[0].count >= 8);
  int initial = json_get(&r, 1, "response"),
      provenance = json_get(&r, initial, "provenance");
  CHECK(initial >= 0);
  char *provider = field(&r, initial, "provider_id"),
       *engine = field(&r, provenance, "engine_sha256"),
       *model = field(&r, provenance, "model_sha256"),
       *recipe = field(&r, provenance, "recipe"),
       *profile = slice(&r, json_get(&r, initial, "profile"));
  char *embedding_path = field(&c, 0, "embedding_config_file"),
       *embedding_configuration = load(embedding_path, 8192);
  Json e;
  CHECK(
      json_parse(&e, embedding_configuration, strlen(embedding_configuration)));
  char *query_prefix = field(&e, 0, "query_prefix"),
       *document_prefix = field(&e, 0, "document_prefix");
  size_t context = 0;
  CHECK(json_uint(&c, json_get(&c, 0, "context_tokens"), &context));
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(library);
  int (*create)(const char *, const char *, void **) = NULL;
  int (*sdk)(void *, const char *, const char *, char **) = NULL;
  void (*release)(void *) = NULL;
  void (*close_store)(void *) = NULL;
  symbol(library, "chutni_create", &create, sizeof(create));
  symbol(library, "chutni_call", &sdk, sizeof(sdk));
  symbol(library, "chutni_free", &release, sizeof(release));
  symbol(library, "chutni_close", &close_store, sizeof(close_store));
  char store_path[4096], state[4096], managed_state[4096], config_path[4096],
      output_path[4096];
  CHECK(snprintf(store_path, sizeof(store_path), "%s/memory.chutni",
                 output_directory) < (int)sizeof(store_path));
  CHECK(snprintf(state, sizeof(state), "%s/sources", output_directory) <
        (int)sizeof(state));
  CHECK(snprintf(managed_state, sizeof(managed_state), "%s/inference",
                 output_directory) < (int)sizeof(managed_state));
  CHECK(snprintf(config_path, sizeof(config_path), "%s/managed.json",
                 output_directory) < (int)sizeof(config_path));
  void *store = NULL;
  CHECK(!create(store_path, "Actual model Sources query", &store));
  const char *titles[] = {"Backups", "Bread", "Astronomy", "Access"};
  enum { document_count=12, query_count=12 };
  const char *texts[] = {"Restore a deleted file by opening the backup archive "
                         "and recovering the saved copy.",
                         "Combine flour, water and yeast. Knead the dough, let "
                         "it rise, then bake the loaf.",
                         "The moon orbits Earth and reflects sunlight. Its "
                         "phases repeat each month.",
                         "Reset your forgotten password using the account "
                         "recovery link sent to your email address.",
                         "Set an automated daily backup schedule to copy your documents to encrypted cloud storage. Choose a retention policy and verify each scheduled job completed.",
                         "Store baked bread in a sealed bag. Freeze extra slices and reheat them when needed to keep the loaf fresh.",
                         "During a solar eclipse the Moon passes between Earth and the Sun, blocking sunlight for a few minutes along a narrow path.",
                         "Enable two-factor authentication by pairing an authenticator app. Enter its rotating code after your password to protect against unauthorized login.",
                         "Rinse rice, add it to a pot with water, bring it to a boil and simmer with the lid on until the grains are tender.",
                         "Mars is a rocky red planet that orbits the Sun. It has a thin atmosphere, polar ice caps and two small moons.",
                         "Delete large unwanted files and empty the trash to free disk space. Review downloads and remove old temporary files.",
                         "When email delivery is delayed, check the spam folder and allow the sender address. Incorrect mail filters can hide verification messages."};
  const char *queries[] = {
      "How can I get back a document I accidentally erased?",
      "I mistakenly removed a spreadsheet. Where do I recover a saved copy?",
      "What should I open to restore a lost file?",
      "What are the steps for making a loaf?",
      "How do I prepare and bake yeast bread?",
      "What do I do after kneading flour and yeast dough?",
      "Why does our natural satellite change shape in the sky?",
      "What causes the repeating lunar shapes over a month?",
      "Where does moonlight come from and what does the Moon go around?",
      "I cannot remember my login secret. How do I regain access?",
      "I forgot my password. How can email help me get into my account?",
      "How do I reset credentials when I cannot recall them?"};
  const size_t expected[]={0,0,0,1,1,1,2,2,2,3,3,3};
  char *artifacts[document_count];memset(artifacts,0,sizeof(artifacts));
  char *saved = NULL, *saved_request = NULL;
  int item = 1;
  for (size_t k = 0; k < 4; k++, item = r.tokens[item].next) {
    int response = json_get(&r, item, "response");
    char *input = field(&r, response, "input_sha256"),
         *role = field(&r, response, "role"),
         *title = field(&r, response, "title");
    char sha[65];
    hash_bytes(texts[k], strlen(texts[k]), sha);
    CHECK(!strcmp(input, sha) && !strcmp(role, "document") &&
          !strcmp(title, titles[k]));
    free(input);
    free(role);
    free(title);
    char *document_profile = slice(&r, json_get(&r, response, "profile"));
    CHECK(!strcmp(document_profile, profile));
    free(document_profile);
  }
  char root[4096],path[4096];
  CHECK(snprintf(root,sizeof(root),"%s/documents",output_directory)<(int)sizeof(root));
  CHECK(!mkdir(root,0700));
  for(size_t k=0;k<document_count;k++){
    CHECK(snprintf(path,sizeof(path),"%s/%02zu.txt",root,k)<(int)sizeof(path));
    FILE *f=fopen(path,"w");CHECK(f);CHECK(fputs(texts[k],f)>=0);CHECK(!fclose(f));
  }
  char *info = NULL;
  CHECK(!sdk(store, "store_info", "{}", &info));
  Json metadata;
  CHECK(json_parse(&metadata, info, strlen(info)));
  char *store_id = field(&metadata, 0, "store_id");
  release(info);
  close_store(store);
  SourcesEmbeddingConfig embedding = {.inference = {.provider_id = provider,
                                                    .engine_sha256 = engine,
                                                    .model_sha256 = model,
                                                    .call = actual},
                                      .recipe = recipe,
                                      .query_prefix = query_prefix,
                                      .document_prefix = document_prefix,
                                      .context_tokens = context};
  SourcesConfig cfg = {.root = root, .state = state,
                       .owner = "actual-model-qa",
                       .native_helper = argv[2],
                       .embedding = &embedding,
                       .chutni_library = argv[1],
                       .chutni_store = store_path,
                       .chutni_helper = argv[3],
                       .chutni_store_id = store_id,
                       .chutni_semantic_profile = profile,
                       .chutni_publish = 1};
  char *arguments = NULL;
  size_t arguments_bytes = 0;
  FILE *out = open_memstream(&arguments, &arguments_bytes);
  CHECK(out);
  fputs("[\"--config\",", out);
  json_quote(out, config_path, strlen(config_path));
  fputs(",\"--single-current\"]", out);
  CHECK(!fclose(out));
  Transport transport = {.process = {.sources = &cfg,
                                     .executable = inference_binary,
                                     .arguments_json = arguments},
                         .configuration = configuration,
                         .config_path = config_path,
                         .state = managed_state};
  embedding.inference.context = &transport;
  if(controls){
    size_t port=0;CHECK(json_uint(&c,json_get(&c,0,"port"),&port)&&port>0&&port<65536);
    listener_reaped((unsigned)port);
    SourcesConfig limited=cfg; limited.deadline_monotonic=seconds_now()+.2;
    char *capture=NULL;size_t bytes=0;
    const char *error=sources_embedding_generate(&limited,texts[0],"document",NULL,&capture,&bytes);
    CHECK(error&&!strcmp(error,"deadline_exceeded")&&!capture&&!bytes&&transport.calls==1);
    RuntimeStop active={.port=(unsigned)port,.until=seconds_now()+60};
    SourcesConfig stopped=cfg;stopped.cancelled=runtime_active;stopped.cancellation=&active;
    char *created=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"control-index\"}");
    Json j;CHECK(json_parse(&j,created,strlen(created)));char *job=field(&j,0,"job_id");free(created);
    char request[512];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"operation_id\":\"control-read\",\"max_items\":1}",job);
    char *body=call(&cfg,request);free(body);
    FILE *runtime=tmpfile();CHECK(runtime);CHECK(!sources_queue_run(&stopped,100,runtime));CHECK(!fclose(runtime));
    CHECK(active.observed&&!active.expired&&transport.calls==2);
    listener_reaped((unsigned)port);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"index_embedding_status\",\"job_id\":\"%s\"}",job);
    body=call(&cfg,request);CHECK(strstr(body,"\"phase\":\"pending\"")&&strstr(body,"cancelled"));free(body);
    char db_path[4096];CHECK(snprintf(db_path,sizeof(db_path),"%s/sources.sqlite3",state)<(int)sizeof(db_path));
    QueueStop stop={.documents=1,.until=seconds_now()+120};CHECK(sqlite3_open_v2(db_path,&stop.db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);
    CHECK(sqlite3_busy_timeout(stop.db,20)==SQLITE_OK);
    CHECK(sqlite3_prepare_v2(stop.db,"SELECT count(*) FROM index_embeddings WHERE job=? AND status='published'",-1,&stop.count,NULL)==SQLITE_OK);
    CHECK(sqlite3_bind_text(stop.count,1,job,-1,SQLITE_TRANSIENT)==SQLITE_OK);
    SourcesConfig queued=cfg;queued.cancelled=queue_complete;queued.cancellation=&stop;
    runtime=tmpfile();CHECK(runtime);CHECK(!sources_queue_run(&queued,100,runtime));CHECK(!fclose(runtime));CHECK(!stop.failed&&transport.calls==3);
    CHECK(snprintf(path,sizeof(path),"%s/00.txt",root)<(int)sizeof(path));
    FILE *changed=fopen(path,"w");CHECK(changed);CHECK(fputs("Recover deleted files using the saved backup archive and restore the retained copy.",changed)>=0);CHECK(!fclose(changed));
    stop.documents=2;stop.until=seconds_now()+120;queued.discovery_interval_ms=100;
    runtime=tmpfile();CHECK(runtime);CHECK(!sources_queue_run(&queued,100,runtime));CHECK(!fclose(runtime));CHECK(!stop.failed&&transport.calls==4);
    CHECK(sqlite3_finalize(stop.count)==SQLITE_OK);CHECK(sqlite3_close(stop.db)==SQLITE_OK);
    body=call(&cfg,request);CHECK(strstr(body,"\"generation\":0")&&strstr(body,"\"generation\":1"));free(body);free(job);
    listener_reaped((unsigned)port);
    goto cleanup;
  }
  char *created=call(&cfg,"{\"contract_version\":1,\"op\":\"index_create\",\"path\":\"\",\"operation_id\":\"actual-index\"}");
  Json indexed;CHECK(json_parse(&indexed,created,strlen(created)));char *job=field(&indexed,0,"job_id");free(created);
  char index_request[512];
  for(size_t batch=0;batch<document_count/4;batch++){
    snprintf(index_request,sizeof(index_request),"{\"contract_version\":1,\"op\":\"index_step\",\"job_id\":\"%s\",\"max_items\":4,\"operation_id\":\"actual-index-step-%zu\"}",job,batch);
    char *step=call(&cfg,index_request);if(batch+1==document_count/4)CHECK(strstr(step,"\"indexed_items\":12"));free(step);
  }
  snprintf(index_request,sizeof(index_request),"{\"contract_version\":1,\"op\":\"index_embedding_status\",\"job_id\":\"%s\"}",job);
  char *status=call(&cfg,index_request);free(status);
  char db_path[4096];CHECK(snprintf(db_path,sizeof(db_path),"%s/sources.sqlite3",state)<(int)sizeof(db_path));
  QueueStop stop={.documents=document_count};CHECK(sqlite3_open_v2(db_path,&stop.db,SQLITE_OPEN_READONLY,NULL)==SQLITE_OK);
  CHECK(sqlite3_busy_timeout(stop.db,20)==SQLITE_OK);
  CHECK(sqlite3_prepare_v2(stop.db,"SELECT count(*) FROM index_embeddings WHERE job=? AND status='published'",-1,&stop.count,NULL)==SQLITE_OK);
  CHECK(sqlite3_bind_text(stop.count,1,job,-1,SQLITE_TRANSIENT)==SQLITE_OK);
  struct timespec now;CHECK(!clock_gettime(CLOCK_MONOTONIC,&now));stop.until=(double)now.tv_sec+(double)now.tv_nsec/1e9+900;
  SourcesConfig queued=cfg;queued.cancelled=queue_complete;queued.cancellation=&stop;
  FILE *runtime=tmpfile();CHECK(runtime);CHECK(!sources_queue_run(&queued,100,runtime));CHECK(!fclose(runtime));CHECK(!stop.failed);
  CHECK(sqlite3_finalize(stop.count)==SQLITE_OK);CHECK(sqlite3_close(stop.db)==SQLITE_OK);CHECK(transport.calls==document_count);
  status=call(&cfg,index_request);CHECK(json_parse(&indexed,status,strlen(status)));
  int entries=json_get(&indexed,0,"items");CHECK(entries>=0&&indexed.tokens[entries].count==document_count);
  for(int entry=entries+1;entry<indexed.tokens[entries].next;entry=indexed.tokens[entry].next){
    size_t ordinal=document_count;CHECK(json_uint(&indexed,json_get(&indexed,entry,"ordinal"),&ordinal)&&ordinal<document_count&&!artifacts[ordinal]);
    char *phase=field(&indexed,entry,"phase"),*representation=field(&indexed,entry,"representation_id");CHECK(!strcmp(phase,"published")&&strlen(representation)==36);free(phase);free(representation);
    artifacts[ordinal]=field(&indexed,entry,"artifact_id");
  }
  CHECK(snprintf(output_path,sizeof(output_path),"%s/index-embedding-status.json",output_directory)<(int)sizeof(output_path));
  out=fopen(output_path,"w");CHECK(out);CHECK(fputs(status,out)>=0);CHECK(!fclose(out));free(status);free(job);
  for (size_t k = 0; k < query_count; k++) {
    char *request = NULL;
    size_t bytes = 0;
    out = open_memstream(&request, &bytes);
    CHECK(out);
    fprintf(out,
            "{\"contract_version\":1,\"op\":\"memory_search_text\",\"operation_"
            "id\":\"actual-query-%zu\",\"timeout_ms\":60000,\"query\":",
            k);
    json_quote(out, queries[k], strlen(queries[k]));
    fputc('}', out);
    CHECK(!fclose(out));
    char *response = call(&cfg, request);
    Json result;
    CHECK(json_parse(&result, response, strlen(response)));
    int memory = json_get(&result, 0, "memory"),
        items = json_get(&result, json_get(&result, memory, "result"), "items");
    CHECK(items >= 0 && result.tokens[items].count == 8);
    char *top = field(&result, items + 1, "external_artifact_id"),
         *score = field(&result, items + 1, "score_type");
    if(strcmp(top,artifacts[expected[k]]))fprintf(stderr,"retrieval miss query %zu (%s), expected document %zu: %s\n",k,queries[k],expected[k],response);
    CHECK(!strcmp(top, artifacts[expected[k]]) && !strcmp(score, "cosine_bruteforce"));
    free(top);
    free(score);
    CHECK(json_get(&result, memory, "query_embedding") >= 0);
    CHECK(snprintf(output_path, sizeof(output_path), "%s/query-%zu.json",
                   output_directory, k) < (int)sizeof(output_path));
    out = fopen(output_path, "w");
    CHECK(out &&
          fwrite(response, 1, strlen(response), out) == strlen(response) &&
          !fclose(out));
    if (!k) {
      saved = response;
      saved_request = request;
    } else {
      free(response);
      free(request);
    }
  }
  configure(&transport);
  char source_config_path[4096], profile_path[4096], arguments_path[4096];
  CHECK(snprintf(source_config_path, sizeof(source_config_path),
                 "%s/embedding.json",
                 output_directory) < (int)sizeof(source_config_path));
  CHECK(snprintf(profile_path, sizeof(profile_path), "%s/profile.json",
                 output_directory) < (int)sizeof(profile_path));
  CHECK(snprintf(arguments_path, sizeof(arguments_path), "%s/arguments.json",
                 output_directory) < (int)sizeof(arguments_path));
  out = fopen(source_config_path, "w");
  CHECK(out);
  CHECK(!chmod(source_config_path, 0600));
  fputs("{\"provider_id\":", out);
  json_quote(out, provider, strlen(provider));
  fprintf(out, ",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"recipe\":",
          engine, model);
  json_quote(out, recipe, strlen(recipe));
  fputs(",\"query_prefix\":", out);
  json_quote(out, query_prefix, strlen(query_prefix));
  fputs(",\"document_prefix\":", out);
  json_quote(out, document_prefix, strlen(document_prefix));
  fprintf(out, ",\"context_tokens\":%zu}", context);
  CHECK(!fclose(out));
  out = fopen(profile_path, "w");
  CHECK(out);
  CHECK(!chmod(profile_path, 0600));
  CHECK(fwrite(profile, 1, strlen(profile), out) == strlen(profile));
  CHECK(!fclose(out));
  out = fopen(arguments_path, "w");
  CHECK(out);
  CHECK(!chmod(arguments_path, 0600));
  CHECK(fwrite(arguments, 1, strlen(arguments), out) == strlen(arguments));
  CHECK(!fclose(out));
  const char *flags[] = {"--state",
                         state,
                         "--owner",
                         "actual-model-qa",
                         "--chutni-library",
                         argv[1],
                         "--chutni-store",
                         store_path,
                         "--chutni-helper",
                         argv[3],
                         "--chutni-store-id",
                         store_id,
                         "--chutni-semantic-profile-file",
                         profile_path,
                         "--embedding-inference-config-file",
                         source_config_path,
                         "--embedding-inference-executable",
                         inference_binary,
                         "--embedding-inference-arguments-file",
                         arguments_path};
  char *cli_arguments = NULL;
  size_t cli_bytes = 0;
  out = open_memstream(&cli_arguments, &cli_bytes);
  CHECK(out);
  fputc('[', out);
  for (size_t k = 0; k < sizeof(flags) / sizeof(flags[0]); k++) {
    if (k)
      fputc(',', out);
    json_quote(out, flags[k], strlen(flags[k]));
  }
  fputc(']', out);
  CHECK(!fclose(out));
  char *cli_request = NULL;
  size_t request_bytes = 0;
  out = open_memstream(&cli_request, &request_bytes);
  CHECK(out);
  fputs("{\"contract_version\":1,\"op\":\"memory_search_text\",\"operation_"
        "id\":\"actual-cli-query\",\"timeout_ms\":60000,\"query\":",
        out);
  json_quote(out, queries[0], strlen(queries[0]));
  fputc('}', out);
  CHECK(!fclose(out));
  SourcesInferenceProcess cli = {
      .sources = &cfg, .executable = argv[2], .arguments_json = cli_arguments};
  SourcesInferenceControl cli_control = {0};
  char *cli_response = NULL;
  size_t cli_response_bytes = 0;
  const char *cli_error = sources_inference_process_call(
      &cli, &cli_control, cli_request, request_bytes, &cli_response,
      &cli_response_bytes);
  if (cli_error)
    fprintf(stderr, "CLI transport: %s\n", cli_error);
  CHECK(!cli_error && cli_response);
  Json cli_result;
  CHECK(json_parse(&cli_result, cli_response, cli_response_bytes));
  int cli_memory = json_get(&cli_result, 0, "memory"),
      cli_items = json_get(
          &cli_result, json_get(&cli_result, cli_memory, "result"), "items");
  if (cli_items < 0)
    fprintf(stderr, "CLI response: %s\n", cli_response);
  CHECK(cli_items >= 0 && cli_result.tokens[cli_items].count == 8);
  char *cli_top = field(&cli_result, cli_items + 1, "external_artifact_id");
  CHECK(!strcmp(cli_top, artifacts[0]));
  free(cli_top);
  CHECK(snprintf(output_path, sizeof(output_path), "%s/cli-query.json",
                 output_directory) < (int)sizeof(output_path));
  out = fopen(output_path, "w");
  CHECK(out &&
        fwrite(cli_response, 1, cli_response_bytes, out) ==
            cli_response_bytes &&
        !fclose(out));
  free(cli_arguments);
  free(cli_request);
  free(cli_response);
  unsigned calls = transport.calls;
  embedding.inference.call = NULL;
  cfg.chutni_library = NULL;
  cfg.chutni_helper = NULL;
  char *replay = call(&cfg, saved_request);
  CHECK(!strcmp(replay, saved) && transport.calls == calls);
  free(replay);
cleanup:
  free(saved);
  free(saved_request);
  for (size_t k = 0; k < document_count; k++)
    free(artifacts[k]);
  free(configuration);
  free(receipts);
  free(provider);
  free(engine);
  free(model);
  free(recipe);
  free(profile);
  free(embedding_path);
  free(embedding_configuration);
  free(query_prefix);
  free(document_prefix);
  free(store_id);
  free(arguments);
  CHECK(!dlclose(library));
  if(controls){printf("PASS actual-model Sources controls: %u checks; native deadline, active-listener queue cancellation, healthy retry, automatic refresh/reindex and retained publication; %s\n",checks,output_directory);return 0;}
  printf("PASS actual-model Sources text query: %u checks; twelve indexed documents and twelve paraphrase queries through fixed managed configuration; "
         "embeddings, actual Chutni rankings, CLI execution, retained "
         "provenance, offline "
         "Sources replay; indexed source files, durable queued generation and native bound publication; %s\n",
         checks, output_directory);
  return 0;
}
