#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv){
    const char *root=argc==2?argv[1]:"vendor/sqlite";
    if(argc>2)return 2;
    char known[65];hash_bytes("",0,known);
    if(strcmp(known,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))return 2;
    const char *names[]={"sqlite3.c","sqlite3.h"};
    const char *expected[]={"b1dd5d74ec7f29055a6684fa06fb3c2f6821c87dd38f9a458dfd2e8a1db28189","919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d"};
    for(size_t i=0;i<2;i++){
        char path[8192];int length=snprintf(path,sizeof(path),"%s/%s",root,names[i]);
        if(length<0||(size_t)length>=sizeof(path))return 2;
        FILE *file=fopen(path,"rb");if(!file){fprintf(stderr,"missing SQLite source: %s\n",names[i]);return 1;}
        Sha256 hash;sha256_init(&hash);unsigned char buffer[65536],digest[32];size_t n;
        while((n=fread(buffer,1,sizeof(buffer),file)))sha256_update(&hash,buffer,n);
        int failed=ferror(file);if(fclose(file))failed=1;if(failed)return 1;
        sha256_final(&hash,digest);char actual[65];sha256_hex(digest,actual);
        if(strcmp(actual,expected[i])){fprintf(stderr,"SQLite source digest mismatch: %s\n",names[i]);return 1;}
    }
    puts("PASS pinned SQLite source digests");return 0;
}
