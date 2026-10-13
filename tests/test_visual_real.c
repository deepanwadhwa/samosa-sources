#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "json.h"
#include "sha256.h"
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
static void file_sha(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    CHECK(f);
    Sha256 h;
    sha256_init(&h);
    unsigned char block[65536];
    size_t n;
    while ((n = fread(block, 1, sizeof(block), f)))
        sha256_update(&h, block, n);
    CHECK(!ferror(f) && !fclose(f));
    unsigned char digest[32];
    sha256_final(&h, digest);
    sha256_hex(digest, out);
}
static char *invoke(char *const args[], const char *request) {
    FILE *input = tmpfile(), *output = tmpfile();
    CHECK(input && output && fwrite(request, 1, strlen(request), input) == strlen(request) &&
          !fflush(input) && !fseek(input, 0, SEEK_SET));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        if (dup2(fileno(input), 0) < 0 || dup2(fileno(output), 1) < 0)
            _exit(2);
        execv(args[0], args);
        _exit(2);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(!fseek(output, 0, SEEK_END));
    long n = ftell(output);
    CHECK(n > 0 && n <= 2097152 && !fseek(output, 0, SEEK_SET));
    char *body = malloc((size_t)n + 1);
    CHECK(body && fread(body, 1, (size_t)n, output) == (size_t)n);
    body[n] = 0;
    CHECK(!fclose(input) && !fclose(output));
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        fprintf(stderr, "response: %s\n", body);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    return body;
}
static char *field(const char *body, const char *name) {
    Json j;
    CHECK(json_parse(&j, body, strlen(body)));
    char *value = json_string(&j, json_get(&j, 0, name));
    CHECK(value);
    return value;
}
static int remove_file(const char *path, const struct stat *info, int type, struct FTW *walk) {
    (void)info;
    (void)type;
    (void)walk;
    return remove(path);
}
static int temporal_real(int argc,char **argv) {
    CHECK(argc==9);alarm(300);CHECK(argv[7][0]=='/'&&!mkdir(argv[7],0700));
    size_t start_ms=0;const char *start_setting=getenv("SAMOSA_TEST_TEMPORAL_START_MS");
    if(start_setting){char *end_pointer=NULL;errno=0;unsigned long long parsed=strtoull(start_setting,&end_pointer,10);CHECK(!errno&&end_pointer&&!*end_pointer&&parsed<=14398000);start_ms=(size_t)parsed;}
    int full=getenv("SAMOSA_TEST_TEMPORAL_FULL")!=NULL;size_t duration_ms=full?8000u:2000u,expected_frames=full?16u:4u;
    CHECK(start_ms+duration_ms<=14400000u);
    char state[4096],admission[4096],arguments[4096],video[4096],path[4096],response_path[4096];
    CHECK(snprintf(state,sizeof(state),"%s/state",argv[7])<(int)sizeof(state));
    CHECK(snprintf(admission,sizeof(admission),"%s/admission",argv[7])<(int)sizeof(admission));
    CHECK(snprintf(arguments,sizeof(arguments),"%s/inference-args.json",argv[7])<(int)sizeof(arguments));
    CHECK(snprintf(video,sizeof(video),"%s/colors.mp4",argv[7])<(int)sizeof(video));
    FILE *from=fopen(argv[6],"rb"),*to=fopen(video,"wb");CHECK(from&&to);unsigned char block[65536];size_t n;
    while((n=fread(block,1,sizeof(block),from)))CHECK(fwrite(block,1,n,to)==n);
    CHECK(!ferror(from)&&!fclose(from)&&!fclose(to)&&!chmod(video,0600));
    char engine[65],manifest[65],model[65],material[96];file_sha(argv[4],engine);
    CHECK(snprintf(path,sizeof(path),"%s/manifest.json",argv[5])<(int)sizeof(path));file_sha(path,manifest);
    int m=snprintf(material,sizeof(material),"molmo2-package-v1:%s",manifest);CHECK(m>0&&(size_t)m<sizeof(material));hash_bytes(material,(size_t)m,model);
    int fd=open(arguments,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(fd>=0);FILE *out=fdopen(fd,"w");CHECK(out);
    const char *private_args[]={"--temporal-engine",argv[4],"--temporal-model",argv[5],"--temporal-engine-sha256",engine,"--temporal-manifest-sha256",manifest,"--admission-state",admission,"--owner","temporal-real-owner"};
    fputc('[',out);for(size_t k=0;k<sizeof(private_args)/sizeof(*private_args);k++){if(k)fputc(',',out);json_quote(out,private_args[k],strlen(private_args[k]));}fputc(']',out);CHECK(!fclose(out));
    char *args[]={argv[1],"--root",argv[7],"--state",state,"--owner","temporal-real-owner","--media-helper",argv[2],"--temporal-inference-executable",argv[3],"--temporal-inference-arguments-file",arguments,"--temporal-inference-provider-id","molmo2-native-v1","--temporal-inference-engine-sha256",engine,"--temporal-inference-model-sha256",model,NULL};
    char *body=invoke(args,"{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"colors.mp4\"}"),*source=field(body,"source_id");free(body);
    char request[1024];snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"analyze_video\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu,\"prompt\":\"Describe the color change across these video frames in temporal order.\",\"operation_id\":\"real-temporal-capture\",\"parent_operation_id\":\"real-video-parent\",\"timeout_ms\":120000}",source,source,start_ms,start_ms+duration_ms);
    char *saved=invoke(args,request),*text=field(saved,"text");
    char *lower=strdup(text);CHECK(lower);for(char *p=lower;*p;p++)*p=(char)tolower((unsigned char)*p);
    char *red=strstr(lower,"red"),*blue=strstr(lower,"blue");if(!red||!blue||red>=blue)fprintf(stderr,"observation: %s\n",text);CHECK(red&&blue&&red<blue);free(lower);
    CHECK(strstr(saved,"sampled_video_analysis_uncertain")&&strstr(saved,"source_time_us")&&strstr(saved,"visual-media-last-frame-v1"));
    CHECK(!strstr(saved,"rgba_base64")&&!strstr(saved,"\"data\"")&&!strstr(saved,"evidence_id"));
    Json capture;CHECK(json_parse(&capture,saved,strlen(saved)));int frames=json_get(&capture,0,"frames");CHECK(frames>=0&&capture.tokens[frames].count==(int)expected_frames);
    FILE *reference=fopen(argv[8],"rb");CHECK(reference);size_t frame_size=512u*512u*4u;unsigned char *pixels=malloc(frame_size);CHECK(pixels);size_t k=0;
    for(int t=frames+1;t<capture.tokens[frames].next;t=capture.tokens[t].next,k++){
        CHECK(fread(pixels,1,frame_size,reference)==frame_size);char pixel_pin[65],ppm_pin[65];hash_bytes(pixels,frame_size,pixel_pin);
        char *ppm=NULL;size_t ppm_bytes=0;FILE *image=open_memstream(&ppm,&ppm_bytes);CHECK(image);fputs("P6\n512 512\n255\n",image);
        for(size_t p=0;p<512u*512u;p++)CHECK(fwrite(pixels+p*4,1,3,image)==3);
        CHECK(!fclose(image));hash_bytes(ppm,ppm_bytes,ppm_pin);free(ppm);
        size_t expected_time=full?(((k==15?239u:k*16u)*1000000u+15u)/30u):k*500000u;
        size_t timestamp=0,width=0,height=0;CHECK(json_uint(&capture,json_get(&capture,t,"timestamp_us"),&timestamp)&&timestamp==start_ms*1000+expected_time);
        CHECK(json_uint(&capture,json_get(&capture,t,"width"),&width)&&width==512&&json_uint(&capture,json_get(&capture,t,"height"),&height)&&height==512);
        char *actual_pixel=json_string(&capture,json_get(&capture,t,"rgba_sha256")),*actual_ppm=json_string(&capture,json_get(&capture,t,"prepared_sha256"));CHECK(actual_pixel&&actual_ppm&&!strcmp(actual_pixel,pixel_pin)&&!strcmp(actual_ppm,ppm_pin));free(actual_pixel);free(actual_ppm);
    }
    CHECK(fgetc(reference)==EOF&&!ferror(reference)&&!fclose(reference));free(pixels);
    CHECK(snprintf(response_path,sizeof(response_path),"%s/response.json",argv[7])<(int)sizeof(response_path));out=fopen(response_path,"w");CHECK(out&&fwrite(saved,1,strlen(saved),out)==strlen(saved)&&!fclose(out)&&!chmod(response_path,0600));
    CHECK(!unlink(video));args[8]="/missing/media-helper";args[10]="/missing/temporal-inference";
    body=invoke(args,request);CHECK(!strcmp(body,saved));free(body);
    snprintf(request,sizeof(request),"{\"contract_version\":1,\"op\":\"analyze_video\",\"source_id\":\"%s\",\"version\":\"%s\",\"start_ms\":%zu,\"end_ms\":%zu,\"prompt\":\"Describe the color change across these video frames in temporal order.\"}",source,source,start_ms,start_ms+duration_ms);
    body=invoke(args,request);char *cached_text=field(body,"text");CHECK(!strcmp(cached_text,text));free(cached_text);free(body);
    printf("PASS Sources actual temporal capture: %u checks; %zu native512px source frames, first/last timestamps and independent FFmpeg pixel/P6 bindings, actual ordered observation, package/runtime pins, origin/helper/model-free tracked replay and retained cache; text=%s; %s\n",checks,expected_frames,text,argv[7]);
    free(text);free(saved);free(source);return 0;
}
int main(int argc, char **argv) {
    if(getenv("SAMOSA_TEST_TEMPORAL"))return temporal_real(argc,argv);
    CHECK(argc == 7);
    alarm(240);
    char root[] = "/tmp/samosa-source-vision-real-XXXXXX";
    CHECK(mkdtemp(root));
    char state[512], admission[512], arguments[512], video[512], path[4096];
    snprintf(state, sizeof(state), "%s/state", root);
    snprintf(admission, sizeof(admission), "%s/admission", root);
    snprintf(arguments, sizeof(arguments), "%s/inference-args.json", root);
    snprintf(video, sizeof(video), "%s/heading.mp4", root);
    FILE *from = fopen(argv[6], "rb"), *to = fopen(video, "wb");
    CHECK(from && to);
    unsigned char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), from)))
        CHECK(fwrite(buffer, 1, n, to) == n);
    CHECK(!ferror(from) && !fclose(from) && !fclose(to) && !chmod(video, 0600));
    char engine[65], config[65], tokenizer[65], weights[65], model[65], material[256];
    file_sha(argv[4], engine);
    snprintf(path, sizeof(path), "%s/config.json", argv[5]);
    file_sha(path, config);
    snprintf(path, sizeof(path), "%s/tokenizer.json", argv[5]);
    file_sha(path, tokenizer);
    snprintf(path, sizeof(path), "%s/model.safetensors", argv[5]);
    file_sha(path, weights);
    int len = snprintf(material, sizeof(material), "visionpsy-model-v1:%s:%s:%s", config, tokenizer,
                       weights);
    CHECK(len > 0 && (size_t)len < sizeof(material));
    hash_bytes(material, (size_t)len, model);
    int fd = open(arguments, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    FILE *f = fdopen(fd, "w");
    CHECK(f);
    const char *engine_args[] = {"--vision-engine",
                                 argv[4],
                                 "--vision-model",
                                 argv[5],
                                 "--vision-engine-sha256",
                                 engine,
                                 "--vision-config-sha256",
                                 config,
                                 "--vision-tokenizer-sha256",
                                 tokenizer,
                                 "--vision-weights-sha256",
                                 weights,
                                 "--admission-state",
                                 admission,
                                 "--owner",
                                 "visual-real-owner"};
    fputc('[', f);
    for (size_t k = 0; k < sizeof(engine_args) / sizeof(*engine_args); k++) {
        if (k)
            fputc(',', f);
        json_quote(f, engine_args[k], strlen(engine_args[k]));
    }
    fputc(']', f);
    CHECK(!fclose(f));
    char *args[] = {argv[1],
                    "--root",
                    root,
                    "--state",
                    state,
                    "--owner",
                    "visual-real-owner",
                    "--media-helper",
                    argv[2],
                    "--visual-inference-executable",
                    argv[3],
                    "--visual-inference-arguments-file",
                    arguments,
                    "--visual-inference-provider-id",
                    "visionpsy-native-v1",
                    "--visual-inference-engine-sha256",
                    engine,
                    "--visual-inference-model-sha256",
                    model,
                    NULL};
    char *body = invoke(
             args, "{\"contract_version\":1,\"op\":\"register_media\",\"path\":\"heading.mp4\"}"),
         *source = field(body, "source_id");
    free(body);
    char request[1024];
    snprintf(request, sizeof(request),
             "{\"contract_version\":1,\"op\":\"analyze_visual\",\"source_id\":\"%s\",\"version\":"
             "\"%s\",\"start_ms\":1000,\"end_ms\":1500,\"prompt\":\"Read the large heading on this "
             "page. Answer "
             "briefly.\",\"operation_id\":\"real-visual-capture\",\"parent_operation_id\":\"real-"
             "visual-parent\",\"timeout_ms\":60000}",
             source, source);
    char *saved = invoke(args, request), *text = field(saved, "text");
    for (char *p = text; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    if (!strstr(text, "hello"))
        fprintf(stderr, "actual observation: %s\n", text);
    CHECK(strstr(text, "hello") && strstr(saved, engine) && strstr(saved, model) &&
          strstr(saved, "visual_analysis_uncertain") && strstr(saved, "source_time_us") &&
          strstr(saved, "native_decode_scale_rgba"));
    CHECK(!strstr(saved, "evidence_id") && !strstr(saved, "\"data\""));
    Json j;
    CHECK(json_parse(&j, saved, strlen(saved)));
    size_t timestamp = 0;
    CHECK(json_uint(&j, json_get(&j, json_get(&j, 0, "frame"), "timestamp_us"), &timestamp) &&
          timestamp >= 1000000 && timestamp < 1500000);
    CHECK(!unlink(video) && !unlink(arguments));
    char *offline[] = {argv[1], "--state", state, "--owner", "visual-real-owner", NULL};
    body = invoke(offline, request);
    CHECK(!strcmp(body, saved));
    free(body);
    printf(
        "PASS actual Sources/public Inference visual pipeline: %u checks; owned encoded frame "
        "timestamp=%zuus, native VisionPsy heading recognition, source/decoder/model provenance, "
        "persisted capture and exact offline durable replay; text=%s; bounded fixture only\n",
        checks, timestamp, text);
    free(text);
    free(saved);
    free(source);
    CHECK(!nftw(root, remove_file, 32, FTW_DEPTH | FTW_PHYS));
    return 0;
}
