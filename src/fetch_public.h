#ifndef SOURCES_FETCH_PUBLIC_H
#define SOURCES_FETCH_PUBLIC_H
#include "sources.h"
#include <netdb.h>
#define SOURCES_FETCH_ADDRESS_MAX 46u
#define SOURCES_FETCH_URL_MAX 2048u
#define SOURCES_FETCH_MAX_HOPS 6u
typedef struct {
    size_t count;
    char urls[SOURCES_FETCH_MAX_HOPS][SOURCES_FETCH_URL_MAX+1];
} SourcesFetchRoute;
const char *sources_fetch_route_begin(const SourcesConfig *,const char *,SourcesFetchRoute *);
const char *sources_fetch_route_next(const char *,SourcesFetchRoute *);
const char *sources_fetch_url_origin(const char *,char [257]);
const char *sources_fetch_redirect_url(const char *,const char *,char [SOURCES_FETCH_URL_MAX+1]);
int sources_fetch_robots_allowed(const char *,const char *,const char *);
const char *sources_fetch_rate_wait(const SourcesConfig *,const char *);
int sources_fetch_address_blocked(const char *);
const char *sources_fetch_addresses(const struct addrinfo *,char [SOURCES_FETCH_ADDRESS_MAX]);
const char *sources_fetch_resolve(const SourcesConfig *,char [256],unsigned *,char [SOURCES_FETCH_ADDRESS_MAX]);
int sources_fetch_address_equal(const char *,const char *);
#endif
