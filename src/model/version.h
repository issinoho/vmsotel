/*
 * vmsotel: the version, in one place. It goes out as service.version,
 * the instrumentation scope's version and the User-Agent.
 */

#ifndef VO_VERSION_H
#define VO_VERSION_H

#define VO_VERSION "0.1.0"

/* The same version as PCSI writes it, for the kit (kit/make_kit.com
   reads both and refuses images whose --version is not VO_VERSION). */
#define VO_PCSI_VERSION "V0.1-0"

#endif
