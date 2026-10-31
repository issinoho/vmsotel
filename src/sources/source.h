/*
 * vmsotel: the source interface.
 *
 * A source reads the system into a vo_raw and describes the host for
 * the resource attributes. The VMS source (step 2c) does it with
 * $GETRMI, $GETSYI and $GETDVI; the fake source in sources/fake/ with
 * scripted values, so the whole agent runs on Linux.
 */

#ifndef VO_SOURCE_H
#define VO_SOURCE_H

#include <stddef.h>

#include "model.h"
#include "otlp_json.h"

/* 1 on success, 0 with a reason in err. */
int vo_source_open(char *err, size_t errcap);

/* Fills r with one reading taken at now_ns (Unix ns, UTC). 1 on
   success, 0 with a reason in err. */
int vo_source_read(vo_raw *r, vo_u64 now_ns, char *err, size_t errcap);

/* Fills the host's part of the resource; the strings stay valid until
   vo_source_close. */
void vo_source_resource(vo_resource *res);

void vo_source_close(void);

#endif
