#include "chutni.h"
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);abort();}}while(0)
static void test(const char *field,const char *value,int accept){
    char *body=NULL;size_t n=0;FILE *out=open_memstream(&body,&n);CHECK(out);
    fprintf(out,"{\"store_id\":\"01234567-89ab-cdef-0123-456789abcdef\",\"query\":\"query\",\"items\":[{");
    const char *fields[]={"external_source_id","external_artifact_id","artifact_kind","snippet","freshness","score","score_type"};
    const char *values[]={"\"01234567-89ab-cdef-0123-456789abcdef\"","\"01234567-89ab-cdef-0123-456789abcdef\"","\"extracted_text\"","\"A snippet\"","\"current\"","0.125","\"bm25_fts5_negated\""};
    for(size_t i=0;i<7;i++){if(i)fputc(',',out);fprintf(out,"\"%s\":%s",fields[i],field&&!strcmp(field,fields[i])?value:values[i]);}fputs("}]}",out);CHECK(!fclose(out));
    CHECK((sources_chutni_validate(body,n,"query")==NULL)==accept);CHECK(sources_chutni_validate(body,n,"other"));free(body);
}
static void publication(const char *field,const char *value,int accept){
    const char *hash="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    char metadata[512];snprintf(metadata,sizeof(metadata),"{\"store_id\":\"01234567-89ab-cdef-0123-456789abcdef\",\"publication_id\":\"%s\",\"version\":\"%s\",\"evidence_id\":\"%s\"}",hash,hash,hash);
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);
    const char *fields[]={"store_id","external_source_id","external_artifact_id","publication_id","version","evidence_id","source_content_hash","artifact_status","recovered"};char quoted[67],blake[74];snprintf(quoted,sizeof(quoted),"\"%s\"",hash);snprintf(blake,sizeof(blake),"\"blake3:%s\"",hash);
    const char *values[]={"\"01234567-89ab-cdef-0123-456789abcdef\"","\"01234567-89ab-cdef-0123-456789abcdef\"","\"01234567-89ab-cdef-0123-456789abcdef\"",quoted,quoted,quoted,blake,"\"active\"","false"};
    fputc('{',out);for(size_t i=0;i<9;i++){if(i)fputc(',',out);fprintf(out,"\"%s\":%s",fields[i],field&&!strcmp(field,fields[i])?value:values[i]);}fputc('}',out);CHECK(!fclose(out));CHECK((sources_chutni_publish_validate(metadata,body,n)==NULL)==accept);free(body);
}
static void artifact_read(const char *field,const char *value,int accept){
    const char *id="\"01234567-89ab-cdef-0123-456789abcdef\"";
    const char *request="{\"store_id\":\"01234567-89ab-cdef-0123-456789abcdef\",\"external_source_id\":\"01234567-89ab-cdef-0123-456789abcdef\",\"external_artifact_id\":\"01234567-89ab-cdef-0123-456789abcdef\"}";
    const char *fields[]={"store_id","external_source_id","external_artifact_id","artifact_kind","artifact_origin","status","freshness","source_content_hash","selector","provenance","semantic_validation","text","range","coverage"};
    const char *values[]={id,id,id,"\"memory\"","\"human\"","\"active\"","\"current\"","\"blake3:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"","null","{\"producer\":{},\"derivation\":{}}","\"not_performed\"","\"ABC\"","{\"start_byte\":0,\"end_byte\":3,\"total_bytes\":3}","{\"status\":\"complete\",\"reason\":\"artifact_content\"}"};
    char *body=NULL;size_t n;FILE *out=open_memstream(&body,&n);CHECK(out);fputc('{',out);
    for(size_t i=0;i<14;i++){if(i)fputc(',',out);fprintf(out,"\"%s\":%s",fields[i],field&&!strcmp(field,fields[i])?value:values[i]);}
    fputc('}',out);CHECK(!fclose(out));CHECK((sources_chutni_read_validate(request,body,n)==NULL)==accept);free(body);
}
int main(void){
    test(NULL,NULL,1);test("external_source_id","\"invalid\"",0);test("external_artifact_id","null",0);test("artifact_kind","\"\"",0);test("snippet","123",0);test("freshness","\"stale\"",0);test("score","1e999",0);test("score","\"0.1\"",0);test("score_type","\"cosine_bruteforce\"",0);
    CHECK(sources_chutni_validate("{}",2,"query"));CHECK(sources_chutni_validate("null",4,"query"));CHECK(sources_chutni_validate(NULL,0,"query"));
    const char *empty="{\"store_id\":\"01234567-89ab-cdef-0123-456789abcdef\",\"query\":\"query\",\"items\":[]}";CHECK(!sources_chutni_validate(empty,strlen(empty),"query"));
    publication(NULL,NULL,1);publication("store_id","\"11234567-89ab-cdef-0123-456789abcdef\"",0);publication("version","\"mismatch\"",0);publication("publication_id","\"bad\"",0);publication("evidence_id","null",0);publication("external_artifact_id","\"bad\"",0);publication("source_content_hash","\"sha256:bad\"",0);publication("artifact_status","\"invalid\"",0);publication("artifact_status","\"stale\"",1);publication("recovered","\"true\"",0);
    artifact_read(NULL,NULL,1);artifact_read("store_id","\"11234567-89ab-cdef-0123-456789abcdef\"",0);artifact_read("external_source_id","null",0);artifact_read("external_artifact_id","\"invalid\"",0);artifact_read("text","\"ABCD\"",0);artifact_read("text","null",0);artifact_read("provenance","{\"producer\":null,\"derivation\":{}}",0);artifact_read("provenance","{}",0);artifact_read("selector","123",0);artifact_read("source_content_hash","\"sha256:bad\"",0);artifact_read("status","\"invalid\"",0);artifact_read("range","{\"start_byte\":1,\"end_byte\":4,\"total_bytes\":4}",0);artifact_read("coverage","{\"status\":\"partial\",\"reason\":\"artifact_content\"}",0);artifact_read("semantic_validation","\"verified\"",0);artifact_read("status","\"stale\"",1);
    printf("PASS Chutni parent shape, references, finite score, freshness and query validation: %u checks\n",checks);return 0;
}
