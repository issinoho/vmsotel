/*
 * Linux stub of the system services the probes call. Arguments are
 * void * where the real starlet.h names a struct; the probes cast to
 * void * at every call so that either declaration accepts them.
 */
#ifndef STUB_STARLET_H
#define STUB_STARLET_H
int sys$getmsg(unsigned int msgid, unsigned short *msglen, void *bufadr,
               unsigned int flags, unsigned char *outadr);
int sys$gettim(void *timadr);
int sys$getsyiw(unsigned int efn, unsigned int *csidadr, void *nodename,
                void *itmlst, void *iosb, void (*astadr)(void), int astprm);
int sys$getrmi(unsigned int efn, unsigned int csidadr, unsigned int nodename,
               void *itmlst, void *iosb, void (*astadr)(void), int astprm);
int sys$synch(unsigned int efn, void *iosb);
int sys$getjpiw(unsigned int efn, unsigned int *pidadr, void *prcnam,
                void *itmlst, void *iosb, void (*astadr)(void), int astprm);
int sys$device_scan(void *return_devnam, unsigned short *retlen,
                    void *search_devnam, void *itmlst, void *contxt);
int sys$open(void *fab);
int sys$connect(void *rab);
int sys$get(void *rab);
int sys$close(void *fab);
/* Stub only: what REQUEST would do -- OPCOM logging the message. */
void probe_stub_request(const char *text);
int sys$trnlnm(unsigned int *attr, void *tabnam, void *lognam,
               unsigned char *acmode, void *itmlst);
int sys$getdviw(unsigned int efn, unsigned short chan, void *devnam,
                void *itmlst, void *iosb, void (*astadr)(void), int astprm,
                void *nullarg);
/* The launcher's. The stub knows one account, VMSOTEL; $CREPRC prints
   what it was asked to create instead of creating it. */
int sys$persona_create(unsigned int *persona, void *usrnam,
                       unsigned int flags, void *usrpro, void *itmlst);
int sys$persona_assume(unsigned int *persona, unsigned int flags,
                       void *retpersona, void *itmlst);
int sys$persona_delete(unsigned int *persona);
int sys$creprc(unsigned int *pidadr, void *image, void *input,
               void *output, void *error, void *prvadr, void *quota,
               void *prcnam, unsigned int baspri, unsigned int uic,
               unsigned short mbxunt, unsigned int stsflg, void *itmlst,
               void *node);
#endif
