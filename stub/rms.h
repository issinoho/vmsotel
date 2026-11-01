/*
 * Linux stub of the RMS structures probe_logs uses: only those fields,
 * with arbitrary codes. The stub's sys$open maps the operator log and
 * the audit journal to files under build/ and makes them if missing.
 */
#ifndef STUB_RMS_H
#define STUB_RMS_H

struct FAB {
    char *fab$l_fna;
    unsigned char fab$b_fns;
    unsigned char fab$b_fac;
    unsigned char fab$b_shr;
    unsigned char fab$b_rfm;
    unsigned char fab$b_rat;
    unsigned short fab$w_mrs;
    void *stub_fp;
    int stub_kind;
};

struct RAB {
    struct FAB *rab$l_fab;
    char *rab$l_ubf;
    unsigned short rab$w_usz;
    unsigned short rab$w_rsz;
    char *rab$l_rbf;
    unsigned int rab$l_rop;
};

extern struct FAB cc$rms_fab;
extern struct RAB cc$rms_rab;

#define FAB$M_GET    0x02
#define FAB$M_SHRGET 0x02
#define FAB$M_SHRPUT 0x01
#define FAB$M_SHRUPD 0x08
#define FAB$C_UDF   0
#define FAB$C_FIX   1
#define FAB$C_VAR   2
#define FAB$C_VFC   3
#define FAB$C_STM   4
#define FAB$C_STMLF 5
#define FAB$C_STMCR 6
#define RAB$M_NLK   0x00200000
#define RAB$M_RRL   0x00100000

#endif
