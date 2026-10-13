#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
/* Native fixture rendering from an explicitly configured font. The same font
 * file yields the same raster on qualified FreeType builds; no external image
 * conversion, platform GUI, Python or JavaScript. */
int main(int argc,char **argv){if(argc!=3)return 2;const char *text="TOKEN 7319";const unsigned width=680,height=150;FT_Library library=NULL;FT_Face face=NULL;unsigned char *pixels=NULL;int rc=2;
    if(FT_Init_FreeType(&library)||FT_New_Face(library,argv[2],0,&face)||FT_Set_Pixel_Sizes(face,0,64))goto done;
    pixels=malloc(width*height*3);if(!pixels)goto done;memset(pixels,255,width*height*3);int pen=40,baseline=100;
    for(unsigned i=0;text[i];i++){if(FT_Load_Char(face,(unsigned char)text[i],FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL))goto done;FT_GlyphSlot glyph=face->glyph;FT_Bitmap *bitmap=&glyph->bitmap;if(bitmap->pixel_mode!=FT_PIXEL_MODE_GRAY||bitmap->pitch<0)goto done;
        for(unsigned y=0;y<bitmap->rows;y++)for(unsigned x=0;x<bitmap->width;x++){int xx=pen+glyph->bitmap_left+(int)x,yy=baseline-glyph->bitmap_top+(int)y;if(xx<0||yy<0||xx>=(int)width||yy>=(int)height)goto done;unsigned char shade=(unsigned char)(255-bitmap->buffer[(size_t)y*(unsigned)bitmap->pitch+x]);size_t at=((size_t)yy*width+(unsigned)xx)*3;memset(pixels+at,shade,3);}
        pen+=(int)(glyph->advance.x>>6);
    }
    FILE *f=fopen(argv[1],"wb");if(!f)goto done;rc=fprintf(f,"P6\n%u %u\n255\n",width,height)<0||fwrite(pixels,1,width*height*3,f)!=width*height*3;if(fclose(f))rc=1;
done: free(pixels);if(face)FT_Done_Face(face);if(library)FT_Done_FreeType(library);return rc;}
