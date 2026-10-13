#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define JSON_TOKENS_MAX 8192
#include "semantic.h"
#include <locale.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void check(size_t dimensions,const char *dtype,const char *normalization,const char *vector,const char *expected){
    char profile[512];snprintf(profile,sizeof(profile),"{\"representation_kind\":\"text_embedding\",\"model_id\":\"fixture/embed\",\"model_revision\":\"1\",\"dimensions\":%zu,\"dtype\":\"%s\",\"normalization\":\"%s\"}",dimensions,dtype,normalization);
    Json j;CHECK(json_parse(&j,vector,strlen(vector)));char *arguments=NULL;const char *error=sources_semantic_arguments(profile,&j,0,&arguments);
    CHECK(expected?(error&&!strcmp(error,expected)):!error);if(error)CHECK(!arguments);else {CHECK(arguments&&!sources_semantic_arguments_validate(arguments));free(arguments);}
}
int main(void){
    check(2,"f32","none","[1.5,-0.25]",NULL);check(2,"f32","none","[0,0]","invalid_semantic_vector");check(2,"f32","none","[1,0,0]","invalid_semantic_vector");check(2,"f32","none","[1e400,0]","invalid_semantic_vector");check(2,"f32","none","[1e100,0]","invalid_semantic_vector");check(2,"f32","none","[1e-100,0]","invalid_semantic_vector");check(2,"f32","none","[1e-45,0]",NULL);check(2,"f32","none","[\"1\",0]","invalid_semantic_vector");
    check(0,"f32","none","[1]","invalid_semantic_configuration");check(1025,"f32","none","[1]","invalid_semantic_configuration");check(2,"f64","none","[1,0]","invalid_semantic_configuration");check(2,"f32","wrong","[1,0]","invalid_semantic_configuration");check(2,"f32","l2","[2,0]","invalid_semantic_vector");check(2,"f32","l2","[0.6,0.8]",NULL);
    char vector[4096];size_t used=0;vector[used++]='[';for(size_t i=0;i<1024;i++){if(i)vector[used++]=',';vector[used++]=i?'0':'1';}vector[used++]=']';vector[used]=0;check(1024,"f32","none",vector,NULL);
    Json j;CHECK(json_parse(&j,"[1.500,-0.2500]",15));char *canonical=NULL;CHECK(!sources_semantic_vector_canonical(&j,0,&canonical)&&!strcmp(canonical,"[1.5,-0.25]"));free(canonical);
    const char *bad[]={"{}","{\"profile\":{},\"vector\":[1],\"limit\":8,\"include_stale\":false}","{\"profile\":{},\"vector\":[1],\"limit\":8,\"include_stale\":true}"};for(size_t i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(sources_semantic_arguments_validate(bad[i]));
    locale_t alternate=newlocale(LC_NUMERIC_MASK,"de_DE.UTF-8",(locale_t)0);if(alternate){locale_t previous=uselocale(alternate);check(2,"f32","none","[1.5,-0.25]",NULL);CHECK(uselocale((locale_t)0)==alternate);check(2,"f32","l2","[2.5,0]","invalid_semantic_vector");CHECK(uselocale((locale_t)0)==alternate);uselocale(previous);freelocale(alternate);}
    printf("PASS semantic input boundaries: %u checks; dimension/dtype/normalization limits, finite f32/nonzero vectors, maximum vector, numeric canonicalization and available-locale restoration\n",checks);return 0;
}
