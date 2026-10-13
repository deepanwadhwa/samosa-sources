#ifndef SOURCES_TEMPORAL_H
#define SOURCES_TEMPORAL_H
#include "visual.h"
typedef struct {
    SourcesPreparedFrame frames[16];
    size_t count, start_us, end_us;
    char input_sha256[65], prepared_sha256[65];
} SourcesTemporalInput;
/* Prepared bodies come from this component's media reader. No paths or raw
 * model APIs cross the public Inference boundary. Caller owns returned DTO. */
const char *sources_temporal_input(const SourcesConfig *, const char *const *,
                                   const size_t *, size_t, size_t, size_t,
                                   const char *, SourcesTemporalInput *,
                                   char **, size_t *);
void sources_temporal_input_release(SourcesTemporalInput *);
const char *sources_temporal_response(const SourcesConfig *,
                                      const SourcesTemporalInput *,
                                      const char *, size_t);
const char *sources_temporal_prepare(const SourcesConfig *, sqlite3 *,
                                     const char *, size_t, size_t, const char *,
                                     SourcesTemporalInput *, char **, size_t *);
const char *sources_temporal_hashes(SourcesTemporalInput *);
int sources_temporal_dispatch(const SourcesConfig *, const Json *, sqlite3 *,
                              FILE *);
#endif
