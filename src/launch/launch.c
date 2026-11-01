/*
 * vmsotel_launch: start the agent's detached process as its own account.
 *
 *   vmsotel_launch ACCOUNT PROCEDURE LOGFILE PROCESS-NAME
 *   vmsotel_launch --password
 *
 * Run by VMSOTEL_START.COM in the kit's layout, as SYSTEM at boot.
 * RUN/DETACHED/UIC cannot do this: LOGINOUT keeps the creator's
 * username and UAF record, so the process would have SYSTEM's
 * authorized privileges (vms-beszel NOTES 36, shown on these two
 * machines). This takes on the account's persona, creates the LOGINOUT
 * process under it, and goes back to its own; LOGINOUT then logs in as
 * the account, with its UIC and only its authorized privileges. It
 * needs IMPERSONATE, which a system startup has.
 *
 * --password prints 32 random letters and digits from OpenSSL's
 * generator, for VMSOTEL$CONFIGURE.COM to give the account a password
 * nobody needs.
 *
 * Exits with the failing status, so DCL reports it. Built with
 * /POINTER_SIZE=32 like tls.c, since it calls the ..._SHR32 OpenSSL
 * images; the system services here take 32-bit pointers either way.
 */

#include <stdio.h>
#include <string.h>

#include <openssl/rand.h>

#include <descrip.h>
#include <pqldef.h>
#include <prcdef.h>
#include <ssdef.h>
#include <starlet.h>

#ifndef ISS$C_ID_NATURAL
#define ISS$C_ID_NATURAL 1
#endif

static void dsc(struct dsc$descriptor_s *d, char *s)
{
    d->dsc$w_length = (unsigned short) strlen(s);
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = s;
}

/* DCL lowercases unquoted arguments; account and process names are
   upper case. Returns 0 if s does not fit. */
static int upcase(char *out, size_t cap, const char *s)
{
    size_t i;

    for (i = 0; s[i] != '\0'; i++) {
        if (i + 1 >= cap)
            return 0;
        out[i] = (char) (s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    }
    out[i] = '\0';
    return i > 0;
}

/*
 * The quota list: entries of a one-byte PQL$_ code and a longword,
 * little-endian, ending with PQL$_LISTEND. Written byte by byte rather
 * than as a struct, which would be padded. The values are the ones
 * VMSOTEL_START.COM gives RUN/DETACHED in the source tree: the queue is
 * allocated at start, and TCP and TLS want buffered I/O.
 */
static size_t quota(unsigned char *b, size_t at, unsigned code,
                    unsigned long v)
{
    b[at] = (unsigned char) code;
    b[at + 1] = (unsigned char) (v & 0xff);
    b[at + 2] = (unsigned char) ((v >> 8) & 0xff);
    b[at + 3] = (unsigned char) ((v >> 16) & 0xff);
    b[at + 4] = (unsigned char) ((v >> 24) & 0xff);
    return at + 5;
}

static void quotas(unsigned char *b)
{
    size_t at = 0;

    at = quota(b, at, PQL$_ASTLM, 100);
    at = quota(b, at, PQL$_BYTLM, 200000);
    at = quota(b, at, PQL$_ENQLM, 500);
    at = quota(b, at, PQL$_FILLM, 100);
    at = quota(b, at, PQL$_BIOLM, 100);
    at = quota(b, at, PQL$_DIOLM, 100);
    at = quota(b, at, PQL$_PGFLQUOTA, 300000);
    at = quota(b, at, PQL$_TQELM, 20);
    at = quota(b, at, PQL$_WSDEFAULT, 4096);
    at = quota(b, at, PQL$_WSQUOTA, 16384);
    at = quota(b, at, PQL$_WSEXTENT, 32768);
    b[at] = PQL$_LISTEND;
}

/* A VMS status to DCL; off VMS (the stub build) to a shell's 0 or 1. */
static int done(int st)
{
#ifdef __VMS
    return st;
#else
    return (st & 1) ? 0 : 1;
#endif
}

static int password(void)
{
    static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned char r[64];
    char pw[33];
    size_t i, n = 0;

    /* 36 symbols: a byte below 252 (7 x 36) is taken mod 36, so every
       symbol is equally likely; the rest are discarded. */
    while (n < 32) {
        if (RAND_bytes(r, (int) sizeof r) != 1) {
            printf("vmsotel_launch: OpenSSL could not make random bytes\n");
            return SS$_ABORT;
        }
        for (i = 0; i < sizeof r && n < 32; i++)
            if (r[i] < 252)
                pw[n++] = set[r[i] % 36];
    }
    pw[32] = '\0';
    printf("%s\n", pw);
    memset(pw, 0, sizeof pw);
    return SS$_NORMAL;
}

int main(int argc, char **argv)
{
    static char loginout[] = "SYS$SYSTEM:LOGINOUT.EXE";
    struct dsc$descriptor_s user, image, in, out, name;
    unsigned char quo[5 * 11 + 1];
    char u[33], n[16];
    unsigned int persona = 0, natural = ISS$C_ID_NATURAL, pid = 0;
    int st, back;

    if (argc == 2 && strcmp(argv[1], "--password") == 0)
        return done(password());
    if (argc != 5) {
        printf("usage: vmsotel_launch ACCOUNT PROCEDURE LOGFILE "
               "PROCESS-NAME\n"
               "       vmsotel_launch --password\n");
        return done(SS$_BADPARAM);
    }
    if (!upcase(u, sizeof u, argv[1]) || !upcase(n, sizeof n, argv[4])) {
        printf("vmsotel_launch: the account (up to 32 characters) or the "
               "process name (up to 15) is empty or too long\n");
        return done(SS$_BADPARAM);
    }
    dsc(&user, u);
    dsc(&image, loginout);
    dsc(&in, argv[2]);
    dsc(&out, argv[3]);
    dsc(&name, n);
    quotas(quo);

    st = sys$persona_create(&persona, (void *) &user, 0, 0, 0);
    if (!(st & 1)) {
        printf("vmsotel_launch: cannot take on the account %s "
               "(status %%X%08X)\n", u, (unsigned) st);
        return done(st);
    }
    st = sys$persona_assume(&persona, 0, 0, 0);
    if (st & 1) {
        /* LOGINOUT, detached: it logs in as the persona's account and
           runs the procedure. Base priority 4, as an interactive user. */
        st = sys$creprc(&pid, (void *) &image, (void *) &in, (void *) &out,
                        (void *) &out, 0, (void *) quo, (void *) &name, 4,
                        0, 0, PRC$M_DETACH, 0, 0);
        /* Back to our own persona whatever happened: a failure here
           matters more than the process. */
        back = sys$persona_assume(&natural, 0, 0, 0);
        if ((st & 1) && !(back & 1))
            st = back;
    }
    (void) sys$persona_delete(&persona);
    if (!(st & 1)) {
        printf("vmsotel_launch: cannot create the process as %s "
               "(status %%X%08X)\n", u, (unsigned) st);
        return done(st);
    }
    printf("vmsotel_launch: process %08X started as %s\n", pid, u);
    return done(SS$_NORMAL);
}
