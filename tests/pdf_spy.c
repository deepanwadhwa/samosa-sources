#define _POSIX_C_SOURCE 200809L
#include <fpdfview.h>
#include <fpdf_text.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static void *library;
static void *lookup(const char *name) {
    if(!library){const char *path=getenv("SAMOSA_TEST_REAL_PDFIUM");if(path)library=dlopen(path,RTLD_NOW|RTLD_LOCAL);}
    void *symbol=library?dlsym(library,name):NULL;if(!symbol){fprintf(stderr,"spy: missing %s\n",name);abort();}return symbol;
}
static void record(const char *kind,int number) {
    const char *path=getenv("SAMOSA_TEST_PDF_SPY_LOG");FILE *out=path?fopen(path,"a"):NULL;
    if(!out)abort();
    fprintf(out,"%s %d\n",kind,number);fclose(out);
}
/* Explicit signatures keep this test portable C11, matching the public ABI. */
#define FN(ret,name,args,call) ret FPDF_CALLCONV name args {typedef ret(FPDF_CALLCONV *Function) args;Function fn;void *symbol=lookup(#name);memcpy(&fn,&symbol,sizeof(fn));return fn call;}
#define VOID(name,args,call) void FPDF_CALLCONV name args {typedef void(FPDF_CALLCONV *Function) args;Function fn;void *symbol=lookup(#name);memcpy(&fn,&symbol,sizeof(fn));fn call;}
VOID(FPDF_InitLibrary,(void),())
void FPDF_CALLCONV FPDF_DestroyLibrary(void) {
    typedef void(FPDF_CALLCONV *Function)(void);Function fn;
    void *symbol=lookup("FPDF_DestroyLibrary");memcpy(&fn,&symbol,sizeof(fn));fn();
    dlclose(library);library=NULL;
}
FN(FPDF_DOCUMENT,FPDF_LoadMemDocument64,(const void *data,size_t n,FPDF_BYTESTRING password),(data,n,password))
VOID(FPDF_CloseDocument,(FPDF_DOCUMENT doc),(doc))
FN(int,FPDF_GetPageCount,(FPDF_DOCUMENT doc),(doc))
FPDF_PAGE FPDF_CALLCONV FPDF_LoadPage(FPDF_DOCUMENT doc,int index) {
    /* Record native-call entry before the deliberate stall. Owned helpers may
     * now be killed inside that stall instead of returning from the call. */
    record("page",index+1);
    const char *delay=getenv("SAMOSA_TEST_PDF_PAGE_DELAY_MS");if(delay){long ms=strtol(delay,NULL,10);if(ms>0&&ms<=2000){struct timespec pause={ms/1000,(ms%1000)*1000000};nanosleep(&pause,NULL);}}
    typedef FPDF_PAGE(FPDF_CALLCONV *Function)(FPDF_DOCUMENT,int);Function fn;void *symbol=lookup("FPDF_LoadPage");memcpy(&fn,&symbol,sizeof(fn));const char *changed=getenv("SAMOSA_TEST_MUTATE_READER"),*destination=getenv("SAMOSA_TEST_MUTATE_DESTINATION");if(changed&&destination){if(rename(changed,destination))abort();unsetenv("SAMOSA_TEST_MUTATE_READER");}return fn(doc,index);
}
VOID(FPDF_ClosePage,(FPDF_PAGE page),(page))
FPDF_TEXTPAGE FPDF_CALLCONV FPDFText_LoadPage(FPDF_PAGE page) {
    typedef FPDF_TEXTPAGE(FPDF_CALLCONV *Function)(FPDF_PAGE);Function fn;void *symbol=lookup("FPDFText_LoadPage");memcpy(&fn,&symbol,sizeof(fn));record("text",1);return fn(page);
}
VOID(FPDFText_ClosePage,(FPDF_TEXTPAGE text),(text))
FN(int,FPDFText_CountChars,(FPDF_TEXTPAGE text),(text))
FN(unsigned int,FPDFText_GetUnicode,(FPDF_TEXTPAGE text,int index),(text,index))
FN(unsigned long,FPDF_GetLastError,(void),())
