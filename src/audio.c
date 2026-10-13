#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include "control.h"
#include "cache.h"
#include "native.h"
#include "sha256.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static unsigned le16(const unsigned char *p) {return p[0] | ((unsigned)p[1]<<8);}
static void put32(unsigned char *p, uint32_t v) {
    for (unsigned i=0;i<4;i++) p[i]=(unsigned char)(v>>(i*8));
}
const char *sources_wave_inspect(const SourcesConfig *cfg, const void *input,
                                 size_t length, SourcesWave *result) {
    if (!result) return "invalid_request";
    memset(result,0,sizeof(*result));
    if (!input || length<12 || length>SOURCES_INPUT_MAX) return "invalid_audio";
    const unsigned char *bytes=input;
    if (memcmp(bytes,"RIFF",4)||memcmp(bytes+8,"WAVE",4)||
        (uint64_t)le32(bytes+4)+8!=length) return "invalid_audio";
    size_t offset=12, chunks=0;
    int format=0, data=0;
    SourcesWave wave={0};
    while (offset<length) {
        const char *error=sources_interrupted(cfg);
        if (error) return error;
        if (++chunks>128) return "audio_chunk_limit";
        if (length-offset<8) return "invalid_audio";
        const unsigned char *chunk=bytes+offset;
        size_t n=le32(chunk+4);
        offset+=8;
        if (n>length-offset) return "invalid_audio";
        if (!memcmp(chunk,"fmt ",4)) {
            if (format++ || n<16) return "invalid_audio";
            const unsigned char *f=bytes+offset;
            if (le16(f)!=1 || le16(f+2)!=1 || le32(f+4)!=16000 ||
                le32(f+8)!=32000 || le16(f+12)!=2 || le16(f+14)!=16)
                return "unsupported_audio_format";
            /* PCM extension must be absent or explicitly empty. */
            if (n!=16 && (n!=18 || le16(f+16))) return "unsupported_audio_format";
        } else if (!memcmp(chunk,"data",4)) {
            if (data++ || !n || (n&1)) return "invalid_audio";
            wave.pcm=bytes+offset;
            wave.pcm_bytes=n;
            wave.frames=n/2;
        }
        offset+=n;
        if (n&1) {
            if (offset==length) return "invalid_audio";
            offset++;
        }
    }
    if (!format || !data) return "invalid_audio";
    *result=wave;
    return NULL;
}
const char *sources_wave_window(const SourcesConfig *cfg, const void *input,
                                size_t length, size_t start, size_t end,
                                unsigned char **output, size_t *output_bytes) {
    if (!output || !output_bytes) return "invalid_request";
    *output=NULL; *output_bytes=0;
    SourcesWave wave;
    const char *error=sources_wave_inspect(cfg,input,length,&wave);
    if (error) return error;
    if (end<=start || end-start>30000 || start>SIZE_MAX/16 ||
        end>SIZE_MAX/16 || end*16>wave.frames) return "invalid_range";
    size_t n=(end-start)*32;
    unsigned char *out=calloc(1,n+44);
    if (!out) return "resource_limit";
    memcpy(out,"RIFF",4); put32(out+4,(uint32_t)n+36);
    memcpy(out+8,"WAVEfmt ",8); put32(out+16,16);
    out[20]=1; out[22]=1; put32(out+24,16000); put32(out+28,32000);
    out[32]=2; out[34]=16; memcpy(out+36,"data",4); put32(out+40,(uint32_t)n);
    for (size_t copied=0;copied<n;) {
        error=sources_interrupted(cfg);
        if (error) {free(out); return error;}
        size_t take=n-copied; if (take>4096) take=4096;
        memcpy(out+44+copied,wave.pcm+start*32+copied,take);
        copied+=take;
    }
    error=sources_interrupted(cfg);
    if (error) {free(out); return error;}
    *output=out; *output_bytes=n+44;
    return NULL;
}
static int valid_id(const char *id) {
    if (!id || strlen(id)!=64) return 0;
    for (size_t i=0;i<64;i++)
        if (!strchr("0123456789abcdef",id[i])) return 0;
    return 1;
}
static int fail(FILE *out,const char *error) {
    fprintf(out,"{\"contract_version\":1,\"error\":{\"code\":\"%s\"}}\n",error);
    return 1;
}
int sources_audio_inspect(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out) {
    char *source=json_string(request,json_get(request,0,"source_id")),
         *version=json_string(request,json_get(request,0,"version"));
    const char *error=NULL;sqlite3_stmt *stmt=NULL;char *body=NULL;size_t body_bytes=0,frames=0;
    char fingerprint[65]={0};int encoded=0;
    if(!valid_id(source)||!valid_id(version))error="invalid_request";
    else if(strcmp(source,version))error="stale_source";
    if(!error&&sqlite3_prepare_v2(db,"SELECT b.bytes,c.media FROM snapshots b JOIN source_claims c ON c.id=b.id WHERE c.owner=? AND c.id=? AND c.media IN ('audio/wav','application/x-samosa-encoded-media') ORDER BY c.media='audio/wav' DESC LIMIT 1",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
    if(!error){
        sqlite3_bind_text(stmt,1,cfg->owner?cfg->owner:"local-owner",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,2,source,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(stmt);
        if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";
        else{
            const void *bytes=sqlite3_column_blob(stmt,0);int n=sqlite3_column_bytes(stmt,0);
            const char *media=(const char*)sqlite3_column_text(stmt,1);
            if(!bytes||n<=0||(size_t)n>SOURCES_INPUT_MAX||sqlite3_column_type(stmt,0)!=SQLITE_BLOB||
               !media||sqlite3_column_type(stmt,1)!=SQLITE_TEXT||strlen(media)!=(size_t)sqlite3_column_bytes(stmt,1))error="corrupt_snapshot";
            else{
                char actual[65];hash_bytes(bytes,(size_t)n,actual);
                if(strcmp(actual,source))error="corrupt_snapshot";
                else if(!strcmp(media,"audio/wav")){
                    SourcesWave wave;error=sources_wave_inspect(cfg,bytes,(size_t)n,&wave);
                    if(!error){frames=wave.frames;const char *recipe="audio-pcm-window-v1:RIFF:PCM16:mono:16000:exclusive-ms:lossless";hash_bytes(recipe,strlen(recipe),fingerprint);}
                }else{
                    encoded=1;error=sources_native_media_fingerprint(cfg,fingerprint);
                    if(!error)error=sources_native_media_probe(cfg,bytes,(size_t)n,fingerprint,&body,&body_bytes);
                    if(!error){Json j;json_parse(&j,body,body_bytes);if(!json_uint(&j,json_get(&j,0,"frames"),&frames))error="native_helper_protocol";}
                }
            }
        }
    }
    sqlite3_finalize(stmt);
    if(!error&&(frames<16||frames>230400000))error="invalid_range";
    if(!error)error=sources_interrupted(cfg);
    if(!error){
        fprintf(out,"{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\","
            "\"kind\":\"derived\",\"artifact_type\":\"audio_timeline\",\"frames\":%zu,"
            "\"sample_rate\":16000,\"duration_ms\":%zu,\"coordinate_space\":\"source_time_ms\","
            "\"duration_policy\":\"%s\",\"provenance\":{\"reader\":\"audio-timeline-v1\","
            "\"extractor_fingerprint\":\"%s\"}",source,version,frames,frames/16,
            encoded?"decoded_samples_floor_ms":"pcm_samples_floor_ms",fingerprint);
        if(body){fputs(",\"decoder\":",out);fwrite(body,1,body_bytes,out);}fputs("}\n",out);
    }
    free(source);free(version);free(body);return error?fail(out,error):0;
}
int sources_visual_inspect(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out) {
    char *source=json_string(request,json_get(request,0,"source_id")),
         *version=json_string(request,json_get(request,0,"version"));
    const char *error=NULL;sqlite3_stmt *stmt=NULL;char *body=NULL;size_t n=0;
    char fingerprint[65]={0};
    if(!valid_id(source)||!valid_id(version))error="invalid_request";
    else if(strcmp(source,version))error="stale_source";
    if(!error&&sqlite3_prepare_v2(db,"SELECT b.bytes FROM snapshots b JOIN source_claims c ON c.id=b.id WHERE c.owner=? AND c.id=? AND c.media='application/x-samosa-encoded-media' LIMIT 1",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
    if(!error) {
        sqlite3_bind_text(stmt,1,cfg->owner?cfg->owner:"local-owner",-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,2,source,-1,SQLITE_TRANSIENT);
        int rc=sqlite3_step(stmt);
        if(rc!=SQLITE_ROW)error=rc==SQLITE_DONE?"unknown_source":"store_failed";
        else {
            const void *bytes=sqlite3_column_blob(stmt,0);int length=sqlite3_column_bytes(stmt,0);
            if(!bytes||length<=0||(size_t)length>SOURCES_INPUT_MAX||sqlite3_column_type(stmt,0)!=SQLITE_BLOB)error="corrupt_snapshot";
            else {
                char actual[65];hash_bytes(bytes,(size_t)length,actual);
                if(strcmp(actual,source))error="corrupt_snapshot";
                if(!error)error=sources_native_media_fingerprint(cfg,fingerprint);
                if(!error)error=sources_native_video_probe(cfg,bytes,(size_t)length,fingerprint,&body,&n);
            }
        }
    }
    sqlite3_finalize(stmt);
    if(!error)error=sources_interrupted(cfg);
    size_t duration=0;
    if(!error) {
        Json j;if(!json_parse(&j,body,n)||!json_uint(&j,json_get(&j,0,"duration_ms"),&duration))error="native_helper_protocol";
    }
    if(!error) {
        fprintf(out,"{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\","
            "\"kind\":\"derived\",\"artifact_type\":\"visual_timeline\",\"duration_ms\":%zu,"
            "\"coordinate_space\":\"source_time_ms\",\"duration_policy\":\"container_floor_ms\","
            "\"coverage\":{\"status\":\"partial\",\"reason\":\"media_inspection_only\"},"
            "\"provenance\":{\"reader\":\"visual-timeline-v1\",\"extractor_fingerprint\":\"%s\"},"
            "\"decoder\":",source,version,duration,fingerprint);
        fwrite(body,1,n,out);fputs("}\n",out);
    }
    free(source);free(version);free(body);return error?fail(out,error):0;
}
static const char *base64(const SourcesConfig *cfg, FILE *out,
                          const unsigned char *bytes,size_t n) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    fputc('"',out);
    for (size_t i=0;i<n;i+=3) {
        if (i%12288==0) {
            const char *error=sources_interrupted(cfg);
            if (error) return error;
        }
        uint32_t value=(uint32_t)bytes[i]<<16;
        if (i+1<n) value|=(uint32_t)bytes[i+1]<<8;
        if (i+2<n) value|=bytes[i+2];
        fputc(alphabet[(value>>18)&63],out); fputc(alphabet[(value>>12)&63],out);
        fputc(i+1<n?alphabet[(value>>6)&63]:'=',out);
        fputc(i+2<n?alphabet[value&63]:'=',out);
    }
    fputc('"',out);
    return sources_interrupted(cfg);
}
typedef struct {
    const char *source,*version,*artifact,*fingerprint,*locator;
    size_t start,end;
    int encoded,visual;
} ExpectedPrepared;
static int same_string(const Json *j,int parent,const char *name,const char *expected) {
    char *value=json_string(j,json_get(j,parent,name));
    int same=value&&!strcmp(value,expected);free(value);return same;
}
static int same_uint(const Json *j,int parent,const char *name,size_t expected) {
    size_t value;return json_uint(j,json_get(j,parent,name),&value)&&value==expected;
}
static const char *cached_prepared_valid(const SourcesConfig *cfg,const char *body,size_t length,const ExpectedPrepared *e) {
    Json j;
    if (!json_parse(&j,body,length)||j.tokens[0].type!=JT_OBJECT||
        json_get(&j,0,"error")>=0||json_get(&j,0,"evidence_id")>=0||
        json_get(&j,0,"operation_id")>=0||json_get(&j,0,"text")>=0||
        json_get(&j,0,"parent_operation_id")>=0||
        !same_uint(&j,0,"contract_version",1)||
        !same_string(&j,0,"source_id",e->source)||
        !same_string(&j,0,"version",e->version)||
        !same_string(&j,0,"artifact_id",e->artifact)||
        !same_string(&j,0,"kind","derived")||
        !same_string(&j,0,"artifact_type",e->visual?"prepared_visual":"prepared_audio")||
        !same_string(&j,0,"media_type",e->visual?"application/x-samosa-rgba":"audio/wav")||
        !same_string(&j,0,"encoding","base64")) return "corrupt_cache";
    int range=json_get(&j,0,"range"),timeline=json_get(&j,0,"prepared_timeline"),
        coverage=json_get(&j,0,"coverage"),provenance=json_get(&j,0,"provenance");
    if (range<0||(!e->visual&&timeline<0)||coverage<0||provenance<0||
        j.tokens[range].type!=JT_OBJECT||(!e->visual&&j.tokens[timeline].type!=JT_OBJECT)||
        j.tokens[coverage].type!=JT_OBJECT||j.tokens[provenance].type!=JT_OBJECT||
        !same_uint(&j,range,"start_ms",e->start)||!same_uint(&j,range,"end_ms",e->end)||
        !same_string(&j,range,"coordinate_space","source_time_ms")||
        (!e->visual&&(!same_uint(&j,timeline,"start_ms",0)||!same_uint(&j,timeline,"end_ms",e->end-e->start)||
        !same_uint(&j,timeline,"sample_rate",16000)||!same_uint(&j,timeline,"channels",1)||
        !same_uint(&j,timeline,"sample_bits",16)))||
        !same_string(&j,coverage,"status","partial")||
        !same_string(&j,coverage,"reason","media_preparation_only")||
        !same_string(&j,provenance,"reader",e->visual?"visual-media-frame-v1":e->encoded?"audio-media-window-v1":"audio-pcm-window-v1")||
        !same_string(&j,provenance,"extractor_fingerprint",e->fingerprint)||
        !same_string(&j,provenance,"derivation",e->visual?"native_decode_scale_rgba":e->encoded?"native_decode_resample":"lossless_pcm_window")||
        !same_string(&j,provenance,"locator",e->locator)) return "corrupt_cache";
    char *data=json_string(&j,json_get(&j,0,"data"));
    if(e->encoded) {
        int decoder=json_get(&j,0,"decoder");
        if(decoder<0 || j.tokens[decoder].type!=JT_OBJECT ||
           !same_string(&j,decoder,"fingerprint",e->fingerprint)) {
            free(data); return "corrupt_cache";
        }
        char *dto=NULL; size_t dto_bytes=0;
        FILE *stream=open_memstream(&dto,&dto_bytes);
        if(!stream) {free(data); return "resource_limit";}
        fwrite(body+j.tokens[decoder].begin,1,
               j.tokens[decoder].end-j.tokens[decoder].begin-1,stream);
        fputs(",\"encoding\":\"base64\",\"data\":",stream);
        if(data) json_quote(stream,data,strlen(data)); else fputs("null",stream);
        fputc('}',stream);
        int failed=fclose(stream);
        const char *error=failed?"resource_limit":
            (e->visual?sources_media_frame_validate(cfg,dto,dto_bytes,e->start,e->end):
            sources_media_response_validate(cfg,dto,dto_bytes,e->start,e->end));
        free(dto);
        if(error) {free(data);return !strcmp(error,"cancelled")||!strcmp(error,"deadline_exceeded")?error:"corrupt_cache";}
    }
    if(e->visual){free(data);return NULL;}
    size_t bytes=44+(e->end-e->start)*32,encoded=(bytes+2)/3*4;
    int valid=data&&strlen(data)==encoded;
    if (valid) {
        size_t padding=(3-bytes%3)%3;
        for (size_t i=0;i<encoded;i++) {
            if (i%12288==0) {const char *error=sources_interrupted(cfg);if(error){free(data);return error;}}
            if (i>=encoded-padding) {if(data[i]!='='){valid=0;break;}}
            else if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/",data[i])) {valid=0;break;}
        }
    }
    free(data);return valid?NULL:"corrupt_cache";
}
static const char *prepared_cache_get(sqlite3 *db,const SourcesConfig *cfg,const char *key,const ExpectedPrepared *e,
                                  char **body,size_t *length) {
    *body=NULL;*length=0;sqlite3_stmt *stmt=NULL;
    const char *error=NULL;
    if(sqlite3_prepare_v2(db,"SELECT response,digest FROM prepared_artifacts WHERE key=?",-1,&stmt,NULL)!=SQLITE_OK)return "store_failed";
    sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(stmt);
    if(rc==SQLITE_ROW) {
        int n=sqlite3_column_bytes(stmt,0);
        const char *value=(const char*)sqlite3_column_text(stmt,0),*expected=(const char*)sqlite3_column_text(stmt,1);
        if(n<=0||n>2097152||!value||!expected||strlen(value)!=(size_t)n)error="corrupt_cache";
        else {char digest[65];hash_bytes(value,(size_t)n,digest);
            if(strcmp(digest,expected))error="corrupt_cache";
            else {*body=strdup(value);if(!*body)error="resource_limit";else *length=(size_t)n;}}
    } else if(rc!=SQLITE_DONE)error="store_failed";
    sqlite3_finalize(stmt);
    if(!error&&*body)error=cached_prepared_valid(cfg,*body,*length,e);
    if(error){free(*body);*body=NULL;*length=0;}return error;
}
static const char *prepared_cache_put(sqlite3 *db,const SourcesConfig *cfg,const char *key,
                                  const ExpectedPrepared *e,const char *body,size_t length) {
    if(length>2097152)return "cache_limit";
    if(sqlite3_exec(db,"BEGIN IMMEDIATE",NULL,NULL,NULL)!=SQLITE_OK)return "store_busy";
    char *old=NULL;size_t old_length=0;
    const char *error=prepared_cache_get(db,cfg,key,e,&old,&old_length);
    if(!error&&old) {if(old_length!=length||memcmp(old,body,length))error="cache_conflict";}
    else if(!error) {
        error=sources_cache_capacity(db,cfg,length);
        sqlite3_stmt *stmt=NULL;
        if(!error&&sqlite3_prepare_v2(db,"INSERT INTO prepared_artifacts VALUES(?,?,?)",-1,&stmt,NULL)!=SQLITE_OK)error="store_failed";
        if(!error) {
            char digest[65];hash_bytes(body,length,digest);
            sqlite3_bind_text(stmt,1,key,-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,2,body,(int)length,SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt,3,digest,-1,SQLITE_TRANSIENT);
            if(sqlite3_step(stmt)!=SQLITE_DONE)error="store_failed";
        }
        sqlite3_finalize(stmt);
    }
    free(old);if(!error)error=sources_interrupted(cfg);
    if(error)sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);
    else if(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)!=SQLITE_OK){sqlite3_exec(db,"ROLLBACK",NULL,NULL,NULL);error="store_failed";}
    return error;
}
static int media_prepare(const SourcesConfig *cfg,const Json *request,
                           sqlite3 *db,FILE *out,int persistent) {
    int visual=same_string(request,0,"op","prepare_visual");
    char *source=json_string(request,json_get(request,0,"source_id"));
    char *version=json_string(request,json_get(request,0,"version"));
    size_t start=0,end=0;
    const char *error=NULL;
    if(cfg->cache_limit>128||cfg->cache_byte_limit>8u*1048576u)error="invalid_configuration";
    if (!valid_id(source)||!valid_id(version)||
        !json_uint(request,json_get(request,0,"start_ms"),&start)||
        !json_uint(request,json_get(request,0,"end_ms"),&end)) error="invalid_request";
    else if (strcmp(source,version)) error="stale_source";
    else if (end<=start||end-start>30000||end>14400000) error="invalid_range";
    sqlite3_stmt *stmt=NULL;
    unsigned char *snapshot=NULL,*wave=NULL;
    char *locator=NULL,*body=NULL;
    size_t length=0,wave_bytes=0,body_bytes=0;
    int encoded=0; char *decoded=NULL;
    if (!error && sqlite3_prepare_v2(db,
        "SELECT snapshots.bytes,source_claims.locator,source_claims.media FROM snapshots JOIN source_claims ON source_claims.id=snapshots.id WHERE snapshots.id=? AND source_claims.owner=? AND (source_claims.media='application/x-samosa-encoded-media' OR (?=0 AND source_claims.media='audio/wav')) ORDER BY source_claims.media='audio/wav' DESC LIMIT 1",
        -1,&stmt,NULL)!=SQLITE_OK) error="store_failed";
    if (!error) {
        sqlite3_bind_text(stmt,1,source,-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt,2,cfg->owner?cfg->owner:"local-owner",-1,SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt,3,visual);
        int rc=sqlite3_step(stmt);
        if (rc==SQLITE_DONE) error="unknown_source";
        else if (rc!=SQLITE_ROW) error="store_failed";
        else {
            encoded=strcmp((const char*)sqlite3_column_text(stmt,2),"audio/wav")!=0;
            int n=sqlite3_column_bytes(stmt,0);
            const void *bytes=sqlite3_column_blob(stmt,0);
            const char *path=(const char*)sqlite3_column_text(stmt,1);
            if (n<=0||(size_t)n>SOURCES_INPUT_MAX||!bytes||!path) error="corrupt_snapshot";
            else {
                length=(size_t)n; snapshot=malloc(length); locator=strdup(path);
                if (!snapshot||!locator) error="resource_limit";
                else {memcpy(snapshot,bytes,length); char actual[65];
                    hash_bytes(snapshot,length,actual);
                    if (strcmp(actual,version)) error="corrupt_snapshot";}
            }
        }
    }
    sqlite3_finalize(stmt);
    char fingerprint[65]={0},artifact[65],selector[256],key[65];
    ExpectedPrepared expected={.source=source,.version=version,.artifact=artifact,
                            .fingerprint=fingerprint,.locator=locator,.start=start,.end=end,.encoded=encoded,.visual=visual};
    if (!error) {
        const char *contract="audio-pcm-window-v1:RIFF:PCM16:mono:16000:exclusive-ms:lossless";
        if(encoded) error=sources_native_media_fingerprint(cfg,fingerprint);
        else hash_bytes(contract,strlen(contract),fingerprint);
        int n=visual?snprintf(selector,sizeof(selector),"visual-media-frame-v1:%s:%s:%zu:%zu",fingerprint,version,start,end):
            snprintf(selector,sizeof(selector),"%s:%s:%zu:%zu",fingerprint,version,start,end);
        hash_bytes(selector,(size_t)n,artifact);
        Sha256 identity;sha256_init(&identity);
        const char *owner=cfg->owner?cfg->owner:"local-owner";
        sha256_update(&identity,"prepared-owner-cache-v1:",sizeof("prepared-owner-cache-v1:")-1);
        sha256_update(&identity,owner,strlen(owner));sha256_update(&identity,artifact,64);
        unsigned char digest[32];sha256_final(&identity,digest);sha256_hex(digest,key);
        if(!error) error=prepared_cache_get(db,cfg,key,&expected,&body,&body_bytes);
    }
    int cached=body!=NULL;
    if (!error&&!cached&&persistent) error=sources_cache_capacity(db,cfg,0);
    if (!error&&!cached) {
        if(encoded) {size_t decoded_bytes=0; error=visual?sources_native_media_frame(cfg,snapshot,length,start,end,fingerprint,&decoded,&decoded_bytes):sources_native_media_audio(cfg,snapshot,length,start,end,fingerprint,&decoded,&decoded_bytes);}
        else error=sources_wave_window(cfg,snapshot,length,start,end,&wave,&wave_bytes);
    }
    if (!error&&!cached) {
        FILE *stream=open_memstream(&body,&body_bytes);
        if (!stream) error="resource_limit";
        else {
            fprintf(stream,"{\"contract_version\":1,\"source_id\":\"%s\",\"version\":\"%s\",\"artifact_id\":\"%s\",\"kind\":\"derived\",\"artifact_type\":\"%s\",\"media_type\":\"%s\",\"encoding\":\"base64\",\"data\":",source,version,artifact,visual?"prepared_visual":"prepared_audio",visual?"application/x-samosa-rgba":"audio/wav");
            if(encoded) {
                Json result; json_parse(&result,decoded,strlen(decoded));
                char *data=json_string(&result,json_get(&result,0,"data"));
                if(!data) error="native_helper_protocol";
                else json_quote(stream,data,strlen(data));
                free(data);
                const char *cut=strstr(decoded,",\"encoding\":");
                if(!cut) error="native_helper_protocol";
                else {fputs(",\"decoder\":",stream); fwrite(decoded,1,(size_t)(cut-decoded),stream); fputc('}',stream);}
            } else error=base64(cfg,stream,wave,wave_bytes);
            fprintf(stream,",\"range\":{\"start_ms\":%zu,\"end_ms\":%zu,\"coordinate_space\":\"source_time_ms\"}",start,end);
            if(!visual)fprintf(stream,",\"prepared_timeline\":{\"start_ms\":0,\"end_ms\":%zu,\"sample_rate\":16000,\"channels\":1,\"sample_bits\":16}",end-start);
            fprintf(stream,",\"coverage\":{\"status\":\"partial\",\"reason\":\"media_preparation_only\"},\"provenance\":{\"reader\":\"%s\",\"extractor_fingerprint\":\"%s\",\"derivation\":\"%s\",\"locator\":",visual?"visual-media-frame-v1":encoded?"audio-media-window-v1":"audio-pcm-window-v1",fingerprint,visual?"native_decode_scale_rgba":encoded?"native_decode_resample":"lossless_pcm_window");
            json_quote(stream,locator,strlen(locator)); fputs("}}\n",stream);
            if (fclose(stream)) error="resource_limit";
        }
    }
    if (!error&&!cached) error=cached_prepared_valid(cfg,body,body_bytes,&expected);
    if (!error) error=sources_interrupted(cfg);
    if (!error&&!cached&&persistent) error=prepared_cache_put(db,cfg,key,&expected,body,body_bytes);
    if (!error) fwrite(body,1,body_bytes,out);
    free(decoded);free(body);free(wave);free(snapshot);free(locator);free(source);free(version);
    return error?fail(out,error):0;
}

int sources_media_prepare(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out){return media_prepare(cfg,request,db,out,1);}
int sources_media_prepare_transient(const SourcesConfig *cfg,const Json *request,sqlite3 *db,FILE *out){return media_prepare(cfg,request,db,out,0);}
const char *sources_audio_window_plan(const SourcesConfig *cfg,size_t duration,size_t ordinal,size_t *count,size_t *start,size_t *end){
    *count=*start=*end=0;const char *error=sources_interrupted(cfg);if(error)return error;
    if(!duration||duration>14400000)return "invalid_range";
    const size_t window=30000,step=29000;
    size_t total=1;if(duration>window){size_t remaining=duration-window;total+=remaining/step+(remaining%step!=0);}
    if(ordinal>=total)return "invalid_request";
    *count=total;*start=ordinal*step;*end=duration-*start<window?duration:*start+window;return NULL;
}
