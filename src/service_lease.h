#ifndef SAMOSA_SOURCES_SERVICE_LEASE_H
#define SAMOSA_SOURCES_SERVICE_LEASE_H
#include "sources.h"
typedef struct {
    int directory, service, queue;
} SourcesServiceLease;
const char *sources_service_lease_open(const SourcesConfig *,
                                       SourcesServiceLease *);
int sources_service_lease_valid(const SourcesConfig *,
                                const SourcesServiceLease *);
void sources_service_lease_close(SourcesServiceLease *);
#endif
