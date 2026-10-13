#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"DOCX fixture failed: %s\n",#x);exit(1);}}while(0)
#include "docx_fixture.h"
int main(int argc,char **argv){if(argc!=2)return 2;const char *xml="<w:document xmlns:w='x'><w:body><w:p><w:r><w:t>Code &amp; café 7319</w:t></w:r></w:p></w:body></w:document>";size_t n;unsigned char *zip=docx_package(xml,NULL,8,&n);FILE *f=fopen(argv[1],"wb");CHECK(f);CHECK(fwrite(zip,1,n,f)==n);CHECK(!fclose(f));free(zip);return 0;}
