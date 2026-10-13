#ifndef SAMOSA_SOURCES_SERVICE_UPLOAD_H
#define SAMOSA_SOURCES_SERVICE_UPLOAD_H
#include "service_http.h"
#include "sources.h"
/* Blocking is bounded by the supplied host reader; all framing is validated
 * before the native immutable upload can commit. */
int sources_service_upload(const SourcesConfig *, const SourcesHttpRequest *,
                           SourcesUploadRead, void *, FILE *);
#endif
