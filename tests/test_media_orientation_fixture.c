#include "sources.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static void big32(unsigned char *out,int32_t value) {
    uint32_t bits=(uint32_t)value;
    for(unsigned i=0;i<4;i++)out[i]=(unsigned char)(bits>>(24-i*8));
}
int main(int argc,char **argv) {
    assert(argc==4||argc==5);char *end;int padding=argc==5;
    if(padding)assert(!strcmp(argv[3],"--pad"));
    long code=strtol(argv[padding?4:3],&end,10);
    assert(*argv[padding?4:3]&&!*end&&code>=0&&
        (padding?(unsigned long)code<=SOURCES_INPUT_MAX:code<=8));
    FILE *input=fopen(argv[1],"rb");assert(input);
    unsigned char *bytes=malloc(SOURCES_INPUT_MAX+1);assert(bytes);
    size_t length=fread(bytes,1,SOURCES_INPUT_MAX+1,input);
    assert(length&&length<=SOURCES_INPUT_MAX&&!ferror(input));fclose(input);
    if(padding) {
        assert((size_t)code>=length+8);
        big32(bytes+length,(int32_t)((size_t)code-length));
        memcpy(bytes+length+4,"free",4);memset(bytes+length+8,0,(size_t)code-length-8);
        FILE *output=fopen(argv[2],"wb");assert(output);
        assert(fwrite(bytes,1,(size_t)code,output)==(size_t)code&&!fclose(output));free(bytes);
        return 0;
    }
    const int transforms[9][4]={
        {1,0,0,1},{0,1,-1,0},{-1,0,0,-1},{0,-1,1,0},
        {-1,0,0,1},{1,0,0,-1},{0,1,1,0},{0,-1,-1,0},{1,1,0,1}
    };
    size_t at=0,count=0;
    for(size_t i=4;i+60<=length;i++)if(!memcmp(bytes+i,"tkhd",4)) {
        unsigned version=bytes[i+4];assert(version<=1);
        at=i+4+(version?52:40);assert(at+36<=length);count++;
    }
    assert(count==1);
    int32_t matrix[9]={0};matrix[8]=1<<30;
    matrix[0]=transforms[code][0]*65536;matrix[1]=transforms[code][1]*65536;
    matrix[3]=transforms[code][2]*65536;matrix[4]=transforms[code][3]*65536;
    for(unsigned i=0;i<9;i++)big32(bytes+at+i*4,matrix[i]);
    FILE *output=fopen(argv[2],"wb");assert(output);
    assert(fwrite(bytes,1,length,output)==length&&!fclose(output));free(bytes);
    return 0;
}
