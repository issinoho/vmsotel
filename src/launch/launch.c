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
 *
 * VAX (OpenVMS V7.3; findings V28) differs in three ways:
 *
 *  - Its persona services are the older interface: $PERSONA_CREATE
 *    takes persona, username and flags, $PERSONA_ASSUME persona and
 *    flags. Assume with flags 0 succeeds and takes on nothing, so the
 *    process was created as SYSTEM with every privilege, while this
 *    reported it started as the account. IMP$M_ASSUME_SECURITY makes
 *    $CREPRC use the persona; ACCOUNT goes with it, as tested.
 *  - So that a silent fall-back like that can never start the agent,
 *    the username is checked once the persona is assumed, and the
 *    launch refused if it is not the account (VO_LAUNCH_CHECK_ID,
 *    automatic on VAX; x86 and I64 are proven without it, findings 45).
 *  - No OpenSSL (VO_LAUNCH_NO_OPENSSL, automatic on VAX): --password
 *    comes from the clock's jitter and this process's counters, mixed.
 *    That is not a cryptographic generator. It does not need to be: the
 *    account has no login class at all, so the password is never
 *    usable; it only has to be one nobody knows. AUTHORIZE's own
 *    generator cannot stand in, as it asks the terminal to choose.
 */

#include <stdio.h>
#include <string.h>

#ifdef __VAX
#  ifndef VO_LAUNCH_NO_OPENSSL
#    define VO_LAUNCH_NO_OPENSSL 1
#  endif
#  ifndef VO_LAUNCH_CHECK_ID
#    define VO_LAUNCH_CHECK_ID 1
#  endif
#endif

#ifndef VO_LAUNCH_NO_OPENSSL
#  include <openssl/rand.h>
#endif

#include <descrip.h>
#include <jpidef.h>
#include <pqldef.h>
#include <prcdef.h>
#include <ssdef.h>
#include <starlet.h>

#ifdef __VAX
#  include <impdef.h>
#  define PERSONA_CREATE(p, u) sys$persona_create((p), (u), 0)
#  define PERSONA_ASSUME(p, f) sys$persona_assume((p), (f))
#  define ASSUME_FLAGS (IMP$M_ASSUME_SECURITY | IMP$M_ASSUME_ACCOUNT)
#else
#  define PERSONA_CREATE(p, u) sys$persona_create((p), (u), 0, 0, 0)
#  define PERSONA_ASSUME(p, f) sys$persona_assume((p), (f), 0, 0)
#  define ASSUME_FLAGS 0
#endif

#ifndef ISS$C_ID_NATURAL
#define ISS$C_ID_NATURAL 1
#endif

#if defined(VO_LAUNCH_NO_OPENSSL) || defined(VO_LAUNCH_CHECK_ID)
/* An item_list_3 entry, written out: 12 bytes with 32-bit pointers. */
typedef struct {
    unsigned short len, code;
    void *buf;
    void *retlen;
} item3;

static void item(item3 *e, unsigned code, void *buf, unsigned len,
                 void *retlen)
{
    e->len = (unsigned short) len;
    e->code = (unsigned short) code;
    e->buf = buf;
    e->retlen = retlen;
}
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

#ifdef VO_LAUNCH_NO_OPENSSL
/* Mixes a longword into h: murmur3's finaliser after an xor, which
   spreads every input bit over all 32. Masked: unsigned int is 32 bits
   wherever this builds (vo_types.h checks it for the agent). */
static unsigned int mix(unsigned int h, unsigned int v)
{
    h = (h ^ v) & 0xFFFFFFFFu;
    h = ((h ^ (h >> 16)) * 0x85EBCA6Bu) & 0xFFFFFFFFu;
    h = ((h ^ (h >> 13)) * 0xC2B2AE35u) & 0xFFFFFFFFu;
    return (h ^ (h >> 16)) & 0xFFFFFFFFu;
}

static int password(void)
{
    static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned int lane[8], t[2], prev[2], jpi[5], spins, ctr = 0;
    unsigned int retlen[5];
    item3 il[6];
    char pw[33];
    int i, n = 0, st;

    for (i = 0; i < 8; i++)
        lane[i] = mix(0x6A09E667u, (unsigned int) i);

    /* This process's PID and counters, and where its stack is. */
    item(&il[0], JPI$_PID, &jpi[0], 4, &retlen[0]);
    item(&il[1], JPI$_CPUTIM, &jpi[1], 4, &retlen[1]);
    item(&il[2], JPI$_BUFIO, &jpi[2], 4, &retlen[2]);
    item(&il[3], JPI$_DIRIO, &jpi[3], 4, &retlen[3]);
    item(&il[4], JPI$_PAGEFLTS, &jpi[4], 4, &retlen[4]);
    item(&il[5], 0, NULL, 0, NULL);
    memset(jpi, 0, sizeof jpi);
    st = sys$getjpiw(0, NULL, NULL, il, NULL, NULL, 0);
    if (!(st & 1)) {
        printf("vmsotel_launch: $GETJPI failed (status %%X%08X)\n",
               (unsigned) st);
        return st;
    }
    for (i = 0; i < 5; i++)
        lane[i] = mix(lane[i], jpi[i]);
    lane[5] = mix(lane[5], (unsigned int) (size_t) &st);

    /* The clock, and how many turns of a loop pass before it moves:
       the scheduler's and the emulator's timing, 256 times. On VAX the
       clock moves every 10 ms, so this takes a few seconds. */
    if (!(sys$gettim(prev) & 1))
        return SS$_ABORT;
    for (i = 0; i < 256; i++) {
        spins = 0;
        do {
            if (!(sys$gettim(t) & 1))
                return SS$_ABORT;
            spins++;
        } while (t[0] == prev[0] && t[1] == prev[1] && spins < 10000000u);
        prev[0] = t[0];
        prev[1] = t[1];
        lane[i & 7] = mix(lane[i & 7], spins);
        lane[(i + 1) & 7] = mix(lane[(i + 1) & 7], t[0]);
        lane[(i + 3) & 7] = mix(lane[(i + 3) & 7], lane[i & 7]);
    }

    /* 36 symbols: a byte below 252 (7 x 36) is taken mod 36, so every
       symbol is equally likely; the rest are discarded. */
    while (n < 32) {
        unsigned int w = mix(lane[ctr & 7], ctr);
        int b;

        lane[ctr & 7] = mix(lane[ctr & 7], lane[(ctr + 5) & 7]);
        ctr++;
        for (b = 0; b < 4 && n < 32; b++) {
            unsigned int c = (w >> (8 * b)) & 0xFF;

            if (c < 252)
                pw[n++] = set[c % 36];
        }
    }
    pw[32] = '\0';
    printf("%s\n", pw);
    memset(pw, 0, sizeof pw);
    memset(lane, 0, sizeof lane);
    return SS$_NORMAL;
}
#else
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
#endif

#ifdef VO_LAUNCH_CHECK_ID
/* 1 if this process's username, under the assumed persona, is u. */
static int is_account(const char *u)
{
    char un[13];
    unsigned int retlen = 0;
    size_t n;
    item3 il[2];

    memset(un, ' ', sizeof un);
    item(&il[0], JPI$_USERNAME, un, 12, &retlen);
    item(&il[1], 0, NULL, 0, NULL);
    if (!(sys$getjpiw(0, NULL, NULL, il, NULL, NULL, 0) & 1))
        return 0;
    for (n = 12; n > 0 && (un[n - 1] == ' ' || un[n - 1] == '\0'); n--)
        ;
    return n == strlen(u) && memcmp(un, u, n) == 0;
}
#endif

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

    st = PERSONA_CREATE(&persona, (void *) &user);
    if (!(st & 1)) {
        printf("vmsotel_launch: cannot take on the account %s "
               "(status %%X%08X)\n", u, (unsigned) st);
        return done(st);
    }
    st = PERSONA_ASSUME(&persona, ASSUME_FLAGS);
#ifdef VO_LAUNCH_CHECK_ID
    if ((st & 1) && !is_account(u)) {
        printf("vmsotel_launch: the persona of %s was assumed, but this "
               "process is not %s: refusing to start the agent as "
               "anyone else\n", u, u);
        (void) PERSONA_ASSUME(&natural, 0);
        (void) sys$persona_delete(&persona);
        return done(SS$_NOPRIV);
    }
#endif
    if (st & 1) {
        /* LOGINOUT, detached: it logs in as the persona's account and
           runs the procedure. Base priority 4, as an interactive user. */
        st = sys$creprc(&pid, (void *) &image, (void *) &in, (void *) &out,
                        (void *) &out, 0, (void *) quo, (void *) &name, 4,
                        0, 0, PRC$M_DETACH, 0, 0);
        /* Back to our own persona whatever happened: a failure here
           matters more than the process. */
        back = PERSONA_ASSUME(&natural, 0);
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
