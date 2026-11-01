/*
 * vmsotel: log records, and the two parsers that make them (phase 3).
 *
 * The operator log is text records: a header line
 *     %%%%%%%%%%%  OPCOM   1-NOV-2026 09:53:17.66  %%%%%%%%%%%
 * then the message's lines, with an empty record before the next
 * header (findings 34). The audit journal is binary records in
 * NSADEF's NSAMSGDEF layout: a 20-byte header, then packets of word
 * size (header included), word type, data (findings 37).
 *
 * Both parsers are pure: they take records the source has read and
 * fill vo_log_rec, so all of it is tested on Linux. Times in both files
 * are VMS local time; tdf_s is the time differential to subtract,
 * seconds east of UTC, as SYS$TIMEZONE_DIFFERENTIAL gives it.
 */

#ifndef VO_LOGPARSE_H
#define VO_LOGPARSE_H

#include <stddef.h>

#include "vo_types.h"

#define VO_LOG_BODY   2048      /* longer bodies end in "..." */
#define VO_LOG_ATTRS  14
#define VO_LOG_VAL    128

enum { VO_LOG_OPCOM = 1, VO_LOG_AUDIT = 2 };

/* OTLP SeverityNumber values used here. */
enum {
    VO_SEV_INFO = 9,
    VO_SEV_WARN = 13,
    VO_SEV_ERROR = 17,
    VO_SEV_FATAL = 21
};

typedef struct {
    const char *key;            /* a string constant */
    int is_int;
    long long ival;
    char sval[VO_LOG_VAL];
} vo_log_attr;

typedef struct {
    int source;                 /* VO_LOG_OPCOM or VO_LOG_AUDIT */
    vo_u64 time_ns;             /* when it happened, Unix ns; 0 unknown */
    int severity;               /* VO_SEV_* */
    const char *severity_text;  /* "INFO", ... */
    const char *event_name;     /* audit: "openvms.audit.logfail"; or NULL */
    char body[VO_LOG_BODY];
    size_t nattrs;
    vo_log_attr attrs[VO_LOG_ATTRS];
} vo_log_rec;

/* VMS binary time (100 ns since 17-Nov-1858, local) to Unix ns, UTC. 0
   for a time before 1970. */
vo_u64 vo_vms_time_to_unix_ns(vo_u64 vms, long tdf_s);

/* ---- the operator log ---------------------------------------------- */

typedef struct {
    long tdf_s;
    int in_msg;                 /* a header has been seen */
    vo_log_rec rec;             /* the message being gathered */
    size_t body_len;
    size_t lines;
    int truncated;
} vo_opcom_parser;

void vo_opcom_init(vo_opcom_parser *p, long tdf_s);

/*
 * Feeds one record of the log. A header or an empty record ends the
 * message being gathered: then it is copied to out and 1 is returned.
 * A header also starts the next one. Lines before the first header are
 * not part of any message and are ignored.
 */
int vo_opcom_feed(vo_opcom_parser *p, const char *line, size_t len,
                  vo_log_rec *out);

/* The message being gathered, if any, as far as it goes: 1 and out
   filled, or 0. The parser is left as it was. */
int vo_opcom_pending(const vo_opcom_parser *p, vo_log_rec *out);

/* Lines in the message being gathered (0 with none), so a reader can
   tell whether it grew between two looks. */
size_t vo_opcom_pending_lines(const vo_opcom_parser *p);

/* 1 if line is an OPCOM header. */
int vo_opcom_is_header(const char *line, size_t len);

/* ---- the audit journal --------------------------------------------- */

/*
 * Decodes one journal record into out. 1 on success; 0 if it does not
 * fit NSAMSGDEF (sizes that do not add up), with out unusable.
 *
 * The breakin password packet (NSA$C_PKT_PASSWORD) and the command line
 * are never copied out: either can hold a password someone typed.
 */
int vo_nsa_decode(const unsigned char *b, size_t len, long tdf_s,
                  vo_log_rec *out);

/* "LOGFAIL" for NSA$C_MSG_LOGFAIL (5), ...; NULL for an unknown type. */
const char *vo_nsa_type_name(unsigned type);

#endif
