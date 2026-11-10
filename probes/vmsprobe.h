/*
 * vmsotel probes: shared helpers for the system-service probes.
 *
 * Every probe here asks one question about one system service and
 * prints enough that the answer can be read from a pasted transcript
 * without a second round trip: which items the headers declare, which
 * ones the service accepts, what came back, and how big it was.
 *
 * Items are queried one at a time. A single bad item code fails a
 * whole item list with SS$_BADPARAM, which would hide every good item
 * behind the one bad one.
 *
 * C89 and no 64-bit integer type, so that the same probes build on
 * VAX, where Compaq C V6.4 has neither long long nor __int64
 * (vms-hass NOTES.md 15). Quadwords are a pair of longwords, probe_q,
 * on every machine, so the VAX arithmetic is the arithmetic tested on
 * Linux. The helpers live in vmsprobe.c, linked into every probe.
 */

#ifndef VMSPROBE_H
#define VMSPROBE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <descrip.h>
#include <efndef.h>
#include <ssdef.h>
#include <starlet.h>

/*
 * Item list entry, item_list_3 layout: word length, word code, then a
 * 32-bit buffer address and a 32-bit return-length address.
 *
 * Declared here rather than taken from <iledef.h> so the probes rest on
 * one fewer header whose spelling is unconfirmed on these machines. The
 * layout is only right with 32-bit pointers, which is VSI C's default
 * and what build_probes.com asks for; probe_check_ile() refuses to run
 * otherwise, because a wrong layout is not a compile error, it is a
 * service writing through a garbage address.
 */
typedef struct {
    unsigned short len;
    unsigned short code;
    void *buf;
    unsigned short *retlen;
} probe_ile3;

/* How a returned buffer is printed. */
#define PK_LONG 1   /* unsigned, up to 64 bits, little-endian */
#define PK_TEXT 2   /* fixed-length text, trimmed at retlen */
#define PK_TIME 3   /* VMS quadword time, 100 ns since 17-NOV-1858 */

/* An item the headers do not declare. Codes are never negative. */
#define PROBE_UNDECLARED (-1)

typedef struct {
    const char *name;
    int code;
    unsigned short size;
    int kind;
} probe_item;

/*
 * An unsigned quadword as two longwords. unsigned int is 32 bits on
 * every VMS architecture and on Linux (unsigned long is 64 on Linux);
 * probe_banner() checks that, and the arithmetic, at run time.
 */
typedef struct {
    unsigned int lo, hi;
} probe_q;

/* Big enough for 18446744073709551615 and a sign. */
#define PROBE_Q_DIGITS 22

/*
 * Offset between the VMS and Unix epochs in 100 ns units:
 * 40587 days (the Modified Julian Day of 1970-01-01) x 86400 s x 10^7
 * = 35067168000000000 = 0x007C95674BEB4000.
 */
#define PROBE_VMS_TO_UNIX_HI 0x007C9567u
#define PROBE_VMS_TO_UNIX_LO 0x4BEB4000u

extern int probe_failures;

void probe_ok(const char *what);
void probe_fail(const char *what);
const char *probe_msg(unsigned int status);

/* Little-endian read of up to 8 bytes, or of up to 4 for the second. */
probe_q probe_le(const unsigned char *b, unsigned n);
unsigned int probe_le32(const unsigned char *b, unsigned n);

probe_q probe_q_make(unsigned int hi, unsigned int lo);
probe_q probe_q_add(probe_q a, probe_q b);
probe_q probe_q_sub(probe_q a, probe_q b);         /* modulo 2^64 */
int probe_q_cmp(probe_q a, probe_q b);             /* -1, 0 or 1 */
/* Divides *q by d in place, 0 < d < 2^31, and returns the remainder. */
unsigned int probe_q_div(probe_q *q, unsigned int d);
double probe_q_double(probe_q q);
/* Decimal into buf, at least PROBE_Q_DIGITS bytes; returns buf. */
char *probe_q_str(probe_q q, char *buf);
/* b - a as "+N" or "-N", for a delta that may run backwards. */
char *probe_q_delta_str(probe_q a, probe_q b, char *buf);

/* $GETTIM as a quadword. */
probe_q probe_gettim(void);
/* A VMS time as Unix seconds, as written (local, not corrected).
   0 when it is before 1970. */
long probe_vms_to_unix_s(probe_q t);
void probe_print_value(const probe_item *it, const unsigned char *buf,
                       unsigned short retlen);
int probe_check_ile(void);
void probe_banner(const char *name);
int probe_summary(void);

/*
 * Declare an item table entry. Each must be spelled out under its own
 * #ifdef by the caller -- the preprocessor cannot test inside a macro
 * -- so this only saves the typing of the entry itself.
 */
#define PROBE_ITEM(nm, sz, kd)      { #nm, nm, sz, kd }
#define PROBE_MISSING(nm, sz, kd)   { #nm, PROBE_UNDECLARED, sz, kd }

#endif
