/*
 * ubmc_ipmi_lan - minimal IPMI 2.0 / RMCP+ server for the Silicom ATT-V150 uBMC
 *
 * The stock firmware only speaks IPMI over KCS (ubmc_ipmi + ubmc_ipmi_slave.ko),
 * so "ipmitool -I lanplus" has nothing to talk to on UDP/623.  This daemon adds
 * that listener and maps the commands that matter onto the tooling the firmware
 * already ships:
 *
 *   chassis status / control  ->  /usr/sbin/ubmc_sys_ctrl.sh (-q -o -s -f -r -c)
 *   mc info                   ->  static device id
 *
 * Only cipher suite 3 is implemented (HMAC-SHA1 authentication, HMAC-SHA1-96
 * integrity, AES-CBC-128 confidentiality) because that is what ipmitool's
 * lanplus and most clients use by default.
 *
 * Credentials: RMCP+ derives its keys from the *plaintext* password
 * (RAKP2/RAKP3 are HMACs keyed with it), so /etc/shadow is useless here -- a
 * one-way hash cannot produce those MACs.  Real BMCs therefore keep a separate
 * IPMI user list.  Ours lives in /config/ipmi-lan.passwd (persistent ext4
 * partition), one "user:password" per line.
 *
 * Build (cross):
 *   arm-linux-gnueabihf-gcc --sysroot=$SYS -O2 -o ubmc_ipmi_lan \
 *       ubmc_ipmi_lan.c -lcrypto
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define IPMI_PORT        623
#define MAX_SESSIONS     8
#define MAX_PKT          1024
#define MAX_SOCKS        8
#define AUTHCODE_LEN     12          /* HMAC-SHA1 truncated to 96 bit */
#define MAX_USERS        16
#define PASSWD_FILE      "/config/ipmi-lan.passwd"
#define LOG_FILE         "/var/log/ipmi-lan.log"

#define PT_IPMI          0x00
#define PT_OPEN_REQ      0x10
#define PT_OPEN_RSP      0x11
#define PT_RAKP1         0x12
#define PT_RAKP2         0x13
#define PT_RAKP3         0x14
#define PT_RAKP4         0x15

#define NETFN_CHASSIS    0x00
#define NETFN_APP        0x06

#define CC_OK            0x00
#define CC_INVALID_CMD   0xC1
#define CC_INVALID_DATA  0xCC

/* RMCP+ open-session status codes we use */
#define RMCP_OK                0x00
#define RMCP_INSUFF_PRIV       0x0C
#define RMCP_INVALID_USER      0x0D

typedef struct {
    char name[17];
    char pass[21];
} ipmi_user_t;

typedef struct {
    int      in_use;
    uint32_t rc_id;
    uint32_t rm_id;
    uint8_t  rc_rand[16];
    uint8_t  rm_rand[16];
    uint8_t  sik[20];
    uint8_t  k1[20];
    uint8_t  k2[20];
    uint8_t  priv;
    uint8_t  role_byte;      /* RAKP1 role byte as received (0x10 = name-only) */
    char     user[17];
    uint32_t out_seq;
    time_t   last;
} session_t;

static session_t   g_sessions[MAX_SESSIONS];
static ipmi_user_t g_users[MAX_USERS];
static int         g_nusers;
static uint32_t    g_next_id = 0x5a170000;

/* 16-byte system GUID advertised during RAKP */
static const uint8_t g_guid[16] = {
    'A','T','T','-','V','1','5','0','-','u','B','M','C','0','0','1'
};

/* ------------------------------------------------------------------ utils */

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ipmi-lan: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int debug_on(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("IPMI_LAN_DEBUG") != NULL;
    return v;
}

static void hexdump(const char *tag, const uint8_t *p, int n)
{
    int i;
    if (!debug_on())
        return;
    fprintf(stderr, "ipmi-lan: %s (%d):", tag, n);
    for (i = 0; i < n && i < 48; i++)
        fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, "\n");
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static uint8_t ipmi_csum(const uint8_t *p, int n)
{
    uint8_t s = 0;
    int i;
    for (i = 0; i < n; i++)
        s = (uint8_t)(s + p[i]);
    return (uint8_t)(-s);
}

static void rand_bytes(uint8_t *p, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)(rand() & 0xff);
}

/* ----------------------------------------------------------------- crypto */

static void hmac_sha1(const uint8_t *key, int keylen,
                      const uint8_t *data, int datalen, uint8_t out[20])
{
    unsigned int len = 20;
    HMAC(EVP_sha1(), key, keylen, data, datalen, out, &len);
}

static int aes_cbc(int enc, const uint8_t key[16], const uint8_t iv[16],
                   const uint8_t *in, int inlen, uint8_t *out)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, fl = 0, rc;

    if (!ctx)
        return -1;
    rc = enc ? EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv)
             : EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv);
    if (rc != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    if (enc) {
        EVP_EncryptUpdate(ctx, out, &n, in, inlen);
        EVP_EncryptFinal_ex(ctx, out + n, &fl);
    } else {
        EVP_DecryptUpdate(ctx, out, &n, in, inlen);
        EVP_DecryptFinal_ex(ctx, out + n, &fl);
    }
    EVP_CIPHER_CTX_free(ctx);
    return n + fl;
}

/*
 * Kg = password, then
 *   sik = HMAC_SHA1(Kg, Rc || Rm || role || userlen || user)
 *   k1  = HMAC_SHA1(sik, 0x01 x 20)
 *   k2  = HMAC_SHA1(sik, 0x02 x 20)      (AES-128 key = k2[0:16])
 */
static void derive_keys(session_t *s, const char *pass, uint8_t role)
{
    uint8_t buf[128], one[20], two[20];
    int n = 0, ul = (int)strlen(s->user), i;

    memcpy(buf + n, s->rc_rand, 16); n += 16;
    memcpy(buf + n, s->rm_rand, 16); n += 16;
    buf[n++] = role;
    buf[n++] = (uint8_t)ul;
    memcpy(buf + n, s->user, ul); n += ul;

    hmac_sha1((const uint8_t *)pass, (int)strlen(pass), buf, n, s->sik);

    memset(one, 0x01, sizeof(one));
    memset(two, 0x02, sizeof(two));
    hmac_sha1(s->sik, 20, one, 20, s->k1);
    hmac_sha1(s->sik, 20, two, 20, s->k2);
    (void)i;
}

/* ------------------------------------------------------------ user store */

static void trim(char *s)
{
    char *p;
    while (*s && isspace((unsigned char)*s))
        memmove(s, s + 1, strlen(s));
    p = s + strlen(s);
    while (p > s && isspace((unsigned char)p[-1]))
        *--p = '\0';
}

static void write_default_passwd(void)
{
    FILE *f = fopen(PASSWD_FILE, "w");
    if (!f) {
        logmsg("cannot create %s: %s", PASSWD_FILE, strerror(errno));
        return;
    }
    fprintf(f, "# IPMI over LAN credentials, one user:password per line.\n");
    fprintf(f, "# RMCP+ derives its session keys from the plaintext password,\n");
    fprintf(f, "# so these cannot come from /etc/shadow.  Keep this file 0600.\n");
    fprintf(f, "root:51L1C0M201B\n");
    fprintf(f, "is_admin:51L1C0M201B\n");
    fclose(f);
    chmod(PASSWD_FILE, 0600);
    logmsg("created default %s", PASSWD_FILE);
}

static void load_users(void)
{
    FILE *f;
    char line[128];

    g_nusers = 0;
    f = fopen(PASSWD_FILE, "r");
    if (!f) {
        write_default_passwd();
        f = fopen(PASSWD_FILE, "r");
        if (!f)
            return;
    }
    chmod(PASSWD_FILE, 0600);

    while (fgets(line, sizeof(line), f) && g_nusers < MAX_USERS) {
        char *colon;
        trim(line);
        if (line[0] == '#' || line[0] == '\0')
            continue;
        colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        trim(line);
        trim(colon + 1);
        if (strlen(line) == 0 || strlen(line) > 16)
            continue;
        if (strlen(colon + 1) == 0 || strlen(colon + 1) > 20)
            continue;
        snprintf(g_users[g_nusers].name, sizeof(g_users[0].name), "%s", line);
        snprintf(g_users[g_nusers].pass, sizeof(g_users[0].pass), "%s", colon + 1);
        g_nusers++;
    }
    fclose(f);
    logmsg("loaded %d ipmi user(s) from %s", g_nusers, PASSWD_FILE);
}

static const char *user_password(const char *name)
{
    int i;
    for (i = 0; i < g_nusers; i++)
        if (strcmp(g_users[i].name, name) == 0)
            return g_users[i].pass;
    return NULL;
}

/* ------------------------------------------------------------ power glue */

static void run_sys_ctrl(const char *arg)
{
    char cmd[160];
    snprintf(cmd, sizeof(cmd),
             "/usr/sbin/ubmc_sys_ctrl.sh %s >/dev/null 2>&1", arg);
    if (system(cmd) != 0)
        logmsg("ubmc_sys_ctrl.sh %s failed", arg);
    else
        logmsg("ubmc_sys_ctrl.sh %s ok", arg);
}

static int host_power_is_on(void)
{
    FILE *f;
    char buf[128] = {0};
    int on;

    f = popen("/usr/sbin/ubmc_sys_ctrl.sh -q 2>/dev/null", "r");
    if (!f)
        return 0;
    if (!fgets(buf, sizeof(buf), f))
        buf[0] = '\0';
    pclose(f);
    on = strstr(buf, "powered up") != NULL;
    return on;
}

/* --------------------------------------------------------- IPMI dispatch */

static int dispatch_ipmi(const uint8_t *req, int reqlen,
                         uint8_t *rsp, int *rsplen, session_t *s)
{
    uint8_t netfn, cmd, rqaddr, rsaddr, rqseq;
    int n = 0;

    if (reqlen < 7)
        return -1;
    if (ipmi_csum(req, 3) != 0)          /* rsAddr + netFn + checksum */
        return -1;
    if (ipmi_csum(req + 3, reqlen - 3) != 0)    /* rest, incl. trailing checksum */
        return -1;

    rsaddr = req[0];
    netfn  = (uint8_t)(req[1] >> 2);
    rqaddr = req[3];
    rqseq  = (uint8_t)(req[4] >> 2);
    cmd    = req[5];

    hexdump("req", req, reqlen);

    rsp[n++] = rqaddr;
    rsp[n++] = (uint8_t)((netfn | 1) << 2);
    rsp[n++] = 0;
    rsp[n++] = rsaddr;
    rsp[n++] = (uint8_t)(rqseq << 2);
    rsp[n++] = cmd;
    rsp[n++] = CC_OK;                    /* completion code, first data byte */

    switch (netfn) {
    case NETFN_APP:
        switch (cmd) {
        case 0x01:                                  /* Get Device ID */
            rsp[n++] = 0x20;
            rsp[n++] = 0x81;                        /* provides device SDRs */
            rsp[n++] = 0x02;                        /* fw major */
            rsp[n++] = 0x10;                        /* fw minor */
            rsp[n++] = 0x02;                        /* IPMI 2.0 */
            rsp[n++] = 0x0F;                        /* sensor/SEL/FRU */
            rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
            rsp[n++] = 0x01; rsp[n++] = 0x00;
            rsp[n++] = 0x00; rsp[n++] = 0x00;
            rsp[n++] = 0x00; rsp[n++] = 0x00;
            break;

        case 0x38: {                                /* Get Channel Auth Caps */
            uint8_t ch = (reqlen - 7) > 0 ? req[6] : 0;
            rsp[n++] = (uint8_t)(ch & 0x0f);
            /* Bit 7 is reserved by the spec but pyghmi only treats the BMC as
             * IPMI 2.0 capable when it is set (see _got_channel_auth_cap). */
            rsp[n++] = 0x91;      /* bit7 + none + straight password */
            rsp[n++] = 0x04;      /* non-null user names enabled */
            rsp[n++] = 0x03;      /* IPMI 2.0 + RMCP+ */
            rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            break;
        }

        case 0x3B:                                  /* Set Session Priv Level */
            if ((reqlen - 7) < 1 || req[6] > 4) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (s)
                s->priv = req[6];
            rsp[n++] = 4;
            break;

        case 0x3D:                                  /* Get Session Info */
            rsp[n++] = 0x00;
            rsp[n++] = (s ? s->priv : 4);
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            break;

        default:
            rsp[6] = CC_INVALID_CMD;
            break;
        }
        break;

    case NETFN_CHASSIS:
        switch (cmd) {
        case 0x00:                                  /* Get Chassis Caps */
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            break;

        case 0x01: {                                /* Get Chassis Status */
            int on = host_power_is_on();
            rsp[n++] = on ? 0x01 : 0x00;            /* bit0 current power */
            rsp[n++] = on ? 0x40 : 0x01;            /* last power event */
            rsp[n++] = 0x00;                        /* misc chassis state */
            rsp[n++] = 0x00;                        /* front panel */
            break;
        }

        case 0x02: {                                /* Chassis Control */
            uint8_t op;
            if ((reqlen - 7) < 1) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            op = req[6];
            switch (op) {
            case 0: run_sys_ctrl("-f"); break;      /* power down */
            case 1: run_sys_ctrl("-o"); break;      /* power up */
            case 2: run_sys_ctrl("-c"); break;      /* power cycle */
            case 3: run_sys_ctrl("-r"); break;      /* hard reset */
            case 4: run_sys_ctrl("-s"); break;      /* diagnostic interrupt */
            case 5: run_sys_ctrl("-s"); break;      /* soft shutdown */
            default:
                rsp[6] = CC_INVALID_DATA;
                goto out;
            }
            break;
        }

        case 0x04:                                  /* Chassis Identify */
            break;

        default:
            rsp[6] = CC_INVALID_CMD;
            break;
        }
        break;

    default:
        rsp[6] = CC_INVALID_CMD;
        break;
    }

out:
    rsp[2] = ipmi_csum(rsp, 2);
    rsp[n] = ipmi_csum(rsp + 3, n - 3);
    n++;
    *rsplen = n;
    return 0;
}

/* --------------------------------------------------------------- framing */

static uint8_t *rmcp_plus_hdr(uint8_t *p, uint8_t ptype,
                              uint32_t sid, uint32_t seq, int payload_len)
{
    *p++ = 0x06; *p++ = 0x00; *p++ = 0xff; *p++ = 0x07;
    *p++ = 0x06;
    *p++ = ptype;
    wr_le32(p, sid); p += 4;
    wr_le32(p, seq); p += 4;
    *p++ = (uint8_t)(payload_len & 0xff);
    *p++ = (uint8_t)((payload_len >> 8) & 0xff);
    return p;
}

static void send_plain(int fd, const struct sockaddr_in *to, uint8_t ptype,
                       uint32_t sid, uint32_t seq, const uint8_t *pl, int plen)
{
    uint8_t out[MAX_PKT];
    uint8_t *p = rmcp_plus_hdr(out, ptype, sid, seq, plen);
    memcpy(p, pl, plen);
    p += plen;
    sendto(fd, out, (size_t)(p - out), 0,
           (const struct sockaddr *)to, sizeof(*to));
}

/* IPMI 1.5 unauthenticated frame, used for Get Channel Auth Capabilities */
static void handle_legacy(int fd, const struct sockaddr_in *to,
                          const uint8_t *req, int len)
{
    uint8_t out[MAX_PKT], rsp[MAX_PKT];
    int n = 0, rlen = 0, plen;

    if (len < 15 || req[4] != 0x00) {
        logmsg("legacy: short/unsupported auth type %02x (len %d)", req[4], len);
        return;
    }
    plen = req[13];
    if (14 + plen > len) {
        logmsg("legacy: truncated payload (%d > %d)", 14 + plen, len);
        return;
    }
    if (dispatch_ipmi(req + 14, plen, rsp, &rlen, NULL) < 0) {
        logmsg("legacy: dispatch rejected (netfn %02x cmd %02x)",
               plen > 1 ? (req[15] >> 2) : 0xff, plen > 5 ? req[19] : 0xff);
        return;
    }

    out[n++] = 0x06; out[n++] = 0x00;
    out[n++] = 0xff; out[n++] = 0x07;
    out[n++] = 0x00;
    memcpy(out + n, req + 5, 4); n += 4;            /* echo sequence */
    memset(out + n, 0, 4); n += 4;
    out[n++] = (uint8_t)rlen;
    memcpy(out + n, rsp, rlen); n += rlen;
    sendto(fd, out, (size_t)n, 0, (const struct sockaddr *)to, sizeof(*to));
}

/* -------------------------------------------------------------- sessions */

static session_t *by_rm_id(uint32_t id)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].in_use && g_sessions[i].rm_id == id)
            return &g_sessions[i];
    return NULL;
}

static session_t *by_rc_id(uint32_t id)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++)
        if (g_sessions[i].in_use && g_sessions[i].rc_id == id)
            return &g_sessions[i];
    return NULL;
}

static session_t *alloc_session(void)
{
    int i, oldest = 0;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use)
            return &g_sessions[i];
        if (g_sessions[i].last < g_sessions[oldest].last)
            oldest = i;
    }
    memset(&g_sessions[oldest], 0, sizeof(g_sessions[oldest]));
    return &g_sessions[oldest];
}

static void drop_session(session_t *s)
{
    if (s)
        memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------ RMCP+ in */

static void handle_open_request(int fd, const struct sockaddr_in *to,
                                const uint8_t *pl, int plen)
{
    uint8_t   rsp[64];
    int       n = 0;
    uint8_t   status = RMCP_OK;
    uint8_t   auth_algo = 1;                        /* HMAC-SHA1 */
    session_t *s;

    if (plen < 16)
        return;

    /* Algorithm payloads start at offset 8: type(1) rsvd(2) len(2) algo(1) ... */
    auth_algo = pl[12];

    if (auth_algo != 1) {
        /* Only SHA-1 is offered.  Answering with an error makes clients that
         * proposed SHA-256 (pyghmi does) fall back and retry with SHA-1. */
        status = RMCP_INSUFF_PRIV;
        logmsg("open request rejected: auth algo %u, suggesting SHA-1", auth_algo);
    }

    rsp[n++] = pl[0];
    rsp[n++] = status;
    rsp[n++] = 0x04;                                /* max privilege: admin */
    rsp[n++] = 0x00;
    wr_le32(rsp + n, rd_le32(pl + 4)); n += 4;      /* echo RcID */
    if (status != RMCP_OK) {
        memset(rsp + n, 0, 4); n += 4;
        send_plain(fd, to, PT_OPEN_RSP, 0, 0, rsp, n);
        return;
    }

    s = by_rc_id(rd_le32(pl + 4));
    if (s)
        drop_session(s);
    s = alloc_session();
    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    s->rc_id  = rd_le32(pl + 4);
    s->rm_id  = ++g_next_id;
    s->priv   = 4;
    s->last   = time(NULL);

    wr_le32(rsp + n, s->rm_id); n += 4;
    /* auth / integrity / confidentiality blocks, suite 3 */
    rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x08;
    rsp[n++] = 0x01; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
    rsp[n++] = 0x01; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x08;
    rsp[n++] = 0x01; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
    rsp[n++] = 0x02; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x08;
    rsp[n++] = 0x01; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;

    logmsg("open session rc=%08x rm=%08x", s->rc_id, s->rm_id);
    send_plain(fd, to, PT_OPEN_RSP, 0, 0, rsp, n);
}

static void rakp_error(int fd, const struct sockaddr_in *to, uint8_t tag,
                       uint8_t code, uint32_t rc_id, uint32_t rm_id)
{
    uint8_t rsp[24];
    int n = 0;
    rsp[n++] = tag;
    rsp[n++] = code;
    rsp[n++] = 0x00;
    rsp[n++] = 0x00;
    wr_le32(rsp + n, rc_id); n += 4;
    memset(rsp + n, 0, 12); n += 12;
    send_plain(fd, to, PT_RAKP4, rm_id, 0, rsp, n);
}

static void handle_rakp1(int fd, const struct sockaddr_in *to,
                         const uint8_t *pl, int plen)
{
    uint8_t   rsp[MAX_PKT], mac[20], buf[128];
    int       n = 0, blen, userlen;
    uint32_t  rm_id;
    uint8_t   role;
    char      user[17];
    const char *pass;
    session_t *s;

    if (plen < 28)
        return;
    rm_id = rd_le32(pl + 4);
    s = by_rm_id(rm_id);
    if (!s)
        return;

    memcpy(s->rc_rand, pl + 8, 16);
    s->role_byte = pl[24];              /* keep 0x10 (name-only) for the MACs */
    role    = (uint8_t)(pl[24] & 0x0f);
    userlen = pl[27];
    if (userlen > 16 || 28 + userlen > plen)
        return;
    memcpy(user, pl + 28, userlen);
    user[userlen] = '\0';

    pass = user_password(user);
    if (!pass) {
        logmsg("rakp1: unknown user '%s'", user);
        rakp_error(fd, to, pl[0], RMCP_INVALID_USER, s->rc_id, s->rm_id);
        drop_session(s);
        return;
    }

    snprintf(s->user, sizeof(s->user), "%s", user);
    s->priv = role;
    rand_bytes(s->rm_rand, 16);
    derive_keys(s, pass, s->role_byte);

    rsp[n++] = pl[0];
    rsp[n++] = RMCP_OK;
    rsp[n++] = 0x04;
    rsp[n++] = 0x00;
    wr_le32(rsp + n, s->rc_id); n += 4;
    memcpy(rsp + n, s->rm_rand, 16); n += 16;
    memcpy(rsp + n, g_guid, 16); n += 16;

    /* RAKP2 authcode = HMAC-SHA1(password,
     *   RcID || RmID || Rc || Rm || GUID || role || userlen || user) */
    wr_le32(buf + 0,  s->rc_id);
    wr_le32(buf + 4,  s->rm_id);
    memcpy(buf + 8,  s->rc_rand, 16);
    memcpy(buf + 24, s->rm_rand, 16);
    memcpy(buf + 40, g_guid, 16);
    buf[56] = s->role_byte;
    buf[57] = (uint8_t)userlen;
    memcpy(buf + 58, user, userlen);
    blen = 58 + userlen;
    hmac_sha1((const uint8_t *)pass, (int)strlen(pass), buf, blen, mac);
    memcpy(rsp + n, mac, 20); n += 20;

    logmsg("rakp1 user=%s role=%u", user, role);
    send_plain(fd, to, PT_RAKP2, s->rm_id, 0, rsp, n);
}

static void handle_rakp3(int fd, const struct sockaddr_in *to,
                         const uint8_t *pl, int plen)
{
    uint8_t   rsp[64], buf[128], expect[20], b2[40];
    int       n = 0, blen, ul;
    uint32_t  rm_id;
    const char *pass;
    session_t *s;

    if (plen < 28)
        return;
    rm_id = rd_le32(pl + 4);
    s = by_rm_id(rm_id);
    if (!s)
        return;

    pass = user_password(s->user);
    if (!pass) {
        rakp_error(fd, to, pl[0], RMCP_INVALID_USER, s->rc_id, s->rm_id);
        drop_session(s);
        return;
    }

    /* RAKP3 authcode = HMAC-SHA1(password,
     *   Rm || RcID || role || userlen || user) */
    ul = (int)strlen(s->user);
    blen = 0;
    memcpy(buf + blen, s->rm_rand, 16); blen += 16;
    wr_le32(buf + blen, s->rc_id);      blen += 4;
    buf[blen++] = s->role_byte;
    buf[blen++] = (uint8_t)ul;
    memcpy(buf + blen, s->user, ul);    blen += ul;
    hmac_sha1((const uint8_t *)pass, (int)strlen(pass), buf, blen, expect);

    if (memcmp(expect, pl + 8, 20) != 0) {
        logmsg("rakp3: bad password for '%s'", s->user);
        rakp_error(fd, to, pl[0], 0x01, s->rc_id, s->rm_id);  /* invalid session */
        drop_session(s);
        return;
    }

    /* RAKP4 authcode = HMAC-SHA1(sik, Rc || RmID || GUID)[:12] */
    rsp[n++] = pl[0];
    rsp[n++] = RMCP_OK;
    rsp[n++] = 0x00;
    rsp[n++] = 0x00;
    wr_le32(rsp + n, s->rc_id); n += 4;
    memcpy(b2 + 0, s->rc_rand, 16);
    wr_le32(b2 + 16, s->rm_id);
    memcpy(b2 + 20, g_guid, 16);
    hmac_sha1(s->sik, 20, b2, 36, expect);
    memcpy(rsp + n, expect, AUTHCODE_LEN); n += AUTHCODE_LEN;

    logmsg("session established: user=%s rm=%08x priv=%u",
           s->user, s->rm_id, s->priv);
    send_plain(fd, to, PT_RAKP4, s->rm_id, 0, rsp, n);
}

static void handle_encapsulated(int fd, const struct sockaddr_in *to,
                                const uint8_t *pkt, int len)
{
    uint8_t   rspmsg[MAX_PKT], plain[MAX_PKT], enc[MAX_PKT], cipher[MAX_PKT];
    uint8_t   out[MAX_PKT], iv[16], mac[20], *o;
    int       plen, i, flen, padlen, msglen, rlen = 0, pad, n;
    uint32_t  sid;
    session_t *s;

    if (len < 16)
        return;
    sid  = rd_le32(pkt + 6);
    plen = pkt[14] | (pkt[15] << 8);

    s = by_rm_id(sid);
    if (!s)
        return;
    if (16 + plen > len || plen < 17)
        return;

    /* integrity: HMAC-SHA1-96 over everything after the RMCP header */
    if (len - 12 - 4 > 0) {
        hmac_sha1(s->k1, 20, pkt + 4, len - 12 - 4, mac);
        if (memcmp(mac, pkt + len - 12, AUTHCODE_LEN) != 0) {
            logmsg("integrity check failed, dropping packet");
            return;
        }
    }

    /* confidentiality: 16-byte IV then AES-CBC-128 */
    memcpy(iv, pkt + 16, 16);
    {
        int clen = plen - 16;
        if (clen <= 0 || clen > (int)sizeof(plain) || (clen % 16) != 0)
            return;
        flen = aes_cbc(0, s->k2, iv, pkt + 32, clen, plain);
        if (flen != clen) {
            logmsg("decrypt failed");
            return;
        }
    }
    padlen = plain[plen - 17];
    msglen = plen - 17 - padlen;
    if (msglen <= 0)
        return;

    if (dispatch_ipmi(plain, msglen, rspmsg, &rlen, s) < 0) {
        logmsg("bad ipmi message");
        return;
    }

    /* encrypt the reply */
    pad = (rlen + 1) % 16;
    if (pad)
        pad = 16 - pad;
    memcpy(enc, rspmsg, rlen);
    for (i = 0; i < pad; i++)
        enc[rlen + i] = 0xff;
    enc[rlen + pad] = (uint8_t)pad;
    {
        int enclen = rlen + pad + 1;
        rand_bytes(iv, 16);
        aes_cbc(1, s->k2, iv, enc, enclen, cipher);

        o = rmcp_plus_hdr(out, PT_IPMI | 0x40 | 0x80, s->rc_id,
                          ++s->out_seq, 16 + enclen);
        memcpy(o, iv, 16); o += 16;
        memcpy(o, cipher, enclen); o += enclen;

        n = (int)(o - out) - 2;                     /* exclude length field */
        pad = n % 4;
        if (pad)
            pad = 4 - pad;
        for (i = 0; i < pad; i++)
            *o++ = 0xff;
        *o++ = (uint8_t)pad;
        *o++ = 0x07;
        hmac_sha1(s->k1, 20, out + 4, (int)(o - out) - 4, mac);
        memcpy(o, mac, AUTHCODE_LEN); o += AUTHCODE_LEN;
        sendto(fd, out, (size_t)(o - out), 0,
               (const struct sockaddr *)to, sizeof(*to));
    }
    s->last = time(NULL);
}

static void handle_rmcp_plus(int fd, const struct sockaddr_in *to,
                             const uint8_t *pkt, int len)
{
    uint8_t ptype;

    if (len < 16)
        return;
    ptype = (uint8_t)(pkt[5] & 0x3f);
    hexdump("rmcp+", pkt, len);

    switch (ptype) {
    case PT_OPEN_REQ: handle_open_request(fd, to, pkt + 16, len - 16); break;
    case PT_RAKP1:    handle_rakp1(fd, to, pkt + 16, len - 16);        break;
    case PT_RAKP3:    handle_rakp3(fd, to, pkt + 16, len - 16);        break;
    case PT_IPMI:     handle_encapsulated(fd, to, pkt, len);           break;
    default:
        logmsg("unhandled payload type 0x%02x", ptype);
        break;
    }
}

/* ------------------------------------------------------------------ main */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/*
 * -d detaches properly: fork/setsid/close fds and reattach stderr to the log.
 * Without it, a caller that owns a pipe (the uhttpd CGI behind the web RPC)
 * would block forever waiting for EOF, because a plain `daemon &` inherits the
 * pipe's write end.
 */
static void daemonize(void)
{
    pid_t pid = fork();
    FILE *log;

    if (pid < 0) {
        logmsg("fork: %s", strerror(errno));
        exit(1);
    }
    if (pid > 0)
        exit(0);

    setsid();
    if (chdir("/") != 0) {
        /* not fatal */
    }
    if (freopen("/dev/null", "r", stdin) == NULL) {
        /* not fatal */
    }
    log = freopen(LOG_FILE, "a", stderr);
    if (log == NULL) {
        if (freopen("/dev/null", "w", stderr) == NULL) {
            /* not fatal */
        }
    }
    dup2(fileno(stderr), fileno(stdout));
}

/*
 * A packet that arrives on an interface enslaved to a VRF is only delivered
 * to listeners bound to that VRF, because is_mgmtd leaves
 * net.ipv4.udp_l3mdev_accept at 0.  "ip vrf exec" cannot give us that binding
 * (it needs a BPF cgroup hook the appliance kernel does not always provide),
 * so bind the socket to the device directly.
 */
static int open_listener(const char *devname)
{
    int fd, one = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        logmsg("socket: %s", strerror(errno));
        return -1;
    }
    if (getenv("IPMI_LAN_REUSE")) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        logmsg("SO_REUSEADDR on %s", *devname ? devname : "any");
    }

    if (*devname &&
        setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, devname,
                   (socklen_t)strlen(devname) + 1) < 0) {
        logmsg("SO_BINDTODEVICE(%s): %s", devname, strerror(errno));
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(IPMI_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logmsg("bind udp/%d on %s: %s", IPMI_PORT,
               *devname ? devname : "any", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    int fds[MAX_SOCKS], nfds = 0, maxfd = -1, i;
    const char *bindlist;
    char listbuf[256];
    char *p;

    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        printf("usage: %s [-d] [-h]\n", argv[0]);
        printf("  -d                              detach and log to %s\n", LOG_FILE);
        printf("  listens on udp/%d, users from %s\n", IPMI_PORT, PASSWD_FILE);
        printf("  IPMI_LAN_BIND=<dev>[,<dev>...]  interfaces to bind\n");
        printf("      empty (the default) = one wildcard listener on all ports\n");
        printf("      an empty list element adds a wildcard socket to a mixed list\n");
        printf("  IPMI_LAN_REUSE=1                set SO_REUSEADDR (breaks VRF rx)\n");
        printf("  IPMI_LAN_DEBUG=1                hexdump every request\n");
        return 0;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)(time(NULL) ^ getpid()));

    if (argc > 1 && strcmp(argv[1], "-d") == 0)
        daemonize();

    load_users();
    if (g_nusers == 0)
        logmsg("warning: no users loaded, every login will fail");

    bindlist = getenv("IPMI_LAN_BIND");
    if (!bindlist)
        bindlist = getenv("IPMI_LAN_VRF");   /* older name, kept for scripts */
    if (!bindlist)
        bindlist = "";                       /* plain wildcard listener */
    if (strlen(bindlist) >= sizeof(listbuf)) {
        logmsg("IPMI_LAN_BIND is too long");
        return 1;
    }
    strcpy(listbuf, bindlist);

    for (p = listbuf; p; ) {
        char *comma = strchr(p, ',');
        int fd;
        if (comma)
            *comma = 0;
        fd = open_listener(p);
        if (fd >= 0 && nfds < MAX_SOCKS) {
            fds[nfds++] = fd;
            if (fd > maxfd)
                maxfd = fd;
            logmsg("listening on udp/%d dev=%s", IPMI_PORT, *p ? p : "any");
        }
        p = comma ? comma + 1 : NULL;
    }

    if (nfds == 0) {
        logmsg("no listener could be created");
        return 1;
    }

    while (!g_stop) {
        fd_set rfds;
        struct timeval tv;
        int r;

        FD_ZERO(&rfds);
        for (i = 0; i < nfds; i++)
            FD_SET(fds[i], &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0)
            continue;

        for (i = 0; i < nfds; i++) {
            uint8_t buf[MAX_PKT];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n;

            if (!FD_ISSET(fds[i], &rfds))
                continue;
            n = (int)recvfrom(fds[i], buf, sizeof(buf), 0,
                              (struct sockaddr *)&from, &fl);
            if (debug_on()) {
                char sa[32];
                inet_ntop(AF_INET, &from.sin_addr, sa, sizeof(sa));
                logmsg("rx %d bytes from %s:%u on %s", n, sa,
                       (unsigned)ntohs(from.sin_port), i == 0 ? "sock0" : "sockN");
            }
            if (n <= 3)
                continue;
            if (buf[0] != 0x06 || buf[3] != 0x07)
                continue;                        /* not an IPMI class packet */
            if (buf[4] == 0x06)
                handle_rmcp_plus(fds[i], &from, buf, n);
            else
                handle_legacy(fds[i], &from, buf, n);
        }
    }

    for (i = 0; i < nfds; i++)
        close(fds[i]);
    logmsg("stopping");
    return 0;
}
