#ifndef SOURCES_PDF_FIXTURE_H
#define SOURCES_PDF_FIXTURE_H
#include <stdio.h>
/* Small native PDF writer; real page objects/xref, no generator interpreter.
 * Page2 blank, page97 sentinel; each other page has a distinct absolute label. */
static inline int pdf_fixture(const char *path,int pages) {
    if(pages<1||pages>100)return 0;
    FILE *out=fopen(path,"wb");if(!out)return 0;
    long offsets[204]={0};int objects=3+pages*2;
    fputs("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n",out);
    offsets[1]=ftell(out);fputs("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",out);
    offsets[2]=ftell(out);fprintf(out,"2 0 obj\n<< /Type /Pages /Count %d /Kids [",pages);
    for(int i=0;i<pages;i++)fprintf(out,"%d 0 R ",4+i*2);
    fputs("] >>\nendobj\n",out);
    offsets[3]=ftell(out);fputs("3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n",out);
    for(int i=0;i<pages;i++) {
        int page=4+i*2,content=page+1;offsets[page]=ftell(out);
        fprintf(out,"%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>\nendobj\n",page,content);
        char text[128],stream[256];
        if(i+1==97)snprintf(text,sizeof(text),"The code on absolute page 97 is 7319.");
        else snprintf(text,sizeof(text),"This is absolute page %d.",i+1);
        int n=i+1==2?0:snprintf(stream,sizeof(stream),"BT /F1 12 Tf 72 730 Td (%s) Tj ET\n",text);
        offsets[content]=ftell(out);fprintf(out,"%d 0 obj\n<< /Length %d >>\nstream\n",content,n);fwrite(stream,1,(size_t)n,out);fputs("endstream\nendobj\n",out);
    }
    long xref=ftell(out);fprintf(out,"xref\n0 %d\n0000000000 65535 f \n",objects+1);
    for(int i=1;i<=objects;i++)fprintf(out,"%010ld 00000 n \n",offsets[i]);
    fprintf(out,"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n",objects+1,xref);
    int valid=!ferror(out);return fclose(out)==0&&valid;
}
#endif
