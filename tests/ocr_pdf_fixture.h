#ifndef SAMOSA_OCR_PDF_FIXTURE_H
#define SAMOSA_OCR_PDF_FIXTURE_H
#include <stdio.h>
#include <string.h>
/* Native image-only PDF: page1 blank, page2 sentinel raster, no fonts/text layer.
 * Oversized variant tests pre-allocation render dimension refusal. */
static inline int ocr_pdf_fixture_mixed(const char *path,const unsigned char *ppm,size_t n,int oversized,int text_layer){
    static const char header[]="P6\n680 150\n255\n";
    if(n!=sizeof(header)-1+680u*150u*3u||memcmp(ppm,header,sizeof(header)-1))return 0;
    FILE *out=fopen(path,"wb");if(!out)return 0;long offsets[9]={0};
    fputs("%PDF-1.4\n",out);
    offsets[1]=ftell(out);fputs("1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n",out);
    offsets[2]=ftell(out);fputs("2 0 obj << /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >> endobj\n",out);
    offsets[3]=ftell(out);fprintf(out,"3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 680 150] /Resources << %s >> /Contents 4 0 R >> endobj\n",text_layer?"/Font << /F1 8 0 R >>":"");
    const char *layer=text_layer?"BT /F1 24 Tf 20 70 Td (EXACT TEXT LAYER 2468) Tj ET\n":"";
    offsets[4]=ftell(out);fprintf(out,"4 0 obj << /Length %zu >> stream\n%sendstream endobj\n",strlen(layer),layer);
    offsets[5]=ftell(out);fprintf(out,"5 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 %u %u] /Resources << /XObject << /Im0 7 0 R >> >> /Contents 6 0 R >> endobj\n",oversized?10000u:680u,oversized?10000u:150u);
    static const char commands[]="q 680 0 0 150 0 0 cm /Im0 Do Q\n";
    offsets[6]=ftell(out);fprintf(out,"6 0 obj << /Length %zu >> stream\n%s",sizeof(commands)-1,commands);fputs("endstream endobj\n",out);
    offsets[7]=ftell(out);fprintf(out,"7 0 obj << /Type /XObject /Subtype /Image /Width 680 /Height 150 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length %u >> stream\n",680u*150u*3u);
    fwrite(ppm+sizeof(header)-1,1,680u*150u*3u,out);fputs("\nendstream endobj\n",out);
    if(text_layer){offsets[8]=ftell(out);fputs("8 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n",out);}
    unsigned objects=text_layer?9:8;long xref=ftell(out);fprintf(out,"xref\n0 %u\n0000000000 65535 f \n",objects);for(unsigned i=1;i<objects;i++)fprintf(out,"%010ld 00000 n \n",offsets[i]);
    fprintf(out,"trailer << /Size %u /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n",objects,xref);int ok=!ferror(out);return !fclose(out)&&ok;
}
static inline int ocr_pdf_fixture(const char *path,const unsigned char *ppm,size_t n,int oversized){return ocr_pdf_fixture_mixed(path,ppm,n,oversized,0);}
#endif
