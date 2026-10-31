/*
 * vmsotel tests: readings with fixed values, for the golden payloads.
 * The numbers are shaped like the x86 probe results in
 * docs/findings.md, so a golden file reads like a real export.
 */

#ifndef VO_FIXTURE_H
#define VO_FIXTURE_H

#include "model.h"
#include "otlp_json.h"

/* 2026-10-31T20:17:49Z and 2026-10-23T15:48:29Z, in Unix nanoseconds. */
#define FIX_NOW_NS  1793477869000000000ULL
#define FIX_BOOT_NS 1792770509000000000ULL

void fixture_full(vo_raw *r);        /* every part present, three disks */
void fixture_minimal(vo_raw *r);     /* no pagefile, no disks, no CPU */
void fixture_resource(vo_resource *res);

#endif
