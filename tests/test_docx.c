#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "samosa_docx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL DOCX parser %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#include "docx_fixture.h"
static void refusal(const char *xml,const char *extra,const char *expected){size_t n;unsigned char *zip=docx_package(xml,extra,0,&n);SamosaDocxResult out={0};const char *error=NULL;CHECK(!samosa_docx_extract(zip,n,65536,&out,&error));if(!error||strcmp(error,expected))fprintf(stderr,"error: %s (expected %s)\n",error?error:"none",expected);CHECK(error&&!strcmp(error,expected));CHECK(!out.text);free(zip);}
static void zip_refusal(unsigned char *zip,size_t n,const char *expected){SamosaDocxResult out={0};const char *error=NULL;CHECK(!samosa_docx_extract(zip,n,65536,&out,&error));if(!error||strcmp(error,expected))fprintf(stderr,"error: %s (expected %s)\n",error?error:"none",expected);CHECK(error&&!strcmp(error,expected));CHECK(!out.text);free(zip);}
static uint32_t get32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void set32(unsigned char *p,uint32_t value){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(value>>(8*i));}
int main(void){const char *xml="<w:document xmlns:w='x'><w:body><w:p><w:r><w:t>Code &amp; café 7319</w:t></w:r><w:del><w:r><w:t>POISON</w:t></w:r></w:del></w:p></w:body></w:document>";
    for(unsigned mode=0;mode<2;mode++){size_t n;unsigned char *zip=docx_package(xml,NULL,mode?8:0,&n);SamosaDocxResult out={0};const char *error=NULL;CHECK(samosa_docx_extract(zip,n,65536,&out,&error));CHECK(out.text&&strstr(out.text,"Code & café 7319")&&!strstr(out.text,"POISON"));CHECK(out.entry_count==3);samosa_docx_result_free(&out);CHECK(!samosa_docx_extract(zip,n,4,&out,&error));CHECK(error&&!strcmp(error,"docx_text_output_limit"));free(zip);}
    refusal(xml,"../escape.xml","docx_unsafe_path");refusal(xml,"word/document.xml","docx_duplicate_required_entry");refusal("<!DOCTYPE w:document [<!ENTITY evil SYSTEM 'file:///etc/passwd'>]><w:document/>",NULL,"docx_xml_doctype_unsupported");refusal("<w:document><w:t>\xff</w:t></w:document>",NULL,"docx_xml_invalid_utf8");
    refusal("<w:document><w:p></w:document>",NULL,"docx_xml_malformed");refusal("<wrong><w:t>spoof</w:t></wrong>",NULL,"docx_xml_malformed");refusal("<w:document a='1' a='2'/>",NULL,"docx_xml_malformed");refusal("<w:document a='&unknown;'/>",NULL,"docx_xml_malformed");refusal("<w:document><1bad/></w:document>",NULL,"docx_xml_malformed");
    size_t n;unsigned char *zip=docx_package(xml,NULL,0,&n);zip[30+strlen("[Content_Types].xml")]^=1;zip_refusal(zip,n,"docx_decompression_failed");
    zip=docx_package(xml,NULL,8,&n);uint32_t central=get32(zip+n-6);CHECK(central+28<n);set32(zip+central+24,1048577);zip_refusal(zip,n,"docx_expansion_limit");
    zip=docx_package(xml,NULL,8,&n);central=get32(zip+n-6);set32(zip+central+24,100000);zip_refusal(zip,n,"docx_compression_ratio_limit");
    char types[]="<Types>application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml</Types>";const char *rels="<Relationships><Relationship Type='officeDocument' Target='word/document.xml'/></Relationships>";
    ZipPart parts[5]={{"[Content_Types].xml",types,0,0,0,0},{"_rels/.rels",rels,0,0,0,0},{"word/document.xml",xml,0,0,0,0},{"word/header1.xml","<w:hdr><w:t>HEADER</w:t></w:hdr>",0,0,0,0},{"word/header1.xml","<w:hdr/>",0,0,0,0}};
    zip=zip_package(parts,5,0,&n);zip_refusal(zip,n,"docx_duplicate_entry");
    SamosaDocxResult out={0};const char *error=NULL;zip=zip_package(parts,4,0,&n);CHECK(samosa_docx_extract(zip,n,65536,&out,&error));CHECK(strstr(out.text,"HEADER")&&!strstr(out.text,"word/header1.xml"));samosa_docx_result_free(&out);free(zip);
    parts[0].text="<!DOCTYPE Types><Types>application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml</Types>";zip=zip_package(parts,3,0,&n);zip_refusal(zip,n,"docx_xml_doctype_unsupported");
    parts[0].text="<Wrong>application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml</Wrong>";zip=zip_package(parts,3,0,&n);zip_refusal(zip,n,"docx_xml_malformed");
    parts[0].text=types;
    ZipPart *many=calloc(2049,sizeof(*many));char (*names)[48]=calloc(2049,sizeof(*names));CHECK(many&&names);
    for(unsigned i=0;i<2049;i++){snprintf(names[i],48,"custom/part%u.xml",i);many[i]=(ZipPart){names[i],"<x/>",0,0,0,0};}zip=zip_package(many,2049,0,&n);zip_refusal(zip,n,"docx_entry_count_limit");
    for(unsigned i=0;i<3;i++)many[i]=parts[i];
    for(unsigned i=3;i<68;i++){snprintf(names[i],48,"word/header%u.xml",i);many[i]=(ZipPart){names[i],"<w:hdr/>",0,0,0,0};}zip=zip_package(many,68,0,&n);zip_refusal(zip,n,"docx_auxiliary_count_limit");free(many);free(names);
    char depth[1024];size_t pos=0;for(unsigned i=0;i<65;i++){memcpy(depth+pos,"<w:r>",5);pos+=5;}for(unsigned i=0;i<65;i++){memcpy(depth+pos,"</w:r>",6);pos+=6;}depth[pos]=0;
    parts[3].text=depth;zip=zip_package(parts,4,0,&n);zip_refusal(zip,n,"docx_xml_depth_limit");
    char inflated[6001];memset(inflated,'A',6000);inflated[6000]=0;ZipPart expanded[9]={0};char expanded_names[9][48];for(unsigned i=0;i<9;i++){snprintf(expanded_names[i],48,"custom/expanded%u.xml",i);expanded[i]=(ZipPart){expanded_names[i],inflated,0,0,0,0};}zip=zip_package(expanded,9,8,&n);central=get32(zip+n-6);
    for(unsigned i=0;i<9;i++){CHECK(central+46<n);set32(zip+central+24,1048576);unsigned name=(unsigned)zip[central+28]|((unsigned)zip[central+29]<<8);central+=46+name;}zip_refusal(zip,n,"docx_expansion_limit");
    CHECK(!samosa_docx_extract((const unsigned char*)"PKbroken",8,65536,&out,&error));CHECK(error&&!strcmp(error,"docx_malformed"));printf("PASS DOCX native parser: %u checks; stored/Deflate, literal auxiliary text, metadata/structure, CRC, package/expansion/depth limits and unsafe input refusal\n",checks);return 0;
}
