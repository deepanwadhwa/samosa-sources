#include "visual_fixture.h"
#include "temporal.h"
#include "temporal_fixture.h"
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--sources-native-worker"))
        return sources_native_worker_main(argc, argv);
    CHECK(argc == 3);
    alarm(getenv("SAMOSA_TEMPORAL_FULL_FIXTURE")?600:180);
    char root[] = "/tmp/samosa-visual-capture-XXXXXX";
    CHECK(mkdtemp(root));
    char state[512], path[512], database[600];
    snprintf(state, sizeof(state), "%s/state", root);
    snprintf(path, sizeof(path), "%s/video.mp4", root);
    snprintf(database, sizeof(database), "%s/sources.sqlite3", state);
    FILE *from = fopen(argv[2], "rb"), *to = fopen(path, "wb");
    CHECK(from && to);
    unsigned char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), from)))
        CHECK(fwrite(buffer, 1, n, to) == n);
    CHECK(!ferror(from) && !fclose(from) && !fclose(to));
    char engine[65], model[65];
    memset(engine, 'a', 64);
    memset(model, 'b', 64);
    engine[64] = model[64] = 0;
    Fixture fixture = {.mode = "ok"};
    SourcesInferenceConfig inference = {.provider_id = "fixture-vision-v1",
                                        .engine_sha256 = engine,
                                        .model_sha256 = model,
                                        .call = transport,
                                        .context = &fixture};
    SourcesConfig cfg = {.root = root,
                         .state = state,
                         .owner = "alice",
                         .media_helper = argv[1],
                         .visual_inference = &inference};
    fixture.inference = &inference;
    fixture.sources = &cfg;
    const char *alpha_frame =
        "{\"fingerprint\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"stream\":0,\"width\":2,\"height\":1,\"source_width\":2,\"source_height\":1,\"timestamp_"
        "us\":1000000,\"origin_us\":0,\"colorspace\":5,\"color_range\":1,\"start_ms\":1000,\"end_"
        "ms\":1500,\"rgba_bytes\":8,\"orientation\":0,\"source_transfer\":13,\"source_primaries\":"
        "1,\"output_transfer\":13,\"output_primaries\":1,\"tone_mapped\":0,\"pixel_format\":"
        "\"rgba8\",\"encoding\":\"base64\",\"data\":\"/wAA/wAAAAA=\"}";
    char rgba_hash[65], ppm_hash[65], expected_hash[65];
    CHECK(!sources_media_frame_hashes(&cfg, alpha_frame, strlen(alpha_frame), 1000, 1500, rgba_hash,
                                      ppm_hash));
    const unsigned char alpha_pixels[] = {255, 0, 0, 255, 0, 0, 0, 0};
    hash_bytes(alpha_pixels, sizeof(alpha_pixels), expected_hash);
    CHECK(!strcmp(rgba_hash, expected_hash));
    const unsigned char ppm_pixels[] = {'P', '6',  '\n', '2', ' ', '1', '\n', '2', '5',
                                        '5', '\n', 255,  0,   0,   255, 255,  255};
    hash_bytes(ppm_pixels, sizeof(ppm_pixels), expected_hash);
    CHECK(!strcmp(ppm_hash, expected_hash));
    char *body = call(&cfg, "{\"contract_version\":1,\"op\":\"describe\"}", NULL);
    CHECK(strstr(body, "analyze_visual") &&
          strstr(body, "\"visual_inference_adapter_configured\":true"));
    free(body);
    body = call(&cfg, "{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"video.mp4\"}",
                NULL);
    char *source = field(body, "source_id");
    free(body);
char inspection[512];
snprintf(inspection,sizeof(inspection),"{\"contract_version\":1,\"op\":\"inspect_visual\",\"source_id\":\"%s\",\"version\":\"%s\",\"operation_id\":\"visual-inspection\",\"timeout_ms\":10000}",source,source);
body=call(&cfg,inspection,NULL);
CHECK(strstr(body,"\"duration_ms\":2000")&&strstr(body,"container_floor_ms")&&strstr(body,"media_inspection_only"));
CHECK(strstr(body,"\"source_width\":640")&&strstr(body,"\"source_height\":360")&&!strstr(body,"rgba_bytes"));
char *inspection_saved=strdup(body);CHECK(inspection_saved);free(body);
SourcesConfig probe_offline=cfg;probe_offline.media_helper=NULL;
body=call(&probe_offline,inspection,NULL);CHECK(!strcmp(body,inspection_saved));free(body);free(inspection_saved);
SourcesConfig probe_foreign=cfg;probe_foreign.owner="foreign-visual-probe";
snprintf(inspection,sizeof(inspection),"{\"contract_version\":1,\"op\":\"inspect_visual\",\"source_id\":\"%s\",\"version\":\"%s\"}",source,source);
body=call(&probe_foreign,inspection,"unknown_source");free(body);
CHECK(fixture.calls==0);
    FILE *video_file=fopen(path,"rb");CHECK(video_file);CHECK(!fseek(video_file,0,SEEK_END));
    long video_length=ftell(video_file);CHECK(video_length>0&&video_length<67108864);
    CHECK(!fseek(video_file,0,SEEK_SET));unsigned char *video_bytes=malloc((size_t)video_length);CHECK(video_bytes);
    CHECK(fread(video_bytes,1,(size_t)video_length,video_file)==(size_t)video_length);CHECK(!fclose(video_file));
    char decoder_pin[65];CHECK(!sources_native_media_fingerprint(&cfg,decoder_pin));
    for(size_t boundary=1000;boundary<=2000;boundary+=1000) {
        char *last_body=NULL;size_t last_bytes=0;
        CHECK(!sources_native_media_last_frame(&cfg,video_bytes,(size_t)video_length,0,boundary,decoder_pin,&last_body,&last_bytes));
        Json last_json;CHECK(json_parse(&last_json,last_body,last_bytes));size_t last_timestamp=0;
        CHECK(json_uint(&last_json,json_get(&last_json,0,"timestamp_us"),&last_timestamp));
        CHECK(last_timestamp<boundary*1000&&last_timestamp>=boundary*1000-100000);
        free(last_body);
    }
    free(video_bytes);
    SourcesConfig temporal_cfg=cfg;temporal_cfg.visual_inference=NULL;
    temporal_cfg.temporal_inference=&inference;
    char preparation_request[512];char *prepared_frames[2];size_t prepared_lengths[2];
    for(size_t k=0;k<2;k++) {
        snprintf(preparation_request,sizeof(preparation_request),
            "{\"contract_version\":1,\"op\":\"prepare_visual\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu}",source,source,k*1000,k*1000+500);
        prepared_frames[k]=call(&temporal_cfg,preparation_request,NULL);
        prepared_lengths[k]=strlen(prepared_frames[k]);
    }
    SourcesTemporalInput temporal_input;char *temporal_request=NULL;size_t temporal_bytes=0;
    CHECK(!sources_temporal_input(&temporal_cfg,(const char *const *)prepared_frames,prepared_lengths,2,0,2000,"Describe changes.",&temporal_input,&temporal_request,&temporal_bytes));
    CHECK(temporal_input.count==2&&temporal_input.frames[0].timestamp<temporal_input.frames[1].timestamp);
    CHECK(temporal_bytes>1048576u&&strstr(temporal_request,"analyze_video_frames")&&strstr(temporal_request,"120000"));
    CHECK(!strstr(temporal_request,"decoder")&&!strstr(temporal_request,"locator"));
    char manifest[65],temporal_model[65],material[96];memset(manifest,'c',64);manifest[64]=0;
    int material_length=snprintf(material,sizeof(material),"molmo2-package-v1:%s",manifest);
    hash_bytes(material,(size_t)material_length,temporal_model);
    SourcesInferenceConfig temporal_provider=inference;temporal_provider.model_sha256=temporal_model;
    temporal_cfg.temporal_inference=&temporal_provider;
    char *temporal_reply=NULL;size_t temporal_reply_bytes=0;FILE *reply_stream=open_memstream(&temporal_reply,&temporal_reply_bytes);CHECK(reply_stream);
    fprintf(reply_stream,"{\"contract_version\":1,\"provider_id\":\"%s\",\"kind\":\"derived\",\"artifact_type\":\"video_observation\",\"input_sha256\":\"%s\",\"text\":\"The sampled scene changes.\",\"range\":{\"start_us\":0,\"end_us\":2000000,\"coordinate_space\":\"input_video_us\"},\"frames\":[",temporal_provider.provider_id,temporal_input.input_sha256);
    for(size_t k=0;k<temporal_input.count;k++) {
        SourcesPreparedFrame *frame=&temporal_input.frames[k];
        fprintf(reply_stream,"%s{\"timestamp_us\":%zu,\"width\":%zu,\"height\":%zu,\"rgba_sha256\":\"%s\",\"prepared_sha256\":\"%s\"}",k?",":"",frame->timestamp,frame->width,frame->height,frame->input,frame->ppm);
    }
    fprintf(reply_stream,"],\"usage\":{\"input_tokens\":200,\"output_tokens\":8},\"coverage\":{\"status\":\"partial\",\"reason\":\"sampled_video_analysis_uncertain\"},\"provenance\":{\"engine\":\"molmo2\",\"engine_sha256\":\"%s\",\"model_sha256\":\"%s\",\"manifest_sha256\":\"%s\",\"prepared_frames_sha256\":\"%s\",\"alpha_policy\":\"rgba8_composited_on_white\",\"timestamp_policy\":\"molmo2_one_decimal_seconds\",\"coordinate_space\":\"input_video_us\"}}",engine,temporal_model,manifest,temporal_input.prepared_sha256);
    CHECK(!fclose(reply_stream));
    CHECK(!sources_temporal_response(&temporal_cfg,&temporal_input,temporal_reply,temporal_reply_bytes));
    temporal_input.frames[0].timestamp++;
    CHECK(!strcmp(sources_temporal_response(&temporal_cfg,&temporal_input,temporal_reply,temporal_reply_bytes),"inference_response_invalid"));
    temporal_input.frames[0].timestamp--;
    temporal_provider.model_sha256=model;
    CHECK(!strcmp(sources_temporal_response(&temporal_cfg,&temporal_input,temporal_reply,temporal_reply_bytes),"inference_response_invalid"));
    temporal_provider.model_sha256=temporal_model;
    char *manifest_in_reply=strstr(temporal_reply,manifest);CHECK(manifest_in_reply);manifest_in_reply[0]='d';
    CHECK(!strcmp(sources_temporal_response(&temporal_cfg,&temporal_input,temporal_reply,temporal_reply_bytes),"inference_response_invalid"));
    free(temporal_reply);
    char temporal_digest[65];memcpy(temporal_digest,temporal_input.input_sha256,65);
    free(temporal_request);sources_temporal_input_release(&temporal_input);
    CHECK(!sources_temporal_input(&temporal_cfg,(const char *const *)prepared_frames,prepared_lengths,2,0,1500,"Describe changes.",&temporal_input,&temporal_request,&temporal_bytes));
    CHECK(strcmp(temporal_digest,temporal_input.input_sha256));
    free(temporal_request);sources_temporal_input_release(&temporal_input);
    const char *repeated[]={prepared_frames[0],prepared_frames[0]};size_t repeated_lengths[]={prepared_lengths[0],prepared_lengths[0]};
    const char *temporal_error=sources_temporal_input(&temporal_cfg,repeated,repeated_lengths,2,0,2000,"Describe changes.",&temporal_input,&temporal_request,&temporal_bytes);
    CHECK(temporal_error&&!strcmp(temporal_error,"invalid_range")&&!temporal_request&&!temporal_input.count);
    temporal_error=sources_temporal_input(&temporal_cfg,(const char *const *)prepared_frames,prepared_lengths,17,0,2000,"Describe changes.",&temporal_input,&temporal_request,&temporal_bytes);
    CHECK(temporal_error&&!strcmp(temporal_error,"invalid_request"));
    temporal_error=sources_temporal_input(&temporal_cfg,(const char *const *)prepared_frames,prepared_lengths,2,1000,2000,"Describe changes.",&temporal_input,&temporal_request,&temporal_bytes);
    CHECK(temporal_error&&!strcmp(temporal_error,"invalid_range"));
    free(prepared_frames[0]);free(prepared_frames[1]);
    TemporalFixture video_fixture={.provider=&temporal_provider,.manifest=manifest};
    temporal_provider.call=temporal_transport;temporal_provider.context=&video_fixture;
    char video_request[1024];
    snprintf(video_request,sizeof(video_request),"{\"contract_version\":1,\"op\":\"analyze_video\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":2000,\"operation_id\":\"temporal-capture\",\"parent_operation_id\":\"video-parent\",\"timeout_ms\":120000}",source,source);
    char *video_saved=call(&temporal_cfg,video_request,NULL);CHECK(video_fixture.calls==1);
    CHECK(strstr(video_saved,"source-temporal-uniform-last-v1")&&strstr(video_saved,"visual-media-last-frame-v1")&&strstr(video_saved,"source_time_us"));
    CHECK(!strstr(video_saved,"rgba_base64")&&!strstr(video_saved,"\"data\"")&&!strstr(video_saved,"evidence_id"));
    SourcesConfig video_offline=temporal_cfg;video_offline.media_helper=NULL;
    SourcesInferenceConfig offline_provider=temporal_provider;offline_provider.call=NULL;video_offline.temporal_inference=&offline_provider;
    body=call(&video_offline,video_request,NULL);CHECK(!strcmp(body,video_saved)&&video_fixture.calls==1);free(body);
    snprintf(video_request,sizeof(video_request),"{\"contract_version\":1,\"op\":\"analyze_video\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":2000}",source,source);
    body=call(&video_offline,video_request,NULL);CHECK(strstr(body,"These sampled frames")&&video_fixture.calls==1);
    char *video_plain=strdup(body),*video_artifact=field(body,"artifact_id");CHECK(video_plain);free(body);
    sqlite3 *video_db=NULL;CHECK(sqlite3_open(database,&video_db)==SQLITE_OK);
    char *damaged=strdup(video_plain);CHECK(damaged);char *timestamp_field=strstr(damaged,"\"timestamp_us\":");CHECK(timestamp_field);
    timestamp_field+=strlen("\"timestamp_us\":");CHECK(*timestamp_field=='0');*timestamp_field='1';
    replace(video_db,video_artifact,damaged);free(damaged);
    body=call(&video_offline,video_request,"corrupt_cache");free(body);CHECK(video_fixture.calls==1);
    replace(video_db,video_artifact,video_plain);CHECK(!sqlite3_close(video_db));free(video_artifact);free(video_plain);
    SourcesConfig changed_video=video_offline;SourcesInferenceConfig changed_provider=offline_provider;
    changed_provider.model_sha256=model;changed_video.temporal_inference=&changed_provider;
    body=call(&changed_video,video_request,"inference_unavailable");free(body);CHECK(video_fixture.calls==1);
    SourcesConfig video_foreign=video_offline;video_foreign.owner="foreign-video";body=call(&video_foreign,video_request,"unknown_source");free(body);
    video_fixture.mode="timestamp";
    snprintf(video_request,sizeof(video_request),"{\"contract_version\":1,\"op\":\"analyze_video\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":2000,\"prompt\":\"Invalid timestamp fixture.\"}",source,source);
    body=call(&temporal_cfg,video_request,"inference_response_invalid");free(body);CHECK(video_fixture.calls==2);
    free(video_saved);
    const char *full_temporal=getenv("SAMOSA_TEMPORAL_FULL_FIXTURE");
    if(full_temporal&&*full_temporal) {
        fputs("QA temporal16: native sampling/capture begins\n",stderr);
        char full_path[512];snprintf(full_path,sizeof(full_path),"%s/full-video.mp4",root);
        FILE *full_from=fopen(full_temporal,"rb"),*full_to=fopen(full_path,"wb");CHECK(full_from&&full_to);
        while((n=fread(buffer,1,sizeof(buffer),full_from)))CHECK(fwrite(buffer,1,n,full_to)==n);
        CHECK(!ferror(full_from)&&!fclose(full_from)&&!fclose(full_to));
        body=call(&temporal_cfg,"{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"full-video.mp4\"}",NULL);
        char *full_source=field(body,"source_id");free(body);video_fixture.mode=NULL;
        snprintf(video_request,sizeof(video_request),"{\"contract_version\":1,\"op\":\"analyze_video\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":0,\"end_ms\":8000}",full_source,full_source);
        body=call(&temporal_cfg,video_request,NULL);fputs("QA temporal16: native sampling/capture completed\n",stderr);Json full_capture;CHECK(json_parse(&full_capture,body,strlen(body)));
        int full_frames=json_get(&full_capture,0,"frames");CHECK(full_frames>=0&&full_capture.tokens[full_frames].count==16);
        CHECK(strstr(body,"visual-media-last-frame-v1")&&!strstr(body,"rgba_base64"));free(body);
        CHECK(!unlink(full_path));body=call(&video_offline,video_request,NULL);CHECK(strstr(body,"These sampled frames"));free(body);free(full_source);
    }
const char *valid_probe="{\"fingerprint\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"stream\":0,\"source_width\":640,\"source_height\":360,\"origin_us\":0,\"duration_ms\":2000,\"duration_policy\":\"container_floor_ms\"}";
CHECK(!sources_video_probe_validate(&cfg,valid_probe,strlen(valid_probe)));
const char *bad_probes[]={"{}", "{\"duration_ms\":14400001}", "null"};
for(size_t p=0;p<sizeof(bad_probes)/sizeof(bad_probes[0]);p++)
    CHECK(!strcmp(sources_video_probe_validate(&cfg,bad_probes[p],strlen(bad_probes[p])),"native_helper_protocol"));
SourcesConfig unconfigured = cfg;

    unconfigured.visual_inference = NULL;
    char *unconfigured_request = request(source, 1000, 1500, NULL, NULL);
    body = call(&unconfigured, unconfigured_request, "inference_unconfigured");
    CHECK(fixture.calls == 0);
    free(body);
    free(unconfigured_request);
    char *req = request(source, 1000, 1500, NULL, "first-visual"), *saved = call(&cfg, req, NULL);
    CHECK(fixture.calls == 1 && strstr(saved, "A colored frame.") &&
          strstr(saved, "visual_analysis_uncertain") && strstr(saved, "source_time_us") &&
          strstr(saved, "native_decode_scale_rgba") && strstr(saved, "visual-parent") &&
          strstr(saved, "selected_frame_only"));
    CHECK(!strstr(saved, "evidence_id") && !strstr(saved, "\"data\""));
    body = call(&cfg, req, NULL);
    CHECK(!strcmp(body, saved) && fixture.calls == 1);
    free(body);
    CHECK(!unlink(path));
    SourcesConfig offline = cfg;
    offline.root = NULL;
    offline.media_helper = NULL;
    inference.call = NULL;
    body = call(&offline, req, NULL);
    CHECK(!strcmp(body, saved) && fixture.calls == 1);
    free(body);
    free(req);
    offline.media_helper = cfg.media_helper;
    req = request(source, 1000, 1500, NULL, NULL);
    char *plain = call(&offline, req, NULL);
    CHECK(fixture.calls == 1 && !sources_visual_capture_validate(&offline, plain, strlen(plain)));
    SourcesConfig foreign = offline;
    foreign.owner = "bob";
    body = call(&foreign, req, "unknown_source");
    CHECK(fixture.calls == 1);
    free(body);
    char *artifact = field(plain, "artifact_id");
    free(req);
    inference.call = transport;
    const char *modes[] = {"wrong-input", "wrong-ppm",   "wrong-model", "wrong-provider",
                           "literal",     "wrong-width", "empty",       "over-budget",
                           "full",        "wrong-alpha", "unavailable", "busy"};
    for (size_t k = 0; k < sizeof(modes) / sizeof(*modes); k++) {
        fixture.mode = modes[k];
        req = request(source, 1000, 1500, modes[k], NULL);
        body = call(&offline, req,
                    k == 10   ? "inference_unavailable"
                    : k == 11 ? "inference_busy"
                              : "inference_response_invalid");
        free(body);
        size_t previous = fixture.calls;
        fixture.mode = "ok";
        body = call(&offline, req, NULL);
        CHECK(fixture.calls == previous + 1);
        free(body);
        free(req);
    }
    fixture.mode = "cancel";
    req = request(source, 1500, 1600, "cancel", "cancel-visual");
    body = call(&offline, req, "cancelled");
    free(body);
    fixture.mode = "ok";
    body = call(&offline, req, "cancelled");
    free(body);
    free(req);
    req = request(source, 1500, 1600, "cancel", NULL);
    size_t previous = fixture.calls;
    body = call(&offline, req, NULL);
    CHECK(fixture.calls == previous + 1);
    free(body);
    free(req);
    fixture.mode = "deadline";
    SourcesConfig deadline = offline;
    struct timespec now;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &now));
    deadline.deadline_monotonic = (double)now.tv_sec + (double)now.tv_nsec / 1e9 + 0.03;
    req = request(source, 1600, 1700, "deadline", NULL);
    body = call(&deadline, req, "deadline_exceeded");
    free(body);
    fixture.mode = "ok";
    body = call(&offline, req, NULL);
    free(body);
    free(req);
    SourcesConfig limited = offline;
    limited.cache_limit = 1;
    req = request(source, 1000, 1500, "new quota recipe", NULL);
    previous = fixture.calls;
    body = call(&limited, req, "cache_limit");
    CHECK(fixture.calls == previous);
    free(body);
    free(req);
    char changed[65];
    memset(changed, 'c', 64);
    changed[64] = 0;
    inference.model_sha256 = changed;
    req = request(source, 1000, 1500, NULL, NULL);
    previous = fixture.calls;
    body = call(&offline, req, NULL);
    char *other = field(body, "artifact_id");
    CHECK(fixture.calls == previous + 1 && strcmp(other, artifact));
    free(other);
    free(body);
    free(req);
    inference.model_sha256 = model;
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(database, &db) == SQLITE_OK);
    size_t before = count(db);
    req = request(source, 1800, 1900, "transient window", NULL);
    Json j;
    CHECK(json_parse(&j, req, strlen(req)));
    char *transient = NULL;
    size_t transient_bytes = 0;
    FILE *out = open_memstream(&transient, &transient_bytes);
    CHECK(out && !sources_visual_window(&offline, &j, db, out) && !fclose(out));
    CHECK(count(db) == before &&
          !sources_visual_capture_validate(&offline, transient, transient_bytes));
    free(transient);
    free(req);
    char *forged = strdup(plain), *pin = strstr(forged, model);
    CHECK(pin);
    memcpy(pin, engine, 64);
    replace(db, artifact, forged);
    free(forged);
    req = request(source, 1000, 1500, NULL, NULL);
    previous = fixture.calls;
    body = call(&offline, req, "corrupt_cache");
    CHECK(fixture.calls == previous);
    free(body);
    replace(db, artifact, plain);
    sqlite3_stmt *s = NULL;
    CHECK(sqlite3_prepare_v2(db,
                             "UPDATE prepared_artifacts SET digest='damaged' WHERE "
                             "json_extract(response,'$.artifact_id')=?",
                             -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, artifact, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(s) == SQLITE_DONE && sqlite3_changes(db) == 1);
    sqlite3_finalize(s);
    body = call(&offline, req, "corrupt_cache");
    CHECK(fixture.calls == previous);
    free(body);
    free(req);
    CHECK(!sqlite3_close(db));
    free(artifact);
    free(plain);
    free(source);
    free(saved);
    CHECK(!nftw(root, remove_file, 32, FTW_DEPTH | FTW_PHYS));
    printf("PASS Sources visual captures: %u checks; actual native frames, public Inference "
           "validation, source timeline/decoder provenance, "
           "cache/owner/replay/model/quota/corruption, cancellation/deadline/transient buffers; "
           "fixture model only\n",
           checks);
    return 0;
}
