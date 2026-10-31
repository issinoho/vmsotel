/*
 * vmsotel: OTLP JSON encoder.
 *
 * Writes one ExportMetricsServiceRequest, in the protobuf JSON mapping
 * that OTLP/HTTP accepts with Content-Type: application/json, into a
 * buffer the caller supplies. No allocation and no I/O.
 *
 * Metric names, instruments, units and attribute values follow
 * OpenTelemetry semantic conventions v1.44.0 (VO_SEMCONV_VERSION),
 * where one fits; docs/design.md says where vmsotel adds its own.
 */

#ifndef VO_OTLP_JSON_H
#define VO_OTLP_JSON_H

#include <stddef.h>

#include "model.h"

#define VO_SEMCONV_VERSION "1.44.0"
#define VO_SCHEMA_URL      "https://opentelemetry.io/schemas/1.44.0"

typedef struct {
    const char *key;
    const char *value;
} vo_attr;

/*
 * Resource attributes. A NULL string is left out. cluster_member is
 * 1 or 0, or -1 to leave it out. extra holds the configured additions,
 * written after the rest.
 */
typedef struct {
    const char *service_version;
    const char *host_name;
    const char *host_arch;              /* "amd64", "ia64" */
    const char *host_type;
    const char *os_version;
    const char *scsnode;
    int cluster_member;
    const vo_attr *extra;
    size_t nextra;
} vo_resource;

enum {
    VO_OK = 0,
    VO_ESPACE = 1        /* buffer too small; *len is the size needed */
};

/*
 * Encodes res and snap into buf, of cap bytes. On VO_OK, *len is the
 * number of bytes written; the output is not NUL-terminated. On
 * VO_ESPACE, the buffer holds nothing usable and *len is the number of
 * bytes the payload needs, so a caller can report it or retry.
 */
int vo_otlp_encode(const vo_resource *res, const vo_snapshot *snap,
                   char *buf, size_t cap, size_t *len);

#endif
