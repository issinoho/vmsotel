/*
 * vmsotel: the configuration file.
 *
 * Lines of `key = value`; blank lines and lines starting with # or !
 * are ignored. Keys are lower-case. Values run to the end of the line,
 * trimmed. For example:
 *
 *     host     = 192.168.0.218
 *     port     = 4319
 *     tls      = yes
 *     cafile   = SYS$MANAGER:VMSOTEL_CA.PEM
 *     interval = 60
 *     resource.deployment.environment = lab
 */

#ifndef VO_CONFIG_H
#define VO_CONFIG_H

#include <stddef.h>

#include "otlp_json.h"

#define VO_CONFIG_MAX_EXTRA 16
#define VO_CONFIG_STR       256

typedef struct {
    char host[VO_CONFIG_STR];
    char port[16];
    char path[VO_CONFIG_STR];
    int tls;
    char cafile[VO_CONFIG_STR];
    unsigned interval_s;
    unsigned timeout_s;
    unsigned queue_len;
    unsigned long max_payload;

    size_t nextra;
    vo_attr extra[VO_CONFIG_MAX_EXTRA];
    char extra_key[VO_CONFIG_MAX_EXTRA][VO_CONFIG_STR];
    char extra_value[VO_CONFIG_MAX_EXTRA][VO_CONFIG_STR];
} vo_config;

void vo_config_defaults(vo_config *c);

/*
 * Parses text (NUL-terminated) into c, over its defaults. Returns 0 on
 * success; otherwise the number of the first bad line, or -1 for a
 * problem with the whole file (no host), with a reason in err.
 */
int vo_config_parse(vo_config *c, const char *text, char *err, size_t errcap);

#endif
