#ifndef SAMOSA_SOURCES_SERVICE_H
#define SAMOSA_SOURCES_SERVICE_H
#include "service_http.h"
#include "sources.h"
/* Dedicated daemon-process loopback HTTP adapter; the caller process owns only
 * service children. Auth bytes have already been loaded through checked
 * owner-only host configuration. Port zero requests an OS-selected test port.
 */
int sources_service_run(const SourcesConfig *, const SourcesHttpAuth *,
                        unsigned, size_t, FILE *);
#endif
