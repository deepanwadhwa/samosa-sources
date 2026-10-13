#define _POSIX_C_SOURCE 200809L
#include <allheaders.h>
#include <stdio.h>
#include <stdlib.h>
/* Fixture-only encoders; no product link dependency or external converters. */
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    PIX *image = pixRead(argv[1]);
    if (!image) return 2;
    const struct {const char *name; int format;} formats[] = {
        {"bmp", IFF_BMP}, {"jpg", IFF_JFIF_JPEG}, {"png", IFF_PNG},
        {"tif", IFF_TIFF_ZIP}, {"gif", IFF_GIF}, {"webp", IFF_WEBP}};
    int rc = 0;
    for (size_t i = 0; i < sizeof(formats)/sizeof(*formats); i++) {
        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/sentinel.%s", argv[2], formats[i].name);
        if (n < 0 || (size_t)n >= sizeof(path) || pixWrite(path, image, formats[i].format)) {
            rc = 1;
            break;
        }
    }
    pixDestroy(&image);
    return rc;
}
