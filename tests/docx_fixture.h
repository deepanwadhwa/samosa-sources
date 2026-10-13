#ifndef SAMOSA_DOCX_FIXTURE_H
#define SAMOSA_DOCX_FIXTURE_H
#include <stdint.h>
/* Actual ZIP fixtures, with deterministic native stored or raw Deflate stored
 * blocks. No external archiver, compression API, network or Python. */
static void zip16(FILE *f,unsigned v){CHECK(fputc(v&255,f)!=EOF);CHECK(fputc((v>>8)&255,f)!=EOF);}
static void zip32(FILE *f,uint32_t v){zip16(f,v&65535);zip16(f,v>>16);}
static uint32_t zip_crc(const unsigned char *p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;i++){c^=p[i];for(unsigned b=0;b<8;b++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
typedef struct {const char *name,*text;unsigned size,offset,packed,crc;} ZipPart;
static unsigned char *zip_package(ZipPart p[],unsigned count,unsigned method,size_t *size){
    char *bytes=NULL;FILE *f=open_memstream(&bytes,size);CHECK(f);
    for(unsigned i=0;i<count;i++){p[i].size=(unsigned)strlen(p[i].text);CHECK(method!=8||p[i].size<=65535);p[i].packed=p[i].size+(method==8?5:0);p[i].crc=zip_crc((const unsigned char*)p[i].text,p[i].size);p[i].offset=(unsigned)ftell(f);zip32(f,0x04034b50);zip16(f,20);zip16(f,0);zip16(f,method);zip32(f,0);zip32(f,p[i].crc);zip32(f,p[i].packed);zip32(f,p[i].size);zip16(f,(unsigned)strlen(p[i].name));zip16(f,0);CHECK(fwrite(p[i].name,1,strlen(p[i].name),f)==strlen(p[i].name));if(method==8){CHECK(fputc(1,f)!=EOF);zip16(f,p[i].size);zip16(f,~p[i].size);}CHECK(fwrite(p[i].text,1,p[i].size,f)==p[i].size);}
    unsigned central=(unsigned)ftell(f);for(unsigned i=0;i<count;i++){zip32(f,0x02014b50);zip16(f,20);zip16(f,20);zip16(f,0);zip16(f,method);zip32(f,0);zip32(f,p[i].crc);zip32(f,p[i].packed);zip32(f,p[i].size);zip16(f,(unsigned)strlen(p[i].name));zip16(f,0);zip16(f,0);zip16(f,0);zip16(f,0);zip32(f,0);zip32(f,p[i].offset);CHECK(fwrite(p[i].name,1,strlen(p[i].name),f)==strlen(p[i].name));}
    unsigned length=(unsigned)ftell(f)-central;zip32(f,0x06054b50);zip16(f,0);zip16(f,0);zip16(f,count);zip16(f,count);zip32(f,length);zip32(f,central);zip16(f,0);CHECK(!fclose(f));return (unsigned char*)bytes;
}
static unsigned char *docx_package(const char *document,const char *extra,unsigned method,size_t *size){
    const char *types="<Types><Override PartName='/word/document.xml' ContentType='application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml'/></Types>";
    const char *rels="<Relationships><Relationship Type='officeDocument' Target='word/document.xml'/></Relationships>";
    ZipPart p[4]={{"[Content_Types].xml",types,0,0,0,0},{"_rels/.rels",rels,0,0,0,0},{"word/document.xml",document,0,0,0,0},{extra,"unused",0,0,0,0}};
    return zip_package(p,extra?4:3,method,size);
}
#endif
