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
 * Cipher suites 3 (HMAC-SHA1 / SHA1-96 / AES-CBC-128) and 17/18/19
 * (HMAC-SHA256 / SHA256-128) are implemented -- those cover ipmitool's lanplus
 * default and the suites real BMCs advertise.
 *
 * Credentials: RMCP+ derives its keys from the *plaintext* password
 * (RAKP2/RAKP3 are HMACs keyed with it), so /etc/shadow is useless here -- a
 * one-way hash cannot produce those MACs.  Real BMCs therefore keep a separate
 * IPMI user list.  Ours lives in /config/ipmi-lan.passwd (persistent ext4
 * partition), one "id:name:password:max-privilege:enabled" per line.
 *
 * Build: scripts/ipmi-lan/build-cross.sh (static, from WSL).  The root
 * filesystem is a ramdisk, so the daemon must not depend on the image's
 * libraries; see ipmi-lan.md.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define IPMI_PORT        623
#define MAX_SESSIONS     16
#define MAX_PKT          1024
#define MAX_SOCKS        8
#define AUTHCODE_LEN     12          /* HMAC-SHA1 truncated to 96 bit */
#define MAX_DIGEST       32          /* HMAC-SHA256 output */
#define MAX_ICV          16          /* HMAC-SHA256-128 */

/* RAKP authentication algorithms, as sent in the Open Session Request */
#define AUTH_ALGO_SHA1      1
#define AUTH_ALGO_SHA256    3
/* integrity algorithms */
#define INTEG_ALGO_SHA1_96      1
#define INTEG_ALGO_SHA256_128   4
/* confidentiality algorithms */
#define CONF_ALGO_AES_CBC_128   1
#define MAX_USERS        16
#define PASSWD_FILE      "/config/ipmi-lan.passwd"
#define LOG_FILE         "/var/log/ipmi-lan.log"
/* the vendor management daemon keeps its network configuration here, one
 * "<path>,<value>" per line; interface-list/<if>/ipv4-origin is authoritative */
#define MGMTD_CONF       "/config/is_mgmtd.conf"

/* SOL */
#define CONSOLE_SOCK    "/var/run/console_manager_socket"
#define PT_SOL          0x01
#define SOL_MAX_OUT     128         /* max characters we put in one packet */
#define SOL_WINDOW      4           /* unacknowledged packets we keep around */
#define SOL_RETRY_MS    1000
#define SOL_MAX_TRIES   3
#define HOST_UART       "/dev/ttyS1"
#define SOL_BREAK_MS    250         /* long enough to be seen as a framing error,
                                     * short enough that the host's SysRq does
                                     * not pick up stray characters */

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
/* RMCP+ open-session status codes we use.  0x04 and 0x06 are in the "algorithm
 * rejected" set that clients treat as "try the next cipher suite". */
#define RMCP_INVALID_AUTH_ALGO 0x04
#define RMCP_INVALID_CONF_ALGO 0x06
#define RMCP_INVALID_USER      0x0D

typedef struct {
    uint8_t id;              /* IPMI user ID, 1..MAX_USERS */
    char    name[17];
    char    pass[21];
    uint8_t priv;            /* max privilege: 1 callback .. 4 admin, 5 OEM */
    uint8_t enabled;
} ipmi_user_t;

typedef struct {
    uint8_t  data[SOL_MAX_OUT + 4];
    int      len;            /* 0 = free slot */
    uint8_t  seq;
    uint8_t  tries;
} sol_pkt_t;

typedef struct {
    int      in_use;
    uint32_t rc_id;
    uint32_t rm_id;
    uint8_t  rc_rand[16];
    uint8_t  rm_rand[16];
    /* SIK / K1 / K2 are as long as the RAKP digest, so they have to hold the
     * 32 bytes of HMAC-SHA256 as well as the 20 of HMAC-SHA1 */
    uint8_t  sik[MAX_DIGEST];
    uint8_t  k1[MAX_DIGEST];
    uint8_t  k2[MAX_DIGEST];
    uint8_t  auth_algo;      /* negotiated RAKP hash: 1 = SHA-1, 3 = SHA-256 */
    uint8_t  integ_algo;     /* 1 = HMAC-SHA1-96, 4 = HMAC-SHA256-128 */
    int      dig_len;        /* 20 or 32 */
    int      icv_len;        /* 12 or 16 */
    /* K1/K2 depend on how the client reads the spec's "0x01 repeated" constant:
     * ipmitool and pyghmi use twenty 0x01 bytes, python-ipmi uses as many as
     * the digest is long.  Both are computed and the one that validates the
     * client's first packet is latched in key_variant (-1 = not decided). */
    uint8_t  k1_alt[MAX_DIGEST];
    uint8_t  k2_alt[MAX_DIGEST];
    int      key_variant;
    uint8_t  priv;
    uint8_t  role_byte;      /* RAKP1 role byte as received (0x10 = name-only) */
    char     user[17];
    uint32_t out_seq;
    time_t   last;
    struct sockaddr_in peer; /* last source address, needed for async SOL */
    int      have_peer;
    int      close_pending;  /* Close Session seen: drop after the reply */
    /* SOL: the console channel is a fan-out socket served by is_console_manager,
     * so SOL just becomes one more client and does not disturb ttyd */
    int      sol_cfd;        /* connected console socket, -1 when idle */
    uint8_t  sol_tx_seq;     /* our outgoing packet sequence, 1..15 */
    uint8_t  sol_ack_seq;    /* last client sequence we acknowledged */
    uint8_t  sol_rx_seq;     /* last client sequence we accepted */
    int      sol_outlen;
    uint8_t  sol_outbuf[4096];
    /* sliding window, so a client that stops acknowledging cannot be flooded */
    sol_pkt_t sol_inflight[SOL_WINDOW];
    int       sol_ninflight;
    long      sol_retry_ms;  /* when to retransmit sol_inflight[0] */
    int       sol_acks_seen; /* client acknowledgements received so far */
    int       sol_noack;     /* client never acks: stream without a window */
} session_t;

static session_t   g_sessions[MAX_SESSIONS];
static ipmi_user_t g_users[MAX_USERS];

/* ------------------------------------------------------------ LAN params */
/*
 * The LAN parameters that are pure configuration (SNMP community, auth type
 * enables, ARP control) live in one small file.  The community string is not
 * cosmetic: it is the community used in the PET traps this daemon sends.
 */
#define LANPARAM_CONF "/config/ipmi-lan-params.conf"

typedef struct {
    char    community[18];
    uint8_t auth_types;         /* param 1: supported auth types */
    uint8_t auth_enables[5];    /* param 2: per privilege level */
    uint8_t arp_control;        /* param 10 */
    uint8_t garp_interval;      /* param 11: gratuitous ARP interval, 500ms units */
} lan_params_t;

static lan_params_t g_lan = {
    "public", 0x15, { 0x15, 0x15, 0x15, 0x15, 0x15 }, 0x02, 0x00
};
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

/* The RAKP hash and the integrity hash are chosen per session: cipher suite 3
 * uses HMAC-SHA1, suites 17/18/19 use HMAC-SHA256. */
static const EVP_MD *auth_md(uint8_t algo)
{
    return algo == AUTH_ALGO_SHA256 ? EVP_sha256() : EVP_sha1();
}

static const EVP_MD *integ_md(uint8_t algo)
{
    return algo == INTEG_ALGO_SHA256_128 ? EVP_sha256() : EVP_sha1();
}

static int dig_len_of(uint8_t algo)
{
    return algo == AUTH_ALGO_SHA256 ? 32 : 20;
}

static int icv_len_of(uint8_t integ)
{
    return integ == INTEG_ALGO_SHA256_128 ? 16 : AUTHCODE_LEN;
}

static int hmac_md(const EVP_MD *md, const uint8_t *key, int keylen,
                   const uint8_t *data, int datalen, uint8_t *out)
{
    unsigned int len = 0;

    HMAC(md, key, keylen, data, datalen, out, &len);
    return (int)len;
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
 *   sik = HASH(Kg,  Rc || Rm || role || userlen || user)
 *   k1  = HASH(sik, 0x01 x n)
 *   k2  = HASH(sik, 0x02 x n)            (AES-128 key = k2[0:16])
 * where HASH is HMAC-SHA1 (n = 20) for cipher suite 3 and HMAC-SHA256
 * (n = 32) for suites 17/18/19.
 */
static void derive_keys(session_t *s, const char *pass, uint8_t role)
{
    uint8_t buf[128], one[MAX_DIGEST], two[MAX_DIGEST];
    int n = 0, ul = (int)strlen(s->user);
    const EVP_MD *md = auth_md(s->auth_algo);

    s->dig_len = dig_len_of(s->auth_algo);
    s->icv_len = icv_len_of(s->integ_algo);

    memcpy(buf + n, s->rc_rand, 16); n += 16;
    memcpy(buf + n, s->rm_rand, 16); n += 16;
    buf[n++] = role;
    buf[n++] = (uint8_t)ul;
    memcpy(buf + n, s->user, ul); n += ul;

    hmac_md(md, (const uint8_t *)pass, (int)strlen(pass), buf, n, s->sik);

    /* variant 0: twenty 0x01 / 0x02 bytes (what ipmitool and pyghmi do) */
    memset(one, 0x01, 20);
    memset(two, 0x02, 20);
    hmac_md(md, s->sik, s->dig_len, one, 20, s->k1);
    hmac_md(md, s->sik, s->dig_len, two, 20, s->k2);

    if (s->dig_len == 20) {
        memcpy(s->k1_alt, s->k1, sizeof(s->k1));
        memcpy(s->k2_alt, s->k2, sizeof(s->k2));
        s->key_variant = 0;
    } else {
        /* variant 1: as many 0x01 / 0x02 bytes as the digest is long */
        memset(one, 0x01, (size_t)s->dig_len);
        memset(two, 0x02, (size_t)s->dig_len);
        hmac_md(md, s->sik, s->dig_len, one, s->dig_len, s->k1_alt);
        hmac_md(md, s->sik, s->dig_len, two, s->dig_len, s->k2_alt);
        s->key_variant = -1;            /* decide on the first packet */
    }
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

/*
 * User database.
 *
 * The file has two accepted shapes, one line per user:
 *
 *   name:password                          legacy, gets the next free id,
 *                                          privilege 4 (administrator), enabled
 *   id:name:password:max-priv:enabled       written by Set User Access below
 *
 * Plaintext passwords are required: RAKP uses the password itself as the HMAC
 * key, so a hash from /etc/shadow is useless here.
 */
static void load_users(void)
{
    FILE *f;
    char line[160];
    int next_id = 1;

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
        char *field[5];
        int nf = 0;
        char *p;
        ipmi_user_t *u;

        trim(line);
        if (line[0] == '#' || line[0] == '\0')
            continue;

        p = line;
        while (nf < 5) {
            field[nf++] = p;
            p = strchr(p, ':');
            if (!p)
                break;
            *p++ = '\0';
        }
        if (nf < 2)
            continue;

        u = &g_users[g_nusers];
        memset(u, 0, sizeof(*u));
        if (nf >= 4) {
            u->id      = (uint8_t)atoi(field[0]);
            u->priv    = (uint8_t)atoi(field[3]);
            u->enabled = nf >= 5 ? (uint8_t)(atoi(field[4]) ? 1 : 0) : 1;
            snprintf(u->name, sizeof(u->name), "%s", field[1]);
            snprintf(u->pass, sizeof(u->pass), "%s", field[2]);
        } else {
            u->id      = (uint8_t)next_id;
            u->priv    = 4;
            u->enabled = 1;
            snprintf(u->name, sizeof(u->name), "%s", field[0]);
            snprintf(u->pass, sizeof(u->pass), "%s", field[1]);
        }
        trim(u->name);
        trim(u->pass);

        if (u->id < 1 || u->id > MAX_USERS)
            u->id = (uint8_t)next_id;
        if (u->priv < 1 || u->priv > 5)
            u->priv = 4;
        /* a user may exist without a password yet (provisioning order is
         * name first, password second), but a record without a name is useless */
        if (!u->name[0])
            continue;
        if (u->id >= next_id)
            next_id = u->id + 1;
        g_nusers++;
    }
    fclose(f);
    logmsg("loaded %d ipmi user(s) from %s", g_nusers, PASSWD_FILE);
}

static void save_users(void)
{
    FILE *f;
    int i;
    const char *tmp = PASSWD_FILE ".new";

    /* write-then-rename: a power cut in the middle must not leave a truncated
     * user database behind */
    f = fopen(tmp, "w");
    if (!f) {
        logmsg("cannot rewrite %s: %s", PASSWD_FILE, strerror(errno));
        return;
    }
    fprintf(f, "# IPMI users: id:name:password:max-privilege:enabled\n"
               "# privilege 1=callback 2=user 3=operator 4=administrator 5=OEM\n");
    for (i = 0; i < g_nusers; i++)
        fprintf(f, "%u:%s:%s:%u:%u\n", g_users[i].id, g_users[i].name,
                g_users[i].pass, g_users[i].priv, g_users[i].enabled);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    chmod(tmp, 0600);
    if (rename(tmp, PASSWD_FILE) != 0) {
        logmsg("cannot replace %s: %s", PASSWD_FILE, strerror(errno));
        remove(tmp);
    }
    sync();
}

static ipmi_user_t *user_by_id(uint8_t id)
{
    int i;
    for (i = 0; i < g_nusers; i++)
        if (g_users[i].id == id)
            return &g_users[i];
    return NULL;
}

static ipmi_user_t *user_by_name(const char *name)
{
    int i;
    if (name == NULL || name[0] == '\0')
        return NULL;
    for (i = 0; i < g_nusers; i++)
        if (strcmp(g_users[i].name, name) == 0)
            return &g_users[i];
    return NULL;
}

/* Return the record for an IPMI user id, creating an empty slot when asked to.
 * Real BMCs let a client configure any user id, so "Set User Name 3" has to
 * work on a slot that has never been used. */
static ipmi_user_t *user_slot(uint8_t id, int create)
{
    ipmi_user_t *u = user_by_id(id);

    if (u || !create || g_nusers >= MAX_USERS || id < 1 || id > MAX_USERS)
        return u;
    u = &g_users[g_nusers];
    memset(u, 0, sizeof(*u));
    u->id = id;
    u->priv = 1;
    u->enabled = 1;
    g_nusers++;
    return u;
}

static const char *user_password(const char *name)
{
    ipmi_user_t *u = user_by_name(name);
    /* an account without a password must not authenticate: RAKP would happily
     * derive keys from the empty string */
    if (u == NULL || !u->enabled || u->pass[0] == '\0')
        return NULL;
    return u->pass;
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

/* ------------------------------------------- SEL / SDR / FRU / boot flags */

#define SEL_FILE        "/var/log/sel"
#define SEL_SLOT        20              /* vendor slot size in /var/log/sel */
#define SEL_MAX         200
#define FRU_EEPROM      "/sys/bus/i2c/devices/0-0050/eeprom"
#define BOOT_FLAGS_FILE "/config/ipmi-boot-flags"
#define MAX_SENSORS     48
#define SDR_BLOB_MAX    (MAX_SENSORS * 80)

#define NETFN_SENSOR    0x04
#define NETFN_STORAGE   0x0A
#define NETFN_TRANSPORT 0x0C

/* hwmon-backed sensor, rebuilt at startup so `ipmitool sensor` has an SDR */
typedef struct {
    uint8_t num;
    uint8_t sensor_type;        /* 0x01 temperature, 0x02 voltage, 0x04 fan */
    uint8_t unit1;
    int     divisor;            /* raw = milli / divisor */
    int     mult;               /* M, so the reported value is raw * M */
    uint8_t thr_mask;           /* bit3 UNC, bit4 UC, bit5 UNR readable */
    int     unc, uc;
    char    name[22];
    char    path[96];
} sensor_t;

static sensor_t g_sensors[MAX_SENSORS];
static int      g_nsensors;
static int      g_sdr_off[MAX_SENSORS];
static int      g_sdr_len[MAX_SENSORS];
static uint8_t  g_sdr_blob[SDR_BLOB_MAX];
static int      g_sdr_total;

static uint8_t  g_fru[256];
static int      g_frulen;
static int      g_fru_board_off, g_fru_product_off;
static uint8_t  g_boot_flags, g_boot_dev;
static uint8_t  g_selwork[SEL_MAX * 16];

static int read_milli(const char *path, long *out)
{
    FILE *f = fopen(path, "r");
    int rc;

    if (!f)
        return -1;
    rc = fscanf(f, "%ld", out);
    fclose(f);
    return rc == 1 ? 0 : -1;
}

/* read a one-line text file and strip the trailing newline */
static int read_str(const char *path, char *out, int outlen)
{
    FILE *f = fopen(path, "r");
    int n;

    if (!f)
        return -1;
    if (fgets(out, outlen, f) == NULL) {
        fclose(f);
        out[0] = '\0';
        return -1;
    }
    fclose(f);
    n = (int)strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
        out[--n] = '\0';
    return 0;
}

static void sensor_add(const char *name, int idx, uint8_t stype, uint8_t unit1,
                       int divisor, int mult, const char *dir, const char *fmt,
                       const char *maxfmt, const char *critfmt)
{
    sensor_t *s;
    char path[96], aux[96];
    long v;

    if (g_nsensors >= MAX_SENSORS)
        return;
    snprintf(path, sizeof(path), fmt, dir, idx);
    if (read_milli(path, &v) != 0)
        return;                             /* channel absent or empty */

    s = &g_sensors[g_nsensors];
    memset(s, 0, sizeof(*s));
    s->num         = (uint8_t)(g_nsensors + 1);
    s->sensor_type = stype;
    s->unit1       = unit1;
    s->divisor     = divisor;
    s->mult        = mult;
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->path, sizeof(s->path), "%s", path);

    if (maxfmt) {
        snprintf(aux, sizeof(aux), maxfmt, dir, idx);
        if (read_milli(aux, &v) == 0 && v > 0) {
            s->unc = (int)(v / divisor);
            s->thr_mask |= 0x08;
        }
    }
    if (critfmt) {
        snprintf(aux, sizeof(aux), critfmt, dir, idx);
        if (read_milli(aux, &v) == 0 && v > 0) {
            s->uc = (int)(v / divisor);
            s->thr_mask |= 0x10;
        }
    }
    g_nsensors++;
}

/* hwmonN is assigned by probe order, so look the device up by name */
static char *hwmon_dir(const char *want, char *buf, int buflen)
{
    char p[64], nm[64];
    int i;

    for (i = 0; i <= 7; i++) {
        snprintf(p, sizeof(p), "/sys/class/hwmon/hwmon%d/name", i);
        if (read_str(p, nm, sizeof(nm)) != 0)
            continue;
        if (strcmp(nm, want) == 0) {
            snprintf(buf, buflen, "/sys/class/hwmon/hwmon%d", i);
            return buf;
        }
    }
    return NULL;
}

static void sensors_scan(void)
{
    char d[64], nm[22];
    int i;

    g_nsensors = 0;

    if (hwmon_dir("peci_cputemp.cpu0", d, sizeof(d)) != NULL) {
        for (i = 1; i <= 13; i++) {
            snprintf(nm, sizeof(nm), "CPU Temp %d", i);
            sensor_add(nm, i, 0x01, 0x01, 1000, 1, d,
                       "%s/temp%d_input", "%s/temp%d_max", "%s/temp%d_crit");
        }
    }
    if (hwmon_dir("silc_fan", d, sizeof(d)) != NULL) {
        for (i = 1; i <= 4; i++) {
            snprintf(nm, sizeof(nm), "Fan %d", i);
            sensor_add(nm, i, 0x04, 0x12, 100, 100, d,
                       "%s/fan%d_input", NULL, NULL);
        }
        for (i = 1; i <= 3; i++) {
            snprintf(nm, sizeof(nm), "Board Temp %d", i);
            sensor_add(nm, i, 0x01, 0x01, 1000, 1, d,
                       "%s/temp%d_input", NULL, NULL);
        }
    }
    if (hwmon_dir("ads7830", d, sizeof(d)) != NULL) {
        for (i = 0; i <= 7; i++) {
            snprintf(nm, sizeof(nm), "Volt %d", i);
            sensor_add(nm, i, 0x02, 0x04, 1000, 1, d,
                       "%s/in%d_input", NULL, NULL);
        }
    }
    logmsg("sdr: %d sensors", g_nsensors);
}

/* Full Sensor Record (type 01h), 50 bytes fixed + ID string */
static int sdr_build_record(uint8_t *p, int id, const sensor_t *s)
{
    int nl = (int)strlen(s->name);

    memset(p, 0, 80);
    p[0]  = (uint8_t)(id & 0xff);
    p[1]  = (uint8_t)(id >> 8);
    p[2]  = 0x51;                       /* SDR version */
    p[3]  = 0x01;                       /* full sensor record */
    p[4]  = (uint8_t)(43 + nl);         /* bytes after this field */
    p[5]  = 0x20;                       /* owned by the BMC */
    p[6]  = 0x00;
    p[7]  = s->num;
    p[8]  = 0x07;                       /* entity: system board */
    p[9]  = 0x01;
    p[10] = 0x00;                       /* no initialisation required */
    p[11] = 0x40;                       /* sensor readable */
    p[12] = s->sensor_type;
    p[13] = 0x01;                       /* threshold based */
    p[14] = s->thr_mask;                /* assertion / lower threshold mask */
    p[16] = s->thr_mask;                /* deassertion / upper threshold mask */
    p[20] = s->unit1;
    p[23] = 0x00;                       /* linear */
    p[24] = (uint8_t)(s->mult & 0xff);  /* M */
    p[25] = (uint8_t)(((s->mult >> 8) & 0x03) << 6);
    p[35] = 0xff;                       /* maximum reading */
    if (s->thr_mask & 0x10)
        p[38] = (uint8_t)s->uc;
    if (s->thr_mask & 0x08)
        p[39] = (uint8_t)s->unc;
    p[47] = (uint8_t)(0xc0 | nl);       /* ID string, ASCII+Latin1 */
    memcpy(p + 48, s->name, (size_t)nl);
    return 48 + nl;
}

static void sdr_build(void)
{
    int i, id = 1;

    g_sdr_total = 0;
    for (i = 0; i < g_nsensors; i++) {
        int len = sdr_build_record(g_sdr_blob + g_sdr_total, id, &g_sensors[i]);
        g_sdr_off[i]  = g_sdr_total;
        g_sdr_len[i]  = len;
        g_sdr_total  += len;
        id++;
    }
}

/* ------------------------------------------------------- user SDR records */
/*
 * The sensor records above are derived from /dev/i2c-* and sysfs on every
 * start, so they cannot be written back -- there is nothing to write them to.
 * What a BMC is still expected to provide is a repository the *host* can add
 * its own records to (Add/Partial Add/Delete/Clear SDR).  Those live in
 * /config and are served after the generated records, with record IDs from
 * 0x0100 up so the two ranges can never collide.
 */
#define SDR_USER_CONF  "/config/ipmi-sdr-user.conf"
#define SDR_USER_MAX   32
#define SDR_REC_MAX    255
#define SDR_USER_ID0   0x0100

static uint8_t  g_usdr[SDR_USER_MAX][SDR_REC_MAX];
static uint16_t g_usdr_len[SDR_USER_MAX];
static int      g_nusdr;
static uint32_t g_sdr_add_ts, g_sdr_erase_ts;

/* staging area for Partial Add SDR, which arrives in pieces */
static uint8_t  g_sdr_stage[SDR_REC_MAX];
static int      g_sdr_stage_len, g_sdr_stage_off;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void sdr_user_load(void)
{
    char line[SDR_REC_MAX * 2 + 16];
    FILE *f = fopen(SDR_USER_CONF, "r");

    g_nusdr = 0;
    g_sdr_add_ts = g_sdr_erase_ts = 0;
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL && g_nusdr < SDR_USER_MAX) {
        char *p = line;
        int n = 0;

        if (sscanf(line, "#ts %u %u", &g_sdr_add_ts, &g_sdr_erase_ts) == 2)
            continue;
        while (n < SDR_REC_MAX) {
            int hi = hexval((unsigned char)p[0]), lo;
            if (hi < 0)
                break;
            lo = hexval((unsigned char)p[1]);
            if (lo < 0)
                break;
            g_usdr[g_nusdr][n++] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        if (n >= 5)
            g_usdr_len[g_nusdr++] = (uint16_t)n;
    }
    fclose(f);
}

static void sdr_user_save(void)
{
    char tmp[128];
    FILE *f;
    int i, k;

    snprintf(tmp, sizeof(tmp), "%s.tmp", SDR_USER_CONF);
    f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fprintf(f, "#ts %u %u\n", g_sdr_add_ts, g_sdr_erase_ts);
    for (i = 0; i < g_nusdr; i++) {
        for (k = 0; k < g_usdr_len[i]; k++)
            fprintf(f, "%02x", g_usdr[i][k]);
        fputc('\n', f);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    chmod(tmp, 0600);
    if (rename(tmp, SDR_USER_CONF) != 0) {
        logmsg("cannot replace %s: %s", SDR_USER_CONF, strerror(errno));
        remove(tmp);
    }
}

static int sdr_rec_count(void)
{
    return g_nsensors + g_nusdr;
}

/* 0-based index -> record id */
static uint16_t sdr_id_of(int idx)
{
    return (uint16_t)(idx < g_nsensors ? idx + 1 : SDR_USER_ID0 + (idx - g_nsensors));
}

/* record id -> 0-based index, -1 when absent */
static int sdr_idx_of(uint16_t rid)
{
    if (rid >= 1 && rid <= g_nsensors)
        return (int)rid - 1;
    if (rid >= SDR_USER_ID0 && rid < SDR_USER_ID0 + g_nusdr)
        return g_nsensors + (int)(rid - SDR_USER_ID0);
    return -1;
}

static const uint8_t *sdr_rec_of(int idx, int *len)
{
    if (idx < g_nsensors) {
        *len = g_sdr_len[idx];
        return g_sdr_blob + g_sdr_off[idx];
    }
    idx -= g_nsensors;
    if (idx < 0 || idx >= g_nusdr)
        return NULL;
    *len = g_usdr_len[idx];
    return g_usdr[idx];
}

static int sdr_user_add(const uint8_t *rec, int len)
{
    if (len < 5 || len > SDR_REC_MAX || g_nusdr >= SDR_USER_MAX)
        return -1;
    memcpy(g_usdr[g_nusdr], rec, (size_t)len);
    g_usdr_len[g_nusdr] = (uint16_t)len;
    return g_nusdr++;
}

/* vendor slots are 20 bytes; IPMI wants standard 16-byte records */
/*
 * Records come out of the vendor file in *slot* order, and the slot order is
 * not the record-ID order: the vendor daemon reuses slots, so after a delete
 * the file can hold id 0x00c0 followed by id 0x0030.  Clients page through the
 * SEL by following the "next record id" field, so a link that goes backwards
 * makes them loop forever (pyghmi does exactly that).  Hence: sort by record
 * id and drop older records that share one -- for a client the id *is* the
 * slot identity, so the newest occupant is the record.
 */
static unsigned sel_id_of(const uint8_t *rec)
{
    return (unsigned)rec[0] | ((unsigned)rec[1] << 8);
}

static int sel_rec_cmp(const void *a, const void *b)
{
    const uint8_t *ra = (const uint8_t *)a, *rb = (const uint8_t *)b;
    unsigned ia = sel_id_of(ra), ib = sel_id_of(rb);

    if (ia != ib)
        return ia < ib ? -1 : 1;
    /* same id: newest first so the de-dup pass keeps the live record */
    {
        uint32_t ta = rd_le32(ra + 3), tb = rd_le32(rb + 3);
        if (ta == tb)
            return 0;
        return ta > tb ? -1 : 1;
    }
}

static int sel_load(uint8_t *out, int maxrec)
{
    FILE *f = fopen(SEL_FILE, "rb");
    uint8_t slot[SEL_SLOT];
    int i, n = 0, keep = 0;

    if (!f)
        return 0;
    while (n < maxrec && fread(slot, 1, SEL_SLOT, f) == SEL_SLOT) {
        if (slot[2] != 0x02)            /* not a system event record */
            continue;
        out[n * 16 + 0]  = slot[1];     /* record id (low byte) */
        out[n * 16 + 1]  = 0x00;
        out[n * 16 + 2]  = slot[2];     /* record type */
        out[n * 16 + 3]  = slot[7];     /* timestamp: big endian -> little */
        out[n * 16 + 4]  = slot[6];
        out[n * 16 + 5]  = slot[5];
        out[n * 16 + 6]  = slot[4];
        out[n * 16 + 7]  = slot[8];     /* generator id */
        out[n * 16 + 8]  = slot[9];
        out[n * 16 + 9]  = slot[10];    /* event message format version */
        out[n * 16 + 10] = slot[11];    /* sensor type */
        out[n * 16 + 11] = slot[12];    /* sensor number */
        out[n * 16 + 12] = slot[13];    /* event dir / event type */
        out[n * 16 + 13] = slot[14];    /* event data 1..3 */
        out[n * 16 + 14] = slot[15];
        out[n * 16 + 15] = slot[16];
        n++;
    }
    fclose(f);

    if (n > 1)
        qsort(out, (size_t)n, 16, sel_rec_cmp);
    for (i = 0; i < n; i++) {
        if (keep > 0 && sel_id_of(out + keep * 16 - 16) == sel_id_of(out + i * 16))
            continue;                   /* older record in the same slot */
        if (keep != i)
            memcpy(out + keep * 16, out + i * 16, 16);
        keep++;
    }
    return keep;
}

static uint32_t sel_last_timestamp(void)
{
    static uint8_t recs[SEL_MAX * 16];
    uint32_t last = 0;
    int i, n = sel_load(recs, SEL_MAX);

    for (i = 0; i < n; i++) {
        uint32_t ts = rd_le32(recs + i * 16 + 3);
        if (ts > last)
            last = ts;
    }
    return last;
}

static void sel_clear(void)
{
    uint8_t zero[512];
    FILE *f;

    memset(zero, 0, sizeof(zero));
    f = fopen(SEL_FILE, "wb");
    if (!f)
        return;
    for (int i = 0; i < (SEL_SLOT * SEL_MAX) / (int)sizeof(zero); i++)
        fwrite(zero, 1, sizeof(zero), f);
    fclose(f);
    logmsg("sel: cleared");
}

/* ---- FRU: convert the ONIE TLV EEPROM into an IPMI FRU image ---- */

static struct {
    char manufacturer[40];
    char product[40];
    char part[40];
    char serial[40];
    char version[40];
    char mfgdate[24];
} g_fruinfo;

static void tlv_str(const uint8_t *buf, int totlen, uint8_t type,
                    char *out, int outlen)
{
    int i = 11, end = 11 + totlen;

    out[0] = '\0';
    while (i + 1 < end && i + 1 < 256) {
        int t = buf[i], l = buf[i + 1];
        if (t == 0xfe || l < 0)
            return;
        if (t == type) {
            int c = l < outlen - 1 ? l : outlen - 1;
            memcpy(out, buf + i + 2, (size_t)c);
            out[c] = '\0';
            return;
        }
        i += 2 + l;
    }
}

/* "MM/DD/YYYY hh:mm:ss" -> minutes since 1996-01-01, IPMI board mfg date */
static int32_t fru_date_minutes(const char *s)
{
    int mo = 0, dy = 0, yy = 0, hh = 0, mi = 0;
    int32_t days;
    static const int mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    if (sscanf(s, "%d/%d/%d %d:%d", &mo, &dy, &yy, &hh, &mi) < 3)
        return 0;
    if (mo < 1 || mo > 12 || yy < 1996)
        return 0;
    days = (yy - 1996) * 365 + (yy - 1996 + 3) / 4 + mdays[mo - 1] + (dy - 1);
    if (mo > 2 && ((yy % 4) == 0))
        days++;
    return days * 1440 + hh * 60 + mi;
}

typedef struct { uint8_t type; const char *val; } fru_tlv_t;

static int fru_build_area(uint8_t *a, int with_date, int32_t minutes,
                          const fru_tlv_t *tlvs, int n)
{
    int off = with_date ? 6 : 3;
    int i, total, pad, sum = 0;

    a[0] = 0x01;                        /* area format version */
    a[2] = 0x19;                        /* English */
    if (with_date) {
        a[3] = (uint8_t)(minutes & 0xff);
        a[4] = (uint8_t)((minutes >> 8) & 0xff);
        a[5] = (uint8_t)((minutes >> 16) & 0xff);
    }
    for (i = 0; i < n; i++) {
        int l = 0;

        if (tlvs[i].val != NULL)
            l = (int)strlen(tlvs[i].val);
        if (l > 63)
            l = 63;
        /* FRU fields are positional, so an empty field still has to be emitted
         * as a zero-length one: leaving it out shifts everything after it. */
        a[off++] = (uint8_t)(0xc0 | l);
        if (l) {
            memcpy(a + off, tlvs[i].val, (size_t)l);
            off += l;
        }
    }
    a[off++] = 0xc1;                    /* end of list */
    total = off + 1;
    pad = (8 - (total % 8)) % 8;
    while (pad-- > 0)
        a[off++] = 0x00;
    total = off + 1;
    a[1] = (uint8_t)(total / 8);
    for (i = 0; i < off; i++)
        sum += a[i];
    a[off] = (uint8_t)(0x100 - (sum & 0xff));
    return total;
}

static void fru_build(void)
{
    uint8_t eeprom[256];
    fru_tlv_t board[6], prod[7];
    FILE *f;
    int got = 0, blen, plen, sum = 0, i;

    g_frulen = 0;
    memset(&g_fruinfo, 0, sizeof(g_fruinfo));

    f = fopen(FRU_EEPROM, "rb");
    if (f) {
        got = (int)fread(eeprom, 1, sizeof(eeprom), f);
        fclose(f);
    }
    if (got > 11 && memcmp(eeprom, "TlvInfo", 7) == 0) {
        tlv_str(eeprom, eeprom[10], 0x2d, g_fruinfo.manufacturer, sizeof(g_fruinfo.manufacturer));
        tlv_str(eeprom, eeprom[10], 0x21, g_fruinfo.product,      sizeof(g_fruinfo.product));
        tlv_str(eeprom, eeprom[10], 0x22, g_fruinfo.part,         sizeof(g_fruinfo.part));
        tlv_str(eeprom, eeprom[10], 0x23, g_fruinfo.serial,       sizeof(g_fruinfo.serial));
        tlv_str(eeprom, eeprom[10], 0x27, g_fruinfo.version,      sizeof(g_fruinfo.version));
        tlv_str(eeprom, eeprom[10], 0x25, g_fruinfo.mfgdate,      sizeof(g_fruinfo.mfgdate));
    } else {
        /* no TLV EEPROM: fall back to the product files */
        g_fruinfo.manufacturer[0] = '\0';
        read_str("/etc/product/vendor", g_fruinfo.manufacturer, sizeof(g_fruinfo.manufacturer));
        read_str("/etc/product/UBMC/OEMI/model.txt", g_fruinfo.product, sizeof(g_fruinfo.product));
        read_str("/etc/product/UBMC/version/software_version.txt", g_fruinfo.version,
                 sizeof(g_fruinfo.version));
    }

    board[0].type = 0x02; board[0].val = g_fruinfo.manufacturer;
    board[1].type = 0x03; board[1].val = g_fruinfo.product;
    board[2].type = 0x04; board[2].val = g_fruinfo.serial;
    board[3].type = 0x05; board[3].val = g_fruinfo.part;
    board[4].type = 0x06; board[4].val = "ubmc-fru";
    board[5].type = 0x00; board[5].val = NULL;

    prod[0].type = 0x02; prod[0].val = g_fruinfo.manufacturer;
    prod[1].type = 0x03; prod[1].val = g_fruinfo.product;
    prod[2].type = 0x04; prod[2].val = g_fruinfo.part;
    prod[3].type = 0x05; prod[3].val = g_fruinfo.version;
    prod[4].type = 0x06; prod[4].val = g_fruinfo.serial;
    prod[5].type = 0x07; prod[5].val = NULL;
    prod[6].type = 0x08; prod[6].val = "ubmc-fru";

    memset(g_fru, 0, sizeof(g_fru));
    g_fru[0] = 0x01;                    /* common header version */
    g_fru[1] = 0x00;                    /* internal use: none */
    g_fru[2] = 0x00;                    /* chassis info: none */
    g_fru_board_off   = 8;
    g_fru_product_off = 8 + fru_build_area(g_fru + 8, 1,
                                           fru_date_minutes(g_fruinfo.mfgdate),
                                           board, 5);
    plen = fru_build_area(g_fru + g_fru_product_off, 0, 0, prod, 7);
    /* the common header stores area offsets in 8-byte units */
    g_fru[3] = (uint8_t)(g_fru_board_off / 8);
    g_fru[4] = (uint8_t)(g_fru_product_off / 8);
    g_fru[5] = 0x00;                    /* multi-record: none */
    g_fru[6] = 0x00;                    /* pad */
    for (i = 0; i < 7; i++)
        sum += g_fru[i];
    g_fru[7] = (uint8_t)(0x100 - (sum & 0xff));
    g_frulen = g_fru_product_off + plen;
    blen = g_fru_product_off - g_fru_board_off;
    logmsg("fru: %d bytes (board %d, product %d) %s / %s",
           g_frulen, blen, plen, g_fruinfo.manufacturer, g_fruinfo.product);
}

static void boot_flags_load(void)
{
    char buf[16];
    unsigned f = 0, d = 0;

    g_boot_flags = 0;
    g_boot_dev = 0;
    if (read_str(BOOT_FLAGS_FILE, buf, sizeof(buf)) != 0)
        return;
    if (sscanf(buf, "%x %x", &f, &d) == 2) {
        g_boot_flags = (uint8_t)f;
        g_boot_dev   = (uint8_t)d;
    }
}

static void boot_flags_save(void)
{
    FILE *f = fopen(BOOT_FLAGS_FILE, "w");

    if (!f)
        return;
    fprintf(f, "%02x %02x\n", g_boot_flags, g_boot_dev);
    fclose(f);
}

/* ------------------------------------------------------------ SOL control */

static session_t *g_sol_owner;
static int        g_ipmi_fd = -1;
static int        g_break_fd = -1;      /* /dev/ttyS1, opened lazily for BREAK */
static long       g_break_release_ms;

/* host -> console BREAK detection (see sol_hostbreak_poll) */
static int        g_hostbrk_fd = -1;
static int        g_hostbrk_last;
static int        g_hostbrk_pending;
static long       g_hostbrk_poll_ms;
static long       g_hostbrk_retry_ms;

static void sol_deactivate(session_t *s);
static void sol_flush(void);
static void sol_pump(void);
static void sol_recv_payload(session_t *s, const uint8_t *pl, int plen);
static void sol_retransmit(void);
static void sol_break_service(void);

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * A real serial BREAK: TIOCSBRK on the host UART.  Reading is unaffected by a
 * write-only open, so is_console_manager keeps the console; this only pulls the
 * line low for SOL_BREAK_MS.  Note the host kernel turns a BREAK into SysRq, so
 * this fires the host's SysRq handler - exactly like a hardware BMC does.
 */
static void sol_break_assert(void)
{
    if (g_break_fd < 0) {
        g_break_fd = open(HOST_UART, O_WRONLY | O_NOCTTY | O_NONBLOCK);
        if (g_break_fd < 0) {
            logmsg("sol: BREAK unavailable, open %s: %s", HOST_UART,
                   strerror(errno));
            return;
        }
    }
    if (ioctl(g_break_fd, TIOCSBRK, 0) < 0) {
        logmsg("sol: TIOCSBRK: %s", strerror(errno));
        close(g_break_fd);
        g_break_fd = -1;
        return;
    }
    g_break_release_ms = now_ms() + SOL_BREAK_MS;
    logmsg("sol: BREAK asserted for %d ms", SOL_BREAK_MS);
}

static void sol_break_service(void)
{
    if (g_break_release_ms == 0 || now_ms() < g_break_release_ms)
        return;
    if (g_break_fd >= 0)
        ioctl(g_break_fd, TIOCCBRK, 0);
    g_break_release_ms = 0;
}

/*
 * A BREAK sent by the host never arrives as data: is_console_manager reads the
 * port as a byte stream and the line event is consumed by the tty layer.  The
 * driver does keep a per-port error counter for it though, so poll
 * TIOCGICOUNT and treat a bump of icount.brk as "the host sent a BREAK".
 *
 * Opening the port read-only is safe as long as we never read() from it, so
 * the console manager keeps every byte.
 */
static void sol_hostbreak_poll(void)
{
    struct serial_icounter_struct ic;
    long now = now_ms();
    int delta;

    if (g_hostbrk_fd < 0) {
        if (g_hostbrk_retry_ms != 0 && now < g_hostbrk_retry_ms)
            return;
        g_hostbrk_fd = open(HOST_UART, O_RDONLY | O_NOCTTY | O_NONBLOCK);
        if (g_hostbrk_fd < 0) {
            g_hostbrk_retry_ms = now + 60000;
            logmsg("host BREAK detection off, open %s: %s", HOST_UART,
                   strerror(errno));
            return;
        }
        memset(&ic, 0, sizeof(ic));
        if (ioctl(g_hostbrk_fd, TIOCGICOUNT, &ic) < 0) {
            logmsg("host BREAK detection off, TIOCGICOUNT: %s", strerror(errno));
            close(g_hostbrk_fd);
            g_hostbrk_fd = -1;
            g_hostbrk_retry_ms = now + 60000;
            return;
        }
        g_hostbrk_last = ic.brk;
        g_hostbrk_poll_ms = now;
        logmsg("host BREAK detection on, brk=%d", ic.brk);
        return;
    }

    if (now - g_hostbrk_poll_ms < 500)
        return;
    g_hostbrk_poll_ms = now;

    memset(&ic, 0, sizeof(ic));
    if (ioctl(g_hostbrk_fd, TIOCGICOUNT, &ic) < 0)
        return;
    if (ic.brk == g_hostbrk_last)
        return;

    delta = ic.brk - g_hostbrk_last;
    if (delta < 0)
        delta = 1;                      /* counter wrapped, report one */
    if (delta > 8)
        delta = 8;
    g_hostbrk_pending += delta;
    if (g_hostbrk_pending > 8)
        g_hostbrk_pending = 8;
    logmsg("host BREAK received (%d -> %d)", g_hostbrk_last, ic.brk);
    g_hostbrk_last = ic.brk;
}

/* forget breaks seen while no session was listening */
static void sol_hostbreak_rebaseline(void)
{
    struct serial_icounter_struct ic;

    g_hostbrk_pending = 0;
    g_hostbrk_poll_ms = now_ms();
    if (g_hostbrk_fd < 0)
        return;
    memset(&ic, 0, sizeof(ic));
    if (ioctl(g_hostbrk_fd, TIOCGICOUNT, &ic) == 0)
        g_hostbrk_last = ic.brk;
}

/*
 * The host serial is multiplexed by is_console_manager: it reads /dev/ttyS1 and
 * fans the bytes out over CONSOLE_SOCK, where every ttyd/serial_socket_client
 * pair already connects.  Joining that as one more client gives SOL both read
 * and write access without stealing bytes from the web console, and without
 * touching /dev/ttyF0 (which is write-only and therefore useless for reading
 * host output).
 */
static int sol_activate(session_t *s)
{
    struct sockaddr_un sa;
    int fd, fl;

    if (g_sol_owner != NULL && g_sol_owner != s)
        return 0x80;                    /* payload already owned by a session */
    if (s->sol_cfd >= 0)
        return 0;                       /* already active for this session */

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        logmsg("sol: socket: %s", strerror(errno));
        return 0x81;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", CONSOLE_SOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        logmsg("sol: connect %s: %s", CONSOLE_SOCK, strerror(errno));
        close(fd);
        return 0x81;                    /* "SOL is disabled" */
    }
    fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    s->sol_cfd   = fd;
    s->sol_tx_seq = 1;
    s->sol_ack_seq = 0;
    s->sol_rx_seq = 0;
    s->sol_outlen = 0;
    s->sol_ninflight = 0;
    s->sol_retry_ms = 0;
    s->sol_acks_seen = 0;
    s->sol_noack = 0;
    memset(s->sol_inflight, 0, sizeof(s->sol_inflight));
    g_sol_owner  = s;
    sol_hostbreak_rebaseline();
    logmsg("sol: activated for rm=%08x", s->rm_id);
    return 0;
}

static void sol_deactivate(session_t *s)
{
    if (s->sol_cfd >= 0) {
        close(s->sol_cfd);
        s->sol_cfd = -1;
        logmsg("sol: deactivated for rm=%08x", s->rm_id);
    }
    s->sol_outlen = 0;
    s->sol_ninflight = 0;
    s->sol_retry_ms = 0;
    s->sol_acks_seen = 0;
    s->sol_noack = 0;
    if (g_break_release_ms) {
        if (g_break_fd >= 0)
            ioctl(g_break_fd, TIOCCBRK, 0);
        g_break_release_ms = 0;
    }
    if (g_sol_owner == s)
        g_sol_owner = NULL;
}

/* ------------------------------------------------- channel network info */

/*
 * The LAN configuration parameters describe the interface a request came in
 * on, so the destination address of the request is captured from IP_PKTINFO
 * in the main loop and remembered here (single threaded, so one slot is
 * enough).
 */
static struct in_addr g_local_ip;

/* find the interface that owns `want`, its netmask and its MAC */
static int iface_for_addr(struct in_addr want, char *name, size_t namelen,
                          struct in_addr *mask, unsigned char mac[6])
{
    struct ifaddrs *ifs, *ifp;
    int found = -1;
    char path[64], buf[32];
    unsigned m[6];

    if (getifaddrs(&ifs) != 0)
        return -1;
    for (ifp = ifs; ifp != NULL; ifp = ifp->ifa_next) {
        struct sockaddr_in *sin;

        if (ifp->ifa_addr == NULL || ifp->ifa_addr->sa_family != AF_INET)
            continue;
        sin = (struct sockaddr_in *)ifp->ifa_addr;
        if (sin->sin_addr.s_addr != want.s_addr)
            continue;
        snprintf(name, namelen, "%s", ifp->ifa_name);
        mask->s_addr = 0;
        if (ifp->ifa_netmask != NULL)
            mask->s_addr =
                ((struct sockaddr_in *)ifp->ifa_netmask)->sin_addr.s_addr;
        found = 0;
        break;
    }
    freeifaddrs(ifs);
    if (found != 0)
        return -1;

    memset(mac, 0, 6);
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", name);
    {
        FILE *f = fopen(path, "r");
        if (f != NULL) {
            if (fgets(buf, sizeof(buf), f) != NULL &&
                sscanf(buf, "%x:%x:%x:%x:%x:%x",
                       &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                int i;
                for (i = 0; i < 6; i++)
                    mac[i] = (unsigned char)m[i];
            }
            fclose(f);
        }
    }
    return 0;
}

/* the interface's default gateway, or 0.0.0.0 when it has none */
static struct in_addr iface_gateway(const char *name)
{
    struct in_addr gw;
    char line[256];
    FILE *f;

    gw.s_addr = 0;
    if (*name == '\0')
        return gw;
    f = fopen("/proc/net/route", "r");
    if (f == NULL)
        return gw;
    if (fgets(line, sizeof(line), f) == NULL) {          /* column header */
        fclose(f);
        return gw;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char ifname[32], dest[16], gate[16];
        unsigned flags;

        if (sscanf(line, "%31s %15s %15s %x", ifname, dest, gate, &flags) != 4)
            continue;
        if (strcmp(ifname, name) != 0 || strcmp(dest, "00000000") != 0)
            continue;
        gw.s_addr = (in_addr_t)strtoul(gate, NULL, 16);
        break;
    }
    fclose(f);
    return gw;
}

/* Read one value out of the vendor's flat config tree: every line is
 * "<path>,<value>".  Returns 0 and fills `out` when the key is present. */
static int conf_lookup(const char *file, const char *key, char *out,
                       size_t outlen)
{
    char line[512];
    size_t klen = strlen(key);
    FILE *f;

    f = fopen(file, "r");
    if (f == NULL)
        return -1;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *v;
        size_t n;

        if (strncmp(line, key, klen) != 0 || line[klen] != ',')
            continue;
        v = line + klen + 1;
        n = strlen(v);
        while (n > 0 && (v[n - 1] == '\n' || v[n - 1] == '\r' ||
                         v[n - 1] == ' ' || v[n - 1] == '\t'))
            v[--n] = '\0';
        snprintf(out, outlen, "%s", v);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/*
 * Write a single "<key>,<value>" line of the mgmtd configuration tree and
 * refresh the checksum next to it.  is_mgmtd validates /config/is_mgmtd.conf
 * with `sha1sum -c` at startup and rolls the file back when it does not match,
 * so the two always have to be updated together.
 */
static int conf_set_line(const char *key, const char *val)
{
    FILE *in, *out;
    char line[512], want[512];
    const char *tmp = MGMTD_CONF ".new";
    size_t klen = strlen(key);
    int found = 0;

    snprintf(want, sizeof(want), "%s,", key);
    in = fopen(MGMTD_CONF, "r");
    if (in == NULL)
        return -1;
    out = fopen(tmp, "w");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while (fgets(line, sizeof(line), in) != NULL) {
        if (strncmp(line, want, klen + 1) == 0) {
            fprintf(out, "%s,%s\n", key, val);
            found = 1;
            continue;
        }
        fputs(line, out);
    }
    fclose(in);
    fclose(out);
    if (!found || rename(tmp, MGMTD_CONF) != 0) {
        remove(tmp);
        logmsg("conf_set_line: %s not found, nothing written", key);
        return -1;
    }
    return 0;
}

static void conf_rehash(void)
{
    char hash[128] = "";
    FILE *p, *f;

    p = popen("sha1sum " MGMTD_CONF " 2>/dev/null", "r");
    if (p == NULL)
        return;
    if (fgets(hash, sizeof(hash), p) != NULL) {
        char *sp = strchr(hash, ' ');
        if (sp != NULL)
            *sp = '\0';
    }
    pclose(p);
    if (hash[0] == '\0')
        return;

    f = fopen(MGMTD_CONF ".sha1sum", "w");
    if (f == NULL)
        return;
    fprintf(f, "%s  %s\n", hash, MGMTD_CONF);
    fclose(f);
}

/* ---------------------------------------------------------------- PEF */
/*
 * Platform Event Filtering, configuration side only.
 *
 * Nothing acts on a filter yet -- the vendor's SEL reader owns event
 * generation -- so this keeps the settings a management tool can read and
 * write and reports that no filter or alert policy table exists.  A PET
 * sender (SNMPv1 trap to the configured LAN alert destinations) is the piece
 * still missing before PEF can push anything out.
 */
#define PEF_CONF "/config/ipmi-pef.conf"

typedef struct {
    uint8_t global;         /* action global control */
    uint8_t percontrol;     /* action per-control  */
    uint8_t startup;        /* startup delay, 100 ms units */
    uint8_t alert_startup;  /* alert startup delay */
} pef_cfg_t;

static pef_cfg_t g_pef = { 0x01, 0x01, 0x00, 0x00 };

static void pef_load(void)
{
    char line[128];
    FILE *f = fopen(PEF_CONF, "r");

    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned v;
        if (sscanf(line, "global=%u", &v) == 1)             g_pef.global = (uint8_t)v;
        else if (sscanf(line, "percontrol=%u", &v) == 1)    g_pef.percontrol = (uint8_t)v;
        else if (sscanf(line, "startup=%u", &v) == 1)        g_pef.startup = (uint8_t)v;
        else if (sscanf(line, "alert_startup=%u", &v) == 1)  g_pef.alert_startup = (uint8_t)v;
    }
    fclose(f);
}

static void pef_save(void)
{
    FILE *f = fopen(PEF_CONF, "w");

    if (f == NULL) {
        logmsg("cannot write %s: %s", PEF_CONF, strerror(errno));
        return;
    }
    fprintf(f, "global=%u\npercontrol=%u\nstartup=%u\nalert_startup=%u\n",
            g_pef.global, g_pef.percontrol, g_pef.startup, g_pef.alert_startup);
    fclose(f);
}

/* ------------------------------------------------- LAN alert destinations */
/*
 * The four "alert destination" LAN parameters (17 number, 18 type, 19 address)
 * are what an IPMI management tool writes when it wants the BMC to notify
 * something.  They are stored in one small text file; the PET sender below is
 * the only consumer.
 */
#define ALERT_CONF "/config/ipmi-lan-alert.conf"
#define ALERT_MAX  4

typedef struct {
    uint8_t       enabled;
    uint8_t       type;         /* 0 none, 1 PET, ... (we only send PET) */
    uint8_t       ack_timeout;  /* seconds per retry */
    uint8_t       retries;
    uint8_t       addr_format;  /* 0 = IPv4 */
    struct in_addr ip;
} alert_dest_t;

static alert_dest_t g_alert[ALERT_MAX];

static void alert_load(void)
{
    char line[160];
    FILE *f = fopen(ALERT_CONF, "r");

    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned idx, en, type, ack, ret, fmt, a, b, c, d;
        if (sscanf(line, "%u:%u:%u:%u:%u:%u:%u:%u:%u:%u",
                   &idx, &en, &type, &ack, &ret, &fmt, &a, &b, &c, &d) != 10)
            continue;
        if (idx < 1 || idx > ALERT_MAX)
            continue;
        g_alert[idx - 1].enabled     = (uint8_t)en;
        g_alert[idx - 1].type        = (uint8_t)type;
        g_alert[idx - 1].ack_timeout = (uint8_t)ack;
        g_alert[idx - 1].retries     = (uint8_t)ret;
        g_alert[idx - 1].addr_format = (uint8_t)fmt;
        g_alert[idx - 1].ip.s_addr =
            htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                  ((uint32_t)c << 8) | (uint32_t)d);
    }
    fclose(f);
}

static void alert_save(void)
{
    FILE *f = fopen(ALERT_CONF, "w");
    int i;

    if (f == NULL)
        return;
    for (i = 0; i < ALERT_MAX; i++) {
        uint32_t h = ntohl(g_alert[i].ip.s_addr);
        fprintf(f, "%d:%u:%u:%u:%u:%u:%u:%u:%u:%u\n",
                i + 1, g_alert[i].enabled, g_alert[i].type,
                g_alert[i].ack_timeout, g_alert[i].retries,
                g_alert[i].addr_format,
                (h >> 24) & 0xff, (h >> 16) & 0xff, (h >> 8) & 0xff, h & 0xff);
    }
    fclose(f);
    chmod(ALERT_CONF, 0600);
}

static int alert_count(void)
{
    int i, n = 0;
    for (i = 0; i < ALERT_MAX; i++)
        if (g_alert[i].enabled && g_alert[i].ip.s_addr != 0)
            n++;
    return n;
}

/* ------------------------------------------------------------ PET sender */
/*
 * A Platform Event Trap, i.e. an SNMPv1 trap carrying the PET varbinds.
 * Only the fields a trap receiver actually needs are filled in; the OIDs are
 * the ones defined by the IPMI PET specification (enterprise 3183).
 */
#define PET_ENTERPRISE_OID  "1.3.6.1.4.1.3183"

static int ber_put(uint8_t *out, int *pos, uint8_t tag, const uint8_t *val,
                   int len)
{
    int n = 1;

    if (len < 0x80) {
        out[pos[0]++] = tag;
        out[pos[0]++] = (uint8_t)len;
    } else if (len < 0x100) {
        out[pos[0]++] = tag;
        out[pos[0]++] = 0x81;
        out[pos[0]++] = (uint8_t)len;
        n = 2;
    } else {
        out[pos[0]++] = tag;
        out[pos[0]++] = 0x82;
        out[pos[0]++] = (uint8_t)(len >> 8);
        out[pos[0]++] = (uint8_t)(len & 0xff);
        n = 3;
    }
    if (val != NULL) {
        memcpy(out + pos[0], val, (size_t)len);
        pos[0] += len;
    }
    return n + len;
}


static int ber_int(uint8_t *out, int *pos, int32_t v)
{
    uint8_t b[4];
    int n = 0;

    if (v < 0 || v > 0x7fffffff) {
        b[0] = (uint8_t)((v >> 24) & 0xff);
        b[1] = (uint8_t)((v >> 16) & 0xff);
        b[2] = (uint8_t)((v >> 8) & 0xff);
        b[3] = (uint8_t)(v & 0xff);
        if (b[0] & 0x80) {                   /* keep it positive */
            uint8_t c[5] = { 0, b[0], b[1], b[2], b[3] };
            return ber_put(out, pos, 0x02, c, 5);
        }
        return ber_put(out, pos, 0x02, b, 4);
    }
    do {
        b[3 - n] = (uint8_t)(v & 0xff);
        v >>= 8;
        n++;
    } while (v > 0 && n < 4);
    return ber_put(out, pos, 0x02, b + 4 - n, n);
}

static int oid_encode(const uint32_t *arcs, int narcs, uint8_t *out)
{
    uint32_t vals[24];
    int nvals = 0, len = 0, i;

    /* the first two arcs share one sub-identifier */
    vals[nvals++] = arcs[0] * 40 + arcs[1];
    for (i = 2; i < narcs && nvals < (int)(sizeof(vals) / sizeof(vals[0])); i++)
        vals[nvals++] = arcs[i];

    for (i = 0; i < nvals; i++) {
        uint8_t tmp[5];
        int n = 0, k;
        uint32_t v = vals[i];

        do {
            tmp[n++] = (uint8_t)(v & 0x7f);
            v >>= 7;
        } while (v > 0 && n < 5);

        /* emit most significant group first; every group except the last
         * carries the continuation bit */
        for (k = n - 1; k >= 0; k--) {
            uint8_t b = tmp[k];
            if (k != 0)
                b |= 0x80;
            out[len++] = b;
        }
    }
    return len;
}

static int ber_oid(uint8_t *out, int *pos, const uint32_t *arcs, int narcs)
{
    uint8_t enc[64];
    int n = oid_encode(arcs, narcs, enc);
    return ber_put(out, pos, 0x06, enc, n);
}

/* one varbind: SEQUENCE { OID, value } */
static int ber_varbind_string(uint8_t *out, int *pos, const uint32_t *arcs,
                              int narcs, const char *s)
{
    uint8_t body[512];
    int b = 0;

    ber_oid(body, &b, arcs, narcs);
    ber_put(body, &b, 0x04, (const uint8_t *)s, (int)strlen(s));
    return ber_put(out, pos, 0x30, body, b);
}

/* value is a byte string of known length, not NUL terminated */
static int ber_varbind_octets(uint8_t *out, int *pos, const uint32_t *arcs,
                              int narcs, const uint8_t *s, int len)
{
    uint8_t body[512];
    int b = 0;

    ber_oid(body, &b, arcs, narcs);
    ber_put(body, &b, 0x04, s, len);
    return ber_put(out, pos, 0x30, body, b);
}

/* value is an OBJECT IDENTIFIER, as snmpTrapOID.0 requires */
static int ber_varbind_oid(uint8_t *out, int *pos, const uint32_t *arcs,
                           int narcs, const uint32_t *val, int nval)
{
    uint8_t body[256];
    int b = 0;

    ber_oid(body, &b, arcs, narcs);
    ber_oid(body, &b, val, nval);
    return ber_put(out, pos, 0x30, body, b);
}

static int ber_varbind_int(uint8_t *out, int *pos, const uint32_t *arcs,
                           int narcs, int32_t v)
{
    uint8_t body[128];
    int b = 0;

    ber_oid(body, &b, arcs, narcs);
    ber_int(body, &b, v);
    return ber_put(out, pos, 0x30, body, b);
}

static int ber_varbind_timeticks(uint8_t *out, int *pos, const uint32_t *arcs,
                                 int narcs, uint32_t v)
{
    uint8_t body[128], num[4];
    int b = 0;

    num[0] = (uint8_t)(v >> 24);
    num[1] = (uint8_t)(v >> 16);
    num[2] = (uint8_t)(v >> 8);
    num[3] = (uint8_t)v;
    ber_oid(body, &b, arcs, narcs);
    ber_put(body, &b, 0x43, num, 4);         /* Timeticks */
    return ber_put(out, pos, 0x30, body, b);
}

/* sysUpTime.0 and snmpTrapOID.0 */
static const uint32_t OID_SYSUPTIME[]  = { 1,3,6,1,2,1,1,3,0 };
static const uint32_t OID_TRAPOID[]    = { 1,3,6,1,6,3,1,1,4,1,0 };
static const uint32_t OID_PET_TRAP[]   = { 1,3,6,1,4,1,3183,1,1,1 };          /* petTrap */
static const uint32_t OID_PET_HOSTNAME[] = { 1,3,6,1,4,1,3183,1,1,1,2,0 };
static const uint32_t OID_PET_DATETIME[] = { 1,3,6,1,4,1,3183,1,1,1,3,0 };
static const uint32_t OID_PET_SEQ[]      = { 1,3,6,1,4,1,3183,1,1,1,4,0 };
static const uint32_t OID_PET_EVTYPE[]   = { 1,3,6,1,4,1,3183,1,1,1,5,0 };
static const uint32_t OID_PET_SRCTYPE[]  = { 1,3,6,1,4,1,3183,1,1,1,7,0 };
static const uint32_t OID_PET_SEVERITY[] = { 1,3,6,1,4,1,3183,1,1,1,8,0 };
static const uint32_t OID_PET_SENSTYPE[] = { 1,3,6,1,4,1,3183,1,1,1,9,0 };
static const uint32_t OID_PET_SENSNUM[]  = { 1,3,6,1,4,1,3183,1,1,1,10,0 };
static const uint32_t OID_PET_EVTDATA[]  = { 1,3,6,1,4,1,3183,1,1,1,11,0 };
static const uint32_t OID_PET_DESC[]     = { 1,3,6,1,4,1,3183,1,1,1,14,0 };

static uint32_t g_pet_seq;

/* The management port is a slave of the vrf-pub VRF, so its routes live in
 * table 300 and the main table has none at all.  A trap socket that is not
 * bound to the VRF device therefore fails with ENETUNREACH -- the listener is
 * bound for exactly this reason (see S47is_ipmi_lan). */
static char g_bind_dev[IF_NAMESIZE];

/* one SEL record (16 bytes in our internal form) -> PET trap */
static void pet_send(const uint8_t *rec, int event_type)
{
    uint8_t pkt[1024], vbl[640], body[256], ent[64];
    int p = 0, pb = 0, vb = 0, i;
    char host[64] = "ubmc";
    char when[32];
    char desc[128];
    uint32_t uptime = (uint32_t)(now_ms() / 10);     /* 10 ms units */
    time_t ts = (time_t)rd_le32(rec + 3);
    struct tm tmv;

    for (i = 0; i < ALERT_MAX; i++) {
        struct sockaddr_in to;
        int one = 1, sock;

        if (!g_alert[i].enabled || g_alert[i].ip.s_addr == 0 ||
            g_alert[i].type != 1)
            continue;

        gethostname(host, sizeof(host));
        localtime_r(&ts, &tmv);
        snprintf(when, sizeof(when), "%02d%02d%02d%02d%02d%02d",
                 tmv.tm_year % 100, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        snprintf(desc, sizeof(desc),
                 "SEL %u: sensor type 0x%02x sensor %u event 0x%02x data %02x%02x%02x",
                 (unsigned)rec[0], rec[10], rec[11], rec[12],
                 rec[13], rec[14], rec[15]);

        vb = 0;
        ber_varbind_timeticks(vbl, &vb, OID_SYSUPTIME,
                             (int)(sizeof(OID_SYSUPTIME) / 4), uptime);
        ber_varbind_oid(vbl, &vb, OID_TRAPOID,
                        (int)(sizeof(OID_TRAPOID) / 4),
                        OID_PET_TRAP, (int)(sizeof(OID_PET_TRAP) / 4));
        ber_varbind_string(vbl, &vb, OID_PET_HOSTNAME,
                           (int)(sizeof(OID_PET_HOSTNAME) / 4), host);
        ber_varbind_string(vbl, &vb, OID_PET_DATETIME,
                           (int)(sizeof(OID_PET_DATETIME) / 4), when);
        ber_varbind_int(vbl, &vb, OID_PET_SEQ,
                        (int)(sizeof(OID_PET_SEQ) / 4), (int32_t)++g_pet_seq);
        ber_varbind_int(vbl, &vb, OID_PET_EVTYPE,
                        (int)(sizeof(OID_PET_EVTYPE) / 4), event_type);
        ber_varbind_int(vbl, &vb, OID_PET_SRCTYPE,
                        (int)(sizeof(OID_PET_SRCTYPE) / 4), 1);      /* IPMI SEL */
        ber_varbind_string(vbl, &vb, OID_PET_SEVERITY,
                           (int)(sizeof(OID_PET_SEVERITY) / 4), "information");
        ber_varbind_int(vbl, &vb, OID_PET_SENSTYPE,
                        (int)(sizeof(OID_PET_SENSTYPE) / 4), rec[10]);
        ber_varbind_int(vbl, &vb, OID_PET_SENSNUM,
                        (int)(sizeof(OID_PET_SENSNUM) / 4), rec[11]);
        {
            uint8_t d[3];
            d[0] = rec[13];
            d[1] = rec[14];
            d[2] = rec[15];
            ber_varbind_octets(vbl, &vb, OID_PET_EVTDATA,
                               (int)(sizeof(OID_PET_EVTDATA) / 4), d, 3);
        }
        ber_varbind_string(vbl, &vb, OID_PET_DESC,
                           (int)(sizeof(OID_PET_DESC) / 4), desc);

        /* trap PDU: enterprise, agent-addr, generic, specific, timeticks,
         * varbinds */
        pb = 0;
        {
            uint32_t arcs[] = { 1,3,6,1,4,1,3183 };
            int n = oid_encode(arcs, 6, ent);
            ber_put(body, &pb, 0x06, ent, n);
        }
        {
            uint8_t ipb[4] = { 0, 0, 0, 0 };
            ber_put(body, &pb, 0x40, ipb, 4);       /* agent-addr */
        }        ber_int(body, &pb, 6);                      /* generic-trap: enterprise */
        ber_int(body, &pb, 1);                      /* specific-trap: PET */
        {
            uint8_t tb[4];
            tb[0] = (uint8_t)(uptime >> 24);
            tb[1] = (uint8_t)(uptime >> 16);
            tb[2] = (uint8_t)(uptime >> 8);
            tb[3] = (uint8_t)uptime;
            ber_put(body, &pb, 0x43, tb, 4);        /* time-stamp */
        }
        ber_put(body, &pb, 0x30, vbl, vb);          /* varbind list */
        {
            uint8_t pdu[768];
            uint8_t inner[896];
            int pp = 0, ib = 0, pl = 0;

            ber_put(pdu, &pp, 0xA4, body, pb);      /* [4] trap PDU */
            ber_int(inner, &ib, 0);                 /* version: SNMPv1 */
            ber_put(inner, &ib, 0x04, (const uint8_t *)g_lan.community,
                      (int)strlen(g_lan.community));
            memcpy(inner + ib, pdu, (size_t)pp);
            ib += pp;
            ber_put(pkt, &pl, 0x30, inner, ib);     /* outer SEQUENCE */
            p = pl;
        }

        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
            continue;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (g_bind_dev[0] != '\0' &&
            setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE,
                       g_bind_dev, (socklen_t)(strlen(g_bind_dev) + 1)) != 0)
            logmsg("pet: cannot bind to %s: %s", g_bind_dev, strerror(errno));
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(162);
        to.sin_addr = g_alert[i].ip;
        if (sendto(sock, pkt, (size_t)p, 0,
                   (struct sockaddr *)&to, sizeof(to)) < 0)
            logmsg("pet: send to %s failed: %s",
                   inet_ntoa(g_alert[i].ip), strerror(errno));
        else
            logmsg("pet: sent %d bytes to %s:162", p, inet_ntoa(g_alert[i].ip));
        close(sock);
    }
}

/* --------------------------------------------------- SEL -> PEF actions */
/*
 * The SEL is a flat file the vendor daemon writes, pre-allocated so that every
 * 20-byte slot already looks like a valid record.  Counting records therefore
 * never changes, so "is there something new" has to be decided on the
 * timestamp of the newest record instead.
 */
static uint32_t g_sel_last_ts;
static int      g_sel_primed;

static void pef_event(const uint8_t *rec)
{
    char desc[160];

    snprintf(desc, sizeof(desc),
             "SEL event id %u: sensor type 0x%02x, sensor %u, "
             "event 0x%02x, data %02x %02x %02x",
             (unsigned)rec[0], rec[10], rec[11], rec[12],
             rec[13], rec[14], rec[15]);

    syslog(LOG_WARNING, "%s", desc);
    logmsg("pef: %s", desc);

    if (!(g_pef.global & 0x01)) {           /* alert action disabled */
        logmsg("pef: alert action disabled, not sending a trap");
        return;
    }
    if (alert_count() == 0) {
        logmsg("pef: no alert destination configured");
        return;
    }
    pet_send(rec, 2);                       /* 2 = event */
}

/* Called from the idle tick.  stat() first: re-parsing the SEL out of the eMMC
 * every second is enough I/O to stall the select loop and make clients
 * retransmit (which shows up as duplicated RAKP4). */
static void pef_poll(void)
{
    static uint8_t recs[SEL_MAX * 16];
    static time_t last_mtime;
    static off_t  last_size;
    struct stat st;
    int n, i, fired = 0;
    uint32_t newest = 0;

    if (stat(SEL_FILE, &st) != 0)
        return;
    if (st.st_mtime == last_mtime && st.st_size == last_size)
        return;                             /* nothing written since last look */
    last_mtime = st.st_mtime;
    last_size  = st.st_size;

    n = sel_load(recs, SEL_MAX);
    if (n <= 0)
        return;
    for (i = 0; i < n; i++) {
        uint32_t ts = rd_le32(recs + i * 16 + 3);
        if (ts > newest)
            newest = ts;
    }
    if (!g_sel_primed) {                    /* first poll: adopt what is there */
        g_sel_primed = 1;
        g_sel_last_ts = newest;
        return;
    }
    if (newest <= g_sel_last_ts)
        return;

    for (i = 0; i < n && fired < 16; i++) {
        uint32_t ts = rd_le32(recs + i * 16 + 3);
        if (ts > g_sel_last_ts) {
            pef_event(recs + i * 16);
            fired++;
        }
    }
    g_sel_last_ts = newest;
}

static void lanparams_load(void)
{
    char line[160];
    FILE *f = fopen(LANPARAM_CONF, "r");

    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned a, b, c, d, e;
        char com[18];
        if (sscanf(line, "community=%17s", com) == 1) {
            snprintf(g_lan.community, sizeof(g_lan.community), "%s", com);
        } else if (sscanf(line, "auth_types=%u", &a) == 1) {
            g_lan.auth_types = (uint8_t)a;
        } else if (sscanf(line, "auth_enables=%u,%u,%u,%u,%u",
                          &a, &b, &c, &d, &e) == 5) {
            g_lan.auth_enables[0] = (uint8_t)a;
            g_lan.auth_enables[1] = (uint8_t)b;
            g_lan.auth_enables[2] = (uint8_t)c;
            g_lan.auth_enables[3] = (uint8_t)d;
            g_lan.auth_enables[4] = (uint8_t)e;
        } else if (sscanf(line, "arp_control=%u", &a) == 1) {
            g_lan.arp_control = (uint8_t)a;
        } else if (sscanf(line, "garp_interval=%u", &a) == 1) {
            g_lan.garp_interval = (uint8_t)a;
        }
    }
    fclose(f);
}

static void lanparams_save(void)
{
    char tmp[128];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", LANPARAM_CONF);
    f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fprintf(f, "community=%s\n", g_lan.community);
    fprintf(f, "auth_types=%u\n", g_lan.auth_types);
    fprintf(f, "auth_enables=%u,%u,%u,%u,%u\n",
            g_lan.auth_enables[0], g_lan.auth_enables[1],
            g_lan.auth_enables[2], g_lan.auth_enables[3],
            g_lan.auth_enables[4]);
    fprintf(f, "arp_control=%u\ngarp_interval=%u\n",
            g_lan.arp_control, g_lan.garp_interval);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    chmod(tmp, 0600);
    if (rename(tmp, LANPARAM_CONF) != 0) {
        logmsg("cannot replace %s: %s", LANPARAM_CONF, strerror(errno));
        remove(tmp);
    }
}

/* IPMI "IP Address Source": 0 unspecified, 1 static, 2 DHCP */
static int iface_ipv4_origin(const char *name)
{
    char key[192], val[32];
    struct stat st;

    if (*name == '\0')
        return 0;

    snprintf(key, sizeof(key),
             "/config/system/mgmt/interface-list/%s/ipv4-origin", name);
    if (conf_lookup(MGMTD_CONF, key, val, sizeof(val)) == 0) {
        if (strcmp(val, "dhcp") == 0)
            return 2;
        if (strcmp(val, "static") == 0)
            return 1;
        return 0;                       /* "none" and anything unknown */
    }

    /* no mgmtd config: fall back to the busybox DHCP client marker */
    snprintf(key, sizeof(key), "/var/run/udhcpc.%s.pid", name);
    return stat(key, &st) == 0 ? 2 : 1;
}

/* LAN configuration parameter numbers, as they go on the wire (ipmitool's
 * ipmi_lan.h uses the same values: 3 is the IP address, 20 the 802.1q ID) */
#define LANP_SET_IN_PROGRESS      0
#define LANP_AUTH_TYPE_SUPPORT    1
#define LANP_AUTH_TYPE_ENABLE     2
#define LANP_IP_ADDRESS           3
#define LANP_IP_ADDRESS_SOURCE    4
#define LANP_MAC_ADDRESS          5
#define LANP_SUBNET_MASK          6
#define LANP_IPV4_HEADER_PARAMS   7
#define LANP_PRIMARY_RMCP_PORT    8
#define LANP_SECONDARY_RMCP_PORT  9
#define LANP_BMC_ARP_CONTROL     10
#define LANP_GRATUITOUS_ARP      11
#define LANP_DEFAULT_GATEWAY     12
#define LANP_DEFAULT_GATEWAY_MAC 13
#define LANP_BACKUP_GATEWAY      14
#define LANP_BACKUP_GATEWAY_MAC  15
#define LANP_COMMUNITY_STRING    16
#define LANP_NUM_DESTINATIONS    17
#define LANP_8021Q_VLAN_ID       20
#define LANP_8021Q_VLAN_PRIORITY 21
#define LANP_RMCP_PLUS_SUPPORT   22
#define LANP_RMCP_PLUS_SUITES    23
#define LANP_RMCP_PLUS_PRIVS     24

/* build the parameter data; returns its length, or -1 when not supported */
static int lan_param(uint8_t *out, int max, uint8_t sel, uint8_t arg)
{
    char ifname[IF_NAMESIZE];
    struct in_addr ip, mask, gw;
    unsigned char mac[6];
    int len = 0;

    ip = g_local_ip;
    if (ip.s_addr == 0)
        return -1;                  /* no IP_PKTINFO: nothing to describe */

    memset(ifname, 0, sizeof(ifname));
    mask.s_addr = 0;
    memset(mac, 0, 6);
    iface_for_addr(ip, ifname, sizeof(ifname), &mask, mac);
    gw = iface_gateway(ifname);

    switch (sel) {
    case LANP_SET_IN_PROGRESS:
        out[len++] = 0x00;
        break;
    case LANP_AUTH_TYPE_SUPPORT:
        out[len++] = g_lan.auth_types;
        break;
    case LANP_AUTH_TYPE_ENABLE:
        memcpy(out + len, g_lan.auth_enables, 5);
        len += 5;
        break;
    case LANP_IP_ADDRESS:
        memcpy(out + len, &ip.s_addr, 4);
        len += 4;
        break;
    case LANP_IP_ADDRESS_SOURCE:
        out[len++] = (uint8_t)iface_ipv4_origin(ifname);
        break;
    case LANP_MAC_ADDRESS:
        memcpy(out + len, mac, 6);
        len += 6;
        break;
    case LANP_SUBNET_MASK:
        memcpy(out + len, &mask.s_addr, 4);
        len += 4;
        break;
    case LANP_IPV4_HEADER_PARAMS:
        out[len++] = 0x40;          /* TTL 64 */
        out[len++] = 0x40;          /* don't fragment */
        out[len++] = 0x00;
        break;
    case LANP_PRIMARY_RMCP_PORT:
        out[len++] = (uint8_t)(IPMI_PORT & 0xff);
        out[len++] = (uint8_t)(IPMI_PORT >> 8);
        break;
    case LANP_SECONDARY_RMCP_PORT:
        out[len++] = 0x98;          /* 664, the IPMI default */
        out[len++] = 0x02;
        break;
    case LANP_BMC_ARP_CONTROL:
        out[len++] = g_lan.arp_control;
        break;
    case LANP_GRATUITOUS_ARP:
        out[len++] = g_lan.garp_interval;
        break;
    case LANP_DEFAULT_GATEWAY: {
        /* prefer the configuration tree: this is where a Set LAN Config write
         * puts the value, and /proc/net/route only ever shows the main table,
         * never the VRF table the management port actually routes through */
        char val[32];
        gw.s_addr = 0;
        if (conf_lookup(MGMTD_CONF, "/config/system/mgmt/default-gateway",
                        val, sizeof(val)) == 0) {
            struct in_addr conf_gw;
            if (inet_aton(val, &conf_gw) != 0)
                gw = conf_gw;
        }
        memcpy(out + len, &gw.s_addr, 4);
        len += 4;
        break;
    }
    case LANP_BACKUP_GATEWAY:
        memset(out + len, 0, 4);
        len += 4;
        break;
    case LANP_DEFAULT_GATEWAY_MAC:
    case LANP_BACKUP_GATEWAY_MAC:
        memset(out + len, 0, 6);    /* ARP cache lookup is not worth it */
        len += 6;
        break;
    case LANP_COMMUNITY_STRING:
        /* fixed 18 bytes, NUL padded; this is the community PET uses */
        memset(out + len, 0, 18);
        memcpy(out + len, g_lan.community, strlen(g_lan.community));
        len += 18;
        break;
    case LANP_NUM_DESTINATIONS:
        out[len++] = (uint8_t)alert_count();
        break;
    case 18:                                    /* Alert Destination Type */
    case 19: {                                  /* Alert Destination Address */
        /* both are per-destination: `arg` is the destination selector that
         * followed the parameter selector in the request */
        const alert_dest_t *d;
        if (arg < 1 || arg > ALERT_MAX) {
            return -1;
        }
        d = &g_alert[arg - 1];
        out[len++] = arg;                       /* echo the selector */
        if (sel == 18) {
            out[len++] = d->type;
            out[len++] = (uint8_t)((d->ack_timeout & 0x3f) |
                                   ((d->retries & 0x03) << 6));
            out[len++] = d->addr_format;
        } else {
            memcpy(out + len, &d->ip.s_addr, 4);
            len += 4;
        }
        break;
    }
    case LANP_8021Q_VLAN_ID: {
        char key[192], val[32];
        unsigned vlan = 0;
        snprintf(key, sizeof(key),
                 "/config/system/mgmt/interface-list/%s/vlan", ifname);
        if (conf_lookup(MGMTD_CONF, key, val, sizeof(val)) == 0)
            vlan = (unsigned)strtoul(val, NULL, 10);
        out[len++] = (uint8_t)(vlan & 0xff);
        out[len++] = (uint8_t)((vlan >> 8) & 0xff);
        break;
    }
    case LANP_8021Q_VLAN_PRIORITY:
        out[len++] = 0x00;
        break;
    case LANP_RMCP_PLUS_SUPPORT:
        out[len++] = 0x08;          /* one set of three algorithms */
        break;
    case LANP_RMCP_PLUS_SUITES:
        out[len++] = 0x02;          /* two entries */
        out[len++] = 0x03;          /* suite 3:  SHA-1 + AES-CBC-128 */
        out[len++] = 0x11;          /* suite 17: SHA-256 + AES-CBC-128 */
        break;
    case LANP_RMCP_PLUS_PRIVS:
        out[len++] = 0x04;          /* suite 3:  up to admin */
        out[len++] = 0x04;          /* suite 17: up to admin */
        break;
    default:
        return -1;
    }
    return len <= max ? len : -1;
}

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

        case 0x42: {                                /* Get Channel Info */
            uint8_t ch = (reqlen - 7) > 0 ? (uint8_t)(req[6] & 0x0f) : 0;

            /* 0x0e is "the channel this request arrived on"; the LAN channel is
             * 1, and everything else is reported as unimplemented. */
            if (ch == 0x0e || ch == 0x0f)
                ch = 1;
            if (ch != 1) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rsp[n++] = 0x01;      /* channel number, low nibble */
            rsp[n++] = 0x04;      /* medium type: 802.3 LAN, low 7 bits */
            rsp[n++] = 0x00;      /* protocol type: reserved */
            rsp[n++] = 0x80;      /* multi-session support, 0 active */
            rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;  /* vendor ID */
            rsp[n++] = 0x00; rsp[n++] = 0x00;   /* auxiliary channel info */
            break;
        }

        case 0x41:                                  /* Get Channel Access */
            /* Two data bytes only -- ipmitool rejects anything longer.
             * byte 1: access mode 0x02 (always available) | 0x20 user level
             *         auth | 0x10 per-message auth
             * byte 2: channel privilege limit (4 = administrator) */
            rsp[n++] = 0x32;
            rsp[n++] = 0x04;
            break;

        case 0x40:                                  /* Set Channel Access */
            /* accepted and ignored: there is no real ACL to change */
            break;

        case 0x54: {                                /* Get Channel Cipher Suites */
            /* Standard records are four bytes: suite id, auth, integrity,
             * crypt.  The list index counts *records*, and an index past the
             * end must come back as an empty list -- returning 0xCC there is
             * what makes ipmitool print "Unable to Get Channel Cipher
             * Suites" on every single command. */
            static const uint8_t suites[] = {
                0x00, 0x00, 0x00, 0x00,   /* 0: none/none/none       */
                0x01, 0x01, 0x01, 0x00,   /* 1: SHA1 / SHA1-96 / none */
                0x03, 0x01, 0x01, 0x01,   /* 3: SHA1 / SHA1-96 / AES  */
                0x11, 0x03, 0x04, 0x01,   /* 17: SHA256 / SHA256-128 / AES */
                0x12, 0x03, 0x05, 0x01,   /* 18: SHA256 / SHA256-256 / AES */
                0x13, 0x03, 0x00, 0x01,   /* 19: SHA256 / none / AES  */
            };
            uint8_t ch = (reqlen - 7) > 0 ? (uint8_t)(req[6] & 0x0f) : 1;
            int     idx = (reqlen - 7) > 2 ? req[8] : 0;
            int     nrec = (int)sizeof(suites) / 4;
            int     from = idx * 16;            /* 16 records per response */
            int     i;

            rsp[n++] = ch;
            for (i = from; i < nrec && i < from + 16; i++) {
                memcpy(rsp + n, suites + i * 4, 4);
                n += 4;
            }
            break;
        }

        case 0x44: {                                /* Get User Access */
            uint8_t uid = (reqlen - 7) >= 2 ? (uint8_t)(req[7] & 0x3f) : 1;
            ipmi_user_t *u = user_by_id(uid);
            int enabled_count = 0, i;

            if (uid < 1 || uid > MAX_USERS) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            for (i = 0; i < g_nusers; i++)
                if (g_users[i].enabled)
                    enabled_count++;

            /* Layout as ipmitool reads it: byte 3 is where both the
             * enable flags (bits 7:4) and the privilege limit (bits 3:0)
             * live, which is why it prints "Channel Priv Limit: USER" when
             * the privilege is put in byte 2 instead. */
            rsp[n++] = MAX_USERS;                        /* max user ids */
            rsp[n++] = (uint8_t)enabled_count;           /* enabled user ids */
            rsp[n++] = 0x00;                             /* fixed user ids */
            /* 0x20 link auth + 0x10 IPMI messaging; the callin bit (0x40)
             * is active-low in ipmitool's display, so leave it clear */
            rsp[n++] = (uint8_t)((u ? u->priv : 0) |
                                 (u && u->enabled ? 0x30 : 0x00));
            break;
        }

        case 0x43: {                                /* Set User Access */
            uint8_t uid, priv;
            ipmi_user_t *u;

            if ((reqlen - 7) < 3) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            uid  = (uint8_t)(req[7] & 0x3f);
            priv = (uint8_t)(req[8] & 0x0f);
            if (uid < 1 || uid > MAX_USERS || priv < 1 || priv > 5) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            u = user_slot(uid, 1);
            if (!u) {
                rsp[6] = CC_INVALID_DATA;           /* all slots used */
                break;
            }
            /* byte 3 carries the per-link permissions (callin / link auth /
             * IPMI messaging), not the account enable bit -- that one is
             * driven by Set User Password op 1/2.  We have no per-link ACLs,
             * so only the privilege limit is stored. */
            u->priv = priv;
            save_users();
            logmsg("user %u (%s): priv %u", uid, u->name, priv);
            break;
        }

        case 0x46: {                                /* Get User Name */
            /* ipmitool sends a single user-id byte; accept the two-byte
             * channel+id form as well so other clients work too. */
            uint8_t uid = (reqlen - 7) >= 2 ? (uint8_t)(req[7] & 0x3f)
                        : (reqlen - 7) == 1 ? (uint8_t)(req[6] & 0x3f) : 0;
            ipmi_user_t *u;
            int i;

            if (uid < 1 || uid > MAX_USERS) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            u = user_by_id(uid);
            for (i = 0; i < 16; i++)
                rsp[n++] = (u && i < (int)strlen(u->name)) ? (uint8_t)u->name[i] : 0x00;
            break;
        }

        case 0x45: {                                /* Set User Name */
            uint8_t uid = (reqlen - 7) >= 17 ? (uint8_t)(req[6] & 0x3f) : 0;
            ipmi_user_t *u;
            char name[17];
            int i;

            if (uid < 1 || uid > MAX_USERS || (reqlen - 7) < 16) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if ((u = user_slot(uid, 1)) == NULL) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            for (i = 0; i < 16; i++)
                name[i] = (char)req[7 + i];
            name[16] = '\0';
            trim(name);
            snprintf(u->name, sizeof(u->name), "%s", name);
            save_users();
            logmsg("user %u renamed to '%s'", uid, u->name);
            break;
        }

        case 0x47: {                                /* Set User Password */
            /* data: user id, operation, password
             * operation 0 = disable user, 1 = enable user,
             *           2 = set password, 3 = test password */
            uint8_t uid = (reqlen - 7) >= 1 ? req[6] : 0;
            uint8_t op  = (reqlen - 7) >= 2 ? req[7] : 0;
            int     len = reqlen - 9;               /* minus uid, op and checksum */
            ipmi_user_t *u;

            if (uid < 1 || uid > MAX_USERS) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if ((u = user_slot(uid, 1)) == NULL) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (len > 20)
                len = 20;
            if (op == 2) {                          /* set password */
                if (len < 1) {
                    rsp[6] = CC_INVALID_DATA;
                    break;
                }
                memset(u->pass, 0, sizeof(u->pass));
                memcpy(u->pass, req + 8, (size_t)len);
                save_users();
                logmsg("user %u (%s): password updated", uid, u->name);
            } else if (op == 1) {                   /* enable user */
                u->enabled = 1;
                save_users();
                logmsg("user %u (%s): enabled", uid, u->name);
            } else if (op == 0) {                   /* disable user */
                u->enabled = 0;
                save_users();
                logmsg("user %u (%s): disabled", uid, u->name);
            } else {                                /* op 3: test password */
                if (len != (int)strlen(u->pass) ||
                    memcmp(u->pass, req + 8, (size_t)len) != 0)
                    rsp[6] = 0x80;                  /* password mismatch */
            }
            break;
        }

        case 0x3B:                                  /* Set Session Privilege Level */
            if ((reqlen - 7) < 1 || req[6] > 4) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (s) {
                /* an account may not raise itself above its configured maximum;
                 * without this check a 3 (operator) account could simply ask for
                 * 4 (administrator) and get it */
                uint8_t limit = 4;
                ipmi_user_t *u = user_by_name(s->user);
                if (u != NULL)
                    limit = u->priv;
                if (req[6] > limit) {
                    logmsg("set-priv: '%s' asked for %u, limited to %u",
                           s->user, req[6], limit);
                    rsp[6] = 0x81;                  /* requested level not available */
                    break;
                }
                s->priv = req[6];
            }
            rsp[n++] = 4;
            break;

        case 0x3C:                                  /* Close Session */
            /* Acknowledge before tearing the session down: the reply has to go
             * out on the very session being closed. */
            if (s)
                s->close_pending = 1;
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

        case 0x48: {                                /* Activate Payload */
            int ptype, rc;

            if ((reqlen - 7) < 1) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            ptype = req[6] & 0x0f;
            if (ptype != PT_SOL || s == NULL) {
                rsp[6] = 0x81;                      /* payload type unsupported */
                break;
            }
            rc = sol_activate(s);
            if (rc != 0) {
                rsp[6] = (uint8_t)rc;
                break;
            }
            rsp[n++] = 0x00; rsp[n++] = 0x00;       /* reserved / test mode */
            rsp[n++] = 0x00; rsp[n++] = 0x00;
            rsp[n++] = SOL_MAX_OUT; rsp[n++] = 0x00;    /* max outbound */
            rsp[n++] = 0x40; rsp[n++] = 0x00;           /* max inbound */
            rsp[n++] = (uint8_t)(IPMI_PORT & 0xff);     /* SOL lands on 623 */
            rsp[n++] = (uint8_t)((IPMI_PORT >> 8) & 0xff);
            rsp[n++] = 0x00; rsp[n++] = 0x00;           /* VLAN */
            break;
        }

        case 0x49:                                  /* Deactivate Payload */
            if (s != NULL)
                sol_deactivate(s);
            break;

        case 0x4A:                                  /* Get Payload Activation Status */
            rsp[n++] = (uint8_t)(g_sol_owner ? 0x01 : 0x00);
            rsp[n++] = 0x00;
            rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
            break;

        case 0x4B: {                                /* Get Payload Instance Info */
            uint32_t id = g_sol_owner ? g_sol_owner->rm_id : 0;
            rsp[n++] = (uint8_t)(id & 0xff);
            rsp[n++] = (uint8_t)((id >> 8) & 0xff);
            rsp[n++] = (uint8_t)((id >> 16) & 0xff);
            rsp[n++] = (uint8_t)((id >> 24) & 0xff);
            rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
            rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
            break;
        }

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
            /* power/reset needs operator (3) or above; a session authenticated
             * as a lower-privilege account must not be able to cycle the host */
            if (s != NULL && s->priv < 3) {
                logmsg("chassis control refused: '%s' has privilege %u",
                       s->user, s->priv);
                rsp[6] = 0x80;                      /* insufficient privilege */
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

        case 0x08: {                                /* Set System Boot Options */
            if ((reqlen - 7) < 2) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (req[6] != 5) {
                /* clients disable the boot-options timer first (parameter 3);
                 * accept anything else and simply do not store it */
                break;
            }
            g_boot_flags = req[7];
            g_boot_dev   = req[8];
            if (g_boot_dev == 0)
                g_boot_flags = 0;                   /* default = no override */
            boot_flags_save();
            logmsg("boot flags set: flags=%02x dev=%02x", g_boot_flags, g_boot_dev);
            break;
        }

        case 0x09: {                                /* Get System Boot Options */
            if ((reqlen - 7) < 1) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (req[6] != 5) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            boot_flags_load();
            rsp[n++] = 0x01;                        /* parameter revision */
            rsp[n++] = 0x05;                        /* selector, no channel */
            if (g_boot_dev == 0) {
                /* bit 7 set means "no override in effect"; pyghmi's
                 * get_bootdev() maps that to {'bootdev': 'default'} */
                rsp[n++] = 0x80;
                rsp[n++] = 0x00;
            } else {
                rsp[n++] = (uint8_t)(g_boot_flags & 0x7f);
                rsp[n++] = g_boot_dev;
            }
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            break;
        }

        default:
            rsp[6] = CC_INVALID_CMD;
            break;
        }
        break;

    case NETFN_SENSOR:
        switch (cmd) {
        case 0x10:                              /* Get PEF Capabilities */
            rsp[n++] = 0x51;   /* PEF version 1.5 ("0x51") */
            rsp[n++] = 0x11;   /* supports alert + power-down actions */
            rsp[n++] = 0x00;   /* event filter table entries */
            rsp[n++] = 0x00;   /* alert policy table entries */
            rsp[n++] = 0x00;   /* alert strings */
            break;

        case 0x11:                              /* Arm PEF Postpone Timer */
            break;                              /* accepted, no timer needed */

        case 0x13: {                            /* Get PEF Config Param */
            uint8_t sel = (reqlen - 7) > 0 ? req[6] : 0;

            rsp[n++] = 0x11;                    /* parameter revision */
            switch (sel) {
            case 1:  rsp[n++] = 0x51; break;                  /* PEF version */
            case 2:  rsp[n++] = g_pef.global; break;
            case 3:  rsp[n++] = g_pef.percontrol; break;
            case 4:  rsp[n++] = g_pef.startup; break;
            case 5:  rsp[n++] = g_pef.alert_startup; break;
            case 6:  case 7:  case 8:
            case 9:  case 10: case 11:
                rsp[n++] = 0x00;                /* table sizes: none */
                break;
            case 12: rsp[n++] = 0x01; break;     /* action support: alert */
            default:
                n--;                            /* drop the revision byte */
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            break;
        }

        case 0x12: {                            /* Set PEF Config Param */
            uint8_t sel = (reqlen - 7) > 0 ? req[6] : 0;
            uint8_t val = (reqlen - 7) > 1 ? req[8] : 0;

            switch (sel) {
            case 1:  break;                     /* version is read-only */
            case 2:  g_pef.global = val;        break;
            case 3:  g_pef.percontrol = val;    break;
            case 4:  g_pef.startup = val;       break;
            case 5:  g_pef.alert_startup = val; break;
            default:
                rsp[6] = CC_INVALID_DATA;       /* table sizes are ours */
                break;
            }
            if (rsp[6] == CC_OK) {
                pef_save();
                logmsg("pef: param %u -> 0x%02x", sel, val);
            }
            break;
        }

        case 0x14:                              /* Get Last Processed Event ID */
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            rsp[n++] = 0x00;
            break;

        case 0x15:                              /* Set Last Processed Event ID */
            break;

        case 0x16: {                            /* Alert Immediate */
            /* send one test PET to the given destination -- this is the
             * standard "does my trap receiver work" hook */
            uint8_t dsel = (reqlen - 7) > 0 ? req[6] : 0;
            uint8_t rec[16];

            if (dsel < 1 || dsel > ALERT_MAX ||
                !g_alert[dsel - 1].enabled ||
                g_alert[dsel - 1].ip.s_addr == 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            memset(rec, 0, sizeof(rec));
            rec[0] = 0x00;                     /* record id */
            rec[10] = 0x37;                    /* sensor type: system firmware */
            rec[11] = 0x00;                    /* sensor number */
            rec[12] = 0x6f;                    /* event dir/type */
            wr_le32(rec + 3, (uint32_t)time(NULL));
            logmsg("pef: alert immediate to destination %u", dsel);
            pet_send(rec, 3);                  /* 3 = test */
            break;
        }

        case 0x2D: {                                /* Get Sensor Reading */
            int i, raw, st = 0;
            long v;

            if ((reqlen - 7) < 1) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            for (i = 0; i < g_nsensors; i++)
                if (g_sensors[i].num == req[6])
                    break;
            if (i == g_nsensors) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (read_milli(g_sensors[i].path, &v) != 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            raw = (int)(v / g_sensors[i].divisor);
            if (raw < 0)
                raw = 0;
            if (raw > 255)
                raw = 255;
            if ((g_sensors[i].thr_mask & 0x10) && raw >= g_sensors[i].uc)
                st |= 0x10;                         /* upper critical */
            if ((g_sensors[i].thr_mask & 0x08) && raw >= g_sensors[i].unc)
                st |= 0x08;                         /* upper non-critical */
            rsp[n++] = (uint8_t)raw;
            rsp[n++] = 0xc0;                        /* scanning + events on */
            rsp[n++] = (uint8_t)st;
            break;
        }

        case 0x23: {                                /* Get Sensor Reading Factors */
            int i;

            if ((reqlen - 7) < 1) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            for (i = 0; i < g_nsensors; i++)
                if (g_sensors[i].num == req[6])
                    break;
            if (i == g_nsensors) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            /* every sensor here is linear with a fixed M, so the factors are
             * the same as the ones published in the SDR */
            rsp[n++] = 0x00;                        /* next reading */
            rsp[n++] = (uint8_t)(g_sensors[i].mult & 0xff);         /* M LSB */
            rsp[n++] = (uint8_t)(((g_sensors[i].mult >> 8) & 0x03) << 6);
            rsp[n++] = 0x00; rsp[n++] = 0x00;       /* B */
            rsp[n++] = 0x00;                        /* R + B exponent */
            rsp[n++] = 0x00; rsp[n++] = 0x00;       /* accuracy */
            rsp[n++] = 0x00;                        /* R exponent */
            break;
        }

        default:
            rsp[6] = CC_INVALID_CMD;
            break;
        }
        break;

    case NETFN_STORAGE:
        switch (cmd) {
        case 0x10: {                                /* Get FRU Area Info */
            if ((reqlen - 7) < 1 || req[6] != 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rsp[n++] = (uint8_t)(g_frulen & 0xff);
            rsp[n++] = (uint8_t)(g_frulen >> 8);
            rsp[n++] = 0x00;                        /* byte-wise access */
            break;
        }

        case 0x11: {                                /* Read FRU Data */
            int off, want, avail;

            if ((reqlen - 7) < 4 || req[6] != 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            off  = req[7] | (req[8] << 8);
            want = req[9];
            if (off >= g_frulen) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            avail = g_frulen - off;
            if (want > avail)
                want = avail;
            if (want > 64)
                want = 64;
            rsp[n++] = (uint8_t)want;
            memcpy(rsp + n, g_fru + off, (size_t)want);
            n += want;
            break;
        }

        case 0x20: {                                /* Get SDR Repository Info */
            int total = sdr_rec_count();
            int freeb = (SDR_USER_MAX - g_nusdr) * SDR_REC_MAX;
            if (freeb > 0xffff)
                freeb = 0xffff;
            rsp[n++] = 0x51;
            rsp[n++] = (uint8_t)(total & 0xff);
            rsp[n++] = (uint8_t)(total >> 8);
            rsp[n++] = (uint8_t)(freeb & 0xff);
            rsp[n++] = (uint8_t)(freeb >> 8);
            rsp[n++] = (uint8_t)(g_sdr_add_ts & 0xff);      /* last addition */
            rsp[n++] = (uint8_t)((g_sdr_add_ts >> 8) & 0xff);
            rsp[n++] = (uint8_t)((g_sdr_add_ts >> 16) & 0xff);
            rsp[n++] = (uint8_t)((g_sdr_add_ts >> 24) & 0xff);
            rsp[n++] = (uint8_t)(g_sdr_erase_ts & 0xff);    /* last erase */
            rsp[n++] = (uint8_t)((g_sdr_erase_ts >> 8) & 0xff);
            rsp[n++] = (uint8_t)((g_sdr_erase_ts >> 16) & 0xff);
            rsp[n++] = (uint8_t)((g_sdr_erase_ts >> 24) & 0xff);
            /* reserve + partial add + alloc info + delete are all implemented */
            rsp[n++] = 0x0f;
            break;
        }

        case 0x21: {                                /* Get SDR Repository Allocation Info */
            int info = ((reqlen - 7) >= 3) ? req[8] : 0;
            int total = SDR_USER_MAX;
            int used  = g_nusdr * SDR_REC_MAX;

            if (info == 0) {                        /* allocation units */
                rsp[n++] = (uint8_t)(total & 0xff);
                rsp[n++] = (uint8_t)(total >> 8);
                rsp[n++] = (uint8_t)(SDR_REC_MAX & 0xff);
                rsp[n++] = (uint8_t)(SDR_REC_MAX >> 8);
            } else if (info == 1) {                 /* bytes used */
                rsp[n++] = (uint8_t)(used & 0xff);
                rsp[n++] = (uint8_t)((used >> 8) & 0xff);
                rsp[n++] = (uint8_t)((used >> 16) & 0xff);
                rsp[n++] = (uint8_t)((used >> 24) & 0xff);
                rsp[n++] = (uint8_t)(SDR_REC_MAX & 0xff);
                rsp[n++] = (uint8_t)(SDR_REC_MAX >> 8);
            } else if (info == 2) {                 /* bytes free */
                int freeb = (SDR_USER_MAX - g_nusdr) * SDR_REC_MAX;
                rsp[n++] = (uint8_t)(freeb & 0xff);
                rsp[n++] = (uint8_t)((freeb >> 8) & 0xff);
                rsp[n++] = (uint8_t)((freeb >> 16) & 0xff);
                rsp[n++] = (uint8_t)((freeb >> 24) & 0xff);
                rsp[n++] = (uint8_t)(SDR_REC_MAX & 0xff);
                rsp[n++] = (uint8_t)(SDR_REC_MAX >> 8);
            } else {
                rsp[6] = CC_INVALID_DATA;
            }
            break;
        }

        case 0x22:                                  /* Reserve SDR Repository */
            rsp[n++] = 0x00; rsp[n++] = 0x00;
            break;

        case 0x23: {                                /* Get SDR */
            int idx, off, want, avail, chunk, total = sdr_rec_count();
            const uint8_t *rec;
            int reclen = 0;
            uint16_t rid, next;

            if ((reqlen - 7) < 6) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rid  = (uint16_t)(req[8] | (req[9] << 8));
            off  = req[10];
            want = req[11];
            if (rid == 0x0000 || rid == 0xffff)
                rid = sdr_id_of(0);                 /* first record */
            idx = sdr_idx_of(rid);
            if (idx < 0 || want == 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rec = sdr_rec_of(idx, &reclen);
            if (rec == NULL) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            avail = reclen - off;
            if (avail <= 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            chunk = want < avail ? want : avail;
            next  = (idx + 1 < total) ? sdr_id_of(idx + 1) : 0xffff;
            rsp[n++] = (uint8_t)(next & 0xff);
            rsp[n++] = (uint8_t)(next >> 8);
            memcpy(rsp + n, rec + off, (size_t)chunk);
            n += chunk;
            break;
        }

        case 0x24: {                                /* Add SDR */
            int len = reqlen - 7;                   /* last byte is the checksum */
            int slot;

            if (len < 5) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (len > SDR_REC_MAX)
                len = SDR_REC_MAX;
            slot = sdr_user_add(req + 6, len);
            if (slot < 0) {
                rsp[6] = 0x80;                      /* repository full */
                break;
            }
            /* the BMC assigns the record ID, whatever the host put in bytes 0-1 */
            g_usdr[slot][0] = (uint8_t)((SDR_USER_ID0 + slot) & 0xff);
            g_usdr[slot][1] = (uint8_t)((SDR_USER_ID0 + slot) >> 8);
            g_sdr_add_ts = (uint32_t)time(NULL);
            sdr_user_save();
            rsp[n++] = g_usdr[slot][0];
            rsp[n++] = g_usdr[slot][1];
            break;
        }

        case 0x25: {                                /* Partial Add SDR */
            int off, more, len, slot;

            if ((reqlen - 7) < 5) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            off  = req[8];
            more = req[9];
            len  = reqlen - 11;                     /* 4 header bytes + checksum */
            if (more != 0 && off == 0) {            /* first fragment */
                g_sdr_stage_len = 0;
                g_sdr_stage_off = 0;
            }
            if (off != g_sdr_stage_off) {           /* pieces must be contiguous */
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (g_sdr_stage_len + len > SDR_REC_MAX) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            memcpy(g_sdr_stage + g_sdr_stage_len, req + 10, (size_t)len);
            g_sdr_stage_len += len;
            g_sdr_stage_off += len;
            if (more != 0) {                        /* more to come */
                rsp[n++] = 0x00; rsp[n++] = 0x00;   /* no record ID yet */
                break;
            }
            slot = sdr_user_add(g_sdr_stage, g_sdr_stage_len);
            if (slot < 0) {
                rsp[6] = 0x80;
                break;
            }
            g_usdr[slot][0] = (uint8_t)((SDR_USER_ID0 + slot) & 0xff);
            g_usdr[slot][1] = (uint8_t)((SDR_USER_ID0 + slot) >> 8);
            g_sdr_add_ts = (uint32_t)time(NULL);
            sdr_user_save();
            rsp[n++] = g_usdr[slot][0];
            rsp[n++] = g_usdr[slot][1];
            break;
        }

        case 0x26: {                                /* Delete SDR */
            int rid, idx;

            if ((reqlen - 7) < 2) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rid = req[6] | (req[7] << 8);
            idx = rid - SDR_USER_ID0;
            if (idx < 0 || idx >= g_nusdr) {
                /* the generated sensor records cannot be deleted */
                rsp[6] = 0xcb;                      /* record not found */
                break;
            }
            memmove(g_usdr[idx], g_usdr[idx + 1],
                    (size_t)((g_nusdr - idx - 1) * SDR_REC_MAX));
            memmove(&g_usdr_len[idx], &g_usdr_len[idx + 1],
                    (size_t)((g_nusdr - idx - 1) * sizeof(g_usdr_len[0])));
            g_nusdr--;
            sdr_user_save();
            break;
        }

        case 0x27: {                                /* Clear SDR Repository */
            if ((reqlen - 7) < 5) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (!(req[8] == 'C' && req[9] == 'L' && req[10] == 'R' &&
                  (req[11] & 0x0f) == 0x0a)) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            g_nusdr = 0;
            g_sdr_erase_ts = (uint32_t)time(NULL);
            sdr_user_save();
            break;
        }

        case 0x28: {                                /* Get SDR Repository Time */
            uint32_t t = (uint32_t)time(NULL);
            rsp[n++] = (uint8_t)(t & 0xff);
            rsp[n++] = (uint8_t)((t >> 8) & 0xff);
            rsp[n++] = (uint8_t)((t >> 16) & 0xff);
            rsp[n++] = (uint8_t)((t >> 24) & 0xff);
            break;
        }

        case 0x29:                                  /* Set SDR Repository Time */
        case 0x2a:                                  /* Enter Update Mode */
        case 0x2b:                                  /* Exit Update Mode */
        case 0x2c:                                  /* Run Initialization Agent */
            /* nothing to do: the repository the host writes lives in /config,
             * so there is no update modal to enter or leave */
            break;

        case 0x40: {                                /* Get SEL Info */
            int cnt = sel_load(g_selwork, SEL_MAX);
            uint32_t ts = sel_last_timestamp();

            rsp[n++] = 0x51;                        /* SEL revision */
            rsp[n++] = (uint8_t)(cnt & 0xff);
            rsp[n++] = (uint8_t)(cnt >> 8);
            rsp[n++] = 0x00; rsp[n++] = 0x00;       /* free space: full */
            rsp[n++] = (uint8_t)(ts & 0xff);
            rsp[n++] = (uint8_t)((ts >> 8) & 0xff);
            rsp[n++] = (uint8_t)((ts >> 16) & 0xff);
            rsp[n++] = (uint8_t)((ts >> 24) & 0xff);
            rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0;
            rsp[n++] = 0x02;                        /* reserve supported */
            break;
        }

        case 0x42:                                  /* Reserve SEL */
            rsp[n++] = 0x01; rsp[n++] = 0x00;
            break;

        case 0x43: {                                /* Get SEL Entry */
            int off, want, chunk, cnt, i, idx = -1;
            uint16_t rid, next;

            if ((reqlen - 7) < 6) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rid  = (uint16_t)(req[8] | (req[9] << 8));
            off  = req[10];
            want = req[11];
            cnt  = sel_load(g_selwork, SEL_MAX);
            if (cnt == 0) {
                next = 0xffff;
                rsp[n++] = (uint8_t)(next & 0xff);
                rsp[n++] = (uint8_t)(next >> 8);
                rsp[n++] = 0xff; rsp[n++] = 0xff;   /* no more records */
                rsp[n++] = 0;
                rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0;
                rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0;
                rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0;
                rsp[n++] = 0; rsp[n++] = 0;
                break;
            }
            for (i = 0; i < cnt; i++) {
                if (rid == 0x0000 || rid == 0xffff) {
                    idx = 0;
                    break;
                }
                if (g_selwork[i * 16] == (rid & 0xff) &&
                    g_selwork[i * 16 + 1] == (rid >> 8)) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (off >= 16) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            chunk = 16 - off;
            if (want < chunk)
                chunk = want;
            next = (uint16_t)((idx + 1 < cnt) ? g_selwork[(idx + 1) * 16] : 0xffff);
            rsp[n++] = (uint8_t)(next & 0xff);
            rsp[n++] = (uint8_t)(next >> 8);
            memcpy(rsp + n, g_selwork + idx * 16 + off, (size_t)chunk);
            n += chunk;
            break;
        }

        case 0x47:                                  /* Clear SEL */
            /* erasing the event log is an operator/admin action */
            if (s != NULL && s->priv < 3) {
                logmsg("clear SEL refused: '%s' has privilege %u", s->user, s->priv);
                rsp[6] = 0x80;                      /* insufficient privilege */
                break;
            }
            if ((reqlen - 7) >= 5) {
                if ((req[8] == 'C') && (req[9] == 'L') && (req[10] == 'R') &&
                    (req[11] & 0x0f) == 0x0a)
                    sel_clear();
            }
            rsp[n++] = 0x00;                        /* erase complete */
            break;

        case 0x48: {                                /* Get SEL Time */
            time_t now = time(NULL);
            uint32_t t = (uint32_t)now;
            rsp[n++] = (uint8_t)(t & 0xff);
            rsp[n++] = (uint8_t)((t >> 8) & 0xff);
            rsp[n++] = (uint8_t)((t >> 16) & 0xff);
            rsp[n++] = (uint8_t)((t >> 24) & 0xff);
            break;
        }

        case 0x49:                                  /* Set SEL Time */
            break;

        default:
            rsp[6] = CC_INVALID_CMD;
            break;
        }
        break;

    case NETFN_TRANSPORT:
        switch (cmd) {
        case 0x01: {                            /* Set LAN Configuration Params */
            uint8_t ch  = (reqlen - 7) > 0 ? (uint8_t)(req[6] & 0x0f) : 0;
            uint8_t sel = (reqlen - 7) > 1 ? req[7] : 0;
            const uint8_t *pd = req + 8;
            int dlen = reqlen - 9;
            char ifname[32], key[192], val[64];
            char ip[32], mask[32];
            int prefix = 0, i;

            if (ch != 1 && ch != 0x0e && ch != 0x0f) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            ifname[0] = '\0';
            {
                struct in_addr imask;
                unsigned char imac[6];
                imask.s_addr = 0;
                memset(imac, 0, sizeof(imac));
                iface_for_addr(g_local_ip, ifname, sizeof(ifname), &imask, imac);
            }
            if (ifname[0] == '\0')
                snprintf(ifname, sizeof(ifname), "eth1");

            switch (sel) {
            case LANP_SET_IN_PROGRESS:          /* "set in progress" handshake */
                break;

            case LANP_IP_ADDRESS:
                if (dlen < 4) { rsp[6] = CC_INVALID_DATA; break; }
                snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                         pd[0], pd[1], pd[2], pd[3]);
                snprintf(key, sizeof(key),
                         "/config/system/mgmt/interface-list/%s/ipv4-address",
                         ifname);
                if (conf_set_line(key, ip) != 0) { rsp[6] = CC_INVALID_DATA; break; }
                conf_rehash();
                logmsg("lan: %s ipv4-address -> %s (takes effect when is_mgmtd "
                       "re-applies its configuration)", ifname, ip);
                break;

            case LANP_SUBNET_MASK:
                if (dlen < 4) { rsp[6] = CC_INVALID_DATA; break; }
                snprintf(mask, sizeof(mask), "%u.%u.%u.%u",
                         pd[0], pd[1], pd[2], pd[3]);
                for (i = 0; i < 4; i++) {
                    int b = pd[i];
                    while (b & 0x80) { prefix++; b = (b << 1) & 0xff; }
                }
                snprintf(key, sizeof(key),
                         "/config/system/mgmt/interface-list/%s/ipv4-prefix",
                         ifname);
                snprintf(val, sizeof(val), "%d", prefix);
                if (conf_set_line(key, val) != 0) { rsp[6] = CC_INVALID_DATA; break; }
                conf_rehash();
                logmsg("lan: %s subnet mask %s -> /%d", ifname, mask, prefix);
                break;

            case LANP_IP_ADDRESS_SOURCE:
                if (dlen < 1) { rsp[6] = CC_INVALID_DATA; break; }
                snprintf(key, sizeof(key),
                         "/config/system/mgmt/interface-list/%s/ipv4-origin",
                         ifname);
                snprintf(val, sizeof(val), "%s", pd[0] == 2 ? "dhcp" : "static");
                if (conf_set_line(key, val) != 0) { rsp[6] = CC_INVALID_DATA; break; }
                conf_rehash();
                logmsg("lan: %s ipv4-origin -> %s", ifname, val);
                break;

            case LANP_DEFAULT_GATEWAY:
                if (dlen < 4) { rsp[6] = CC_INVALID_DATA; break; }
                snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                         pd[0], pd[1], pd[2], pd[3]);
                if (conf_set_line("/config/system/mgmt/default-gateway", ip) != 0) {
                    rsp[6] = CC_INVALID_DATA;
                    break;
                }
                conf_rehash();
                logmsg("lan: default-gateway -> %s", ip);
                break;

            case 18: {                          /* Alert Destination Type */
                uint8_t dsel = dlen > 0 ? pd[0] : 0;
                if (dsel < 1 || dsel > ALERT_MAX || dlen < 4) {
                    rsp[6] = CC_INVALID_DATA;
                    break;
                }
                g_alert[dsel - 1].type        = pd[1];
                g_alert[dsel - 1].ack_timeout = (uint8_t)(pd[2] & 0x3f);
                g_alert[dsel - 1].retries     = (uint8_t)((pd[2] >> 6) & 0x03);
                g_alert[dsel - 1].addr_format = pd[3];
                g_alert[dsel - 1].enabled     = pd[1] != 0 ? 1 : 0;
                alert_save();
                logmsg("lan: alert destination %u type %u enabled %u",
                       dsel, pd[1], g_alert[dsel - 1].enabled);
                break;
            }

            case 19: {                          /* Alert Destination Address */
                uint8_t dsel = dlen > 0 ? pd[0] : 0;
                if (dsel < 1 || dsel > ALERT_MAX || dlen < 5) {
                    rsp[6] = CC_INVALID_DATA;
                    break;
                }
                memcpy(&g_alert[dsel - 1].ip.s_addr, pd + 1, 4);
                alert_save();
                logmsg("lan: alert destination %u -> %s", dsel,
                       inet_ntoa(g_alert[dsel - 1].ip));
                break;
            }

            case LANP_NUM_DESTINATIONS:         /* informational, read-only */
                break;

            case LANP_AUTH_TYPE_ENABLE: {
                /* 5 bytes: per privilege level */
                int k;
                if (dlen < 5) { rsp[6] = CC_INVALID_DATA; break; }
                for (k = 0; k < 5; k++)
                    g_lan.auth_enables[k] = pd[k];
                lanparams_save();
                logmsg("lan: auth enables %02x %02x %02x %02x %02x",
                       pd[0], pd[1], pd[2], pd[3], pd[4]);
                break;
            }

            case LANP_AUTH_TYPE_SUPPORT:
                if (dlen < 1) { rsp[6] = CC_INVALID_DATA; break; }
                g_lan.auth_types = pd[0];
                lanparams_save();
                logmsg("lan: auth types -> 0x%02x", pd[0]);
                break;

            case LANP_COMMUNITY_STRING: {
                int n = dlen > 17 ? 17 : dlen;
                if (n < 1) { rsp[6] = CC_INVALID_DATA; break; }
                memset(g_lan.community, 0, sizeof(g_lan.community));
                memcpy(g_lan.community, pd, (size_t)n);
                lanparams_save();
                logmsg("lan: SNMP community -> '%s' (used by PET)", g_lan.community);
                break;
            }

            case LANP_BMC_ARP_CONTROL:
                if (dlen < 1) { rsp[6] = CC_INVALID_DATA; break; }
                g_lan.arp_control = pd[0];
                lanparams_save();
                break;

            case LANP_GRATUITOUS_ARP:
                if (dlen < 1) { rsp[6] = CC_INVALID_DATA; break; }
                g_lan.garp_interval = pd[0];
                lanparams_save();
                break;

            case LANP_8021Q_VLAN_ID: {
                /* this really does reconfigure the interface: mgmtd turns
                 * eth1 into a VLAN sub-interface on its next start, so a wrong
                 * value means losing the BMC until you reach the console */
                unsigned vlan;
                if (dlen < 2) { rsp[6] = CC_INVALID_DATA; break; }
                vlan = (unsigned)(pd[0] | (pd[1] << 8));
                if (vlan > 4094) { rsp[6] = CC_INVALID_DATA; break; }
                snprintf(key, sizeof(key),
                         "/config/system/mgmt/interface-list/%s/vlan", ifname);
                snprintf(val, sizeof(val), "%u", vlan);
                if (conf_set_line(key, val) != 0) {
                    rsp[6] = CC_INVALID_DATA;
                    break;
                }
                conf_rehash();
                logmsg("lan: %s vlan -> %u (takes effect when is_mgmtd applies "
                       "its configuration -- the switch port must be tagged the "
                       "same way)", ifname, vlan);
                break;
            }

            case LANP_8021Q_VLAN_PRIORITY:
                /* priority comes from the socket, nothing to store */
                break;

            default:
                /* unknown parameter: say so instead of pretending */
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            break;
        }

        case 0x02: {                            /* Get LAN Configuration Params */
            uint8_t ch       = (reqlen - 7) > 0 ? (uint8_t)(req[6] & 0x0f) : 0;
            uint8_t sel      = (reqlen - 7) > 1 ? req[7] : 0;
            uint8_t dsel     = (reqlen - 7) > 2 ? req[8] : 0;
            int     rev_only = (reqlen - 7) > 0 && (req[6] & 0x80);
            uint8_t pd[64];
            int     plen2, start = n;

            if (ch != 1 && ch != 0x0e && ch != 0x0f) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rsp[n++] = 0x11;                /* parameter revision 1 */
            if (rev_only)
                break;
            plen2 = lan_param(pd, (int)sizeof(pd), sel, dsel);
            if (plen2 < 0) {
                n = start;                  /* drop the revision byte again */
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            memcpy(rsp + n, pd, (size_t)plen2);
            n += plen2;
            break;
        }

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

static void drop_session(session_t *s);

/* IPMI lets the BMC close an idle session; without this, eight abandoned
 * sessions (a polling tool that dies, a client that never sends Close Session)
 * fill the table and the next open-session has to evict a live one. */
#define SESSION_IDLE_TIMEOUT 60

static void sessions_expire(void)
{
    time_t now = time(NULL);
    int i;

    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use)
            continue;
        /* last == 0 means "allocated but nothing received yet" -- that is a
         * session in the middle of its handshake, not an idle one */
        if (g_sessions[i].last == 0 ||
            now - g_sessions[i].last < SESSION_IDLE_TIMEOUT)
            continue;
        if (g_sessions[i].sol_cfd >= 0)      /* console traffic keeps it alive */
            continue;
        logmsg("expiring idle session rm=0x%08x user=%s",
               g_sessions[i].rm_id, g_sessions[i].user);
        drop_session(&g_sessions[i]);
    }
}

static session_t *alloc_session(void)
{
    int i, oldest = 0;

    sessions_expire();
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use) {
            /* stamp it now: an un-stamped slot looks like the oldest one and
             * the next open-session would recycle it right out from under the
             * client that just opened it */
            g_sessions[i].last = time(NULL);
            return &g_sessions[i];
        }
        if (g_sessions[i].last < g_sessions[oldest].last)
            oldest = i;
    }
    /* every slot is genuinely in use: recycle the least recently used one */
    logmsg("all %d sessions busy, recycling the oldest (rm=0x%08x)",
           MAX_SESSIONS, g_sessions[oldest].rm_id);
    drop_session(&g_sessions[oldest]);
    g_sessions[oldest].last = time(NULL);
    return &g_sessions[oldest];
}

static void drop_session(session_t *s)
{
    if (!s)
        return;
    sol_deactivate(s);
    memset(s, 0, sizeof(*s));
    s->sol_cfd = -1;            /* 0 would look like an open descriptor */
}

/* ------------------------------------------------------------ RMCP+ in */

static void handle_open_request(int fd, const struct sockaddr_in *to,
                                const uint8_t *pl, int plen)
{
    uint8_t   rsp[64];
    int       n = 0;
    uint8_t   status = RMCP_OK;
    uint8_t   auth_algo = AUTH_ALGO_SHA1;
    uint8_t   integ_algo = INTEG_ALGO_SHA1_96;
    uint8_t   conf_algo = CONF_ALGO_AES_CBC_128;
    session_t *s;

    if (plen < 16)
        return;

    /* Each algorithm payload is 8 bytes: type(1) rsvd(2) len(1) algo(1)
     * rsvd(3).  Clients send exactly one request these details, but only the
     * algorithm byte matters here. */
    auth_algo = pl[12];
    if (plen > 20)
        integ_algo = pl[20];
    if (plen > 28)
        conf_algo = pl[28];

    if (auth_algo == AUTH_ALGO_SHA256) {
        auth_algo  = AUTH_ALGO_SHA256;
        integ_algo = INTEG_ALGO_SHA256_128;
    } else if (auth_algo == AUTH_ALGO_SHA1) {
        auth_algo  = AUTH_ALGO_SHA1;
        integ_algo = INTEG_ALGO_SHA1_96;
    } else {
        /* Report it as an unsupported algorithm rather than with the
         * catch-all code: 0x04 is in the "algorithm rejected" set that clients
         * use to fall back to another cipher suite. */
        status = RMCP_INVALID_AUTH_ALGO;
        logmsg("open request rejected: auth algo %u, only SHA-1 (suite 3) "
               "and SHA-256 (suite 17) are supported", auth_algo);
    }
    if (status == RMCP_OK && conf_algo != CONF_ALGO_AES_CBC_128) {
        status = RMCP_INVALID_CONF_ALGO;
        logmsg("open request rejected: confidentiality algo %u", conf_algo);
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
    s->sol_cfd = -1;
    s->in_use = 1;
    s->rc_id  = rd_le32(pl + 4);
    s->rm_id  = ++g_next_id;
    s->priv   = 4;
    s->auth_algo  = auth_algo;
    s->integ_algo = integ_algo;
    s->dig_len    = dig_len_of(auth_algo);
    s->icv_len    = icv_len_of(integ_algo);
    s->last   = time(NULL);

    wr_le32(rsp + n, s->rm_id); n += 4;
    /* echo the negotiated algorithms back, one 8 byte payload each */
    rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x08;
    rsp[n++] = auth_algo; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
    rsp[n++] = 0x01; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x08;
    rsp[n++] = integ_algo; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;
    rsp[n++] = 0x02; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x08;
    rsp[n++] = conf_algo; rsp[n++] = 0x00; rsp[n++] = 0x00; rsp[n++] = 0x00;

    logmsg("open session rc=%08x rm=%08x auth=%u integ=%u",
           s->rc_id, s->rm_id, auth_algo, integ_algo);
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
    uint8_t   rsp[MAX_PKT], mac[MAX_DIGEST], buf[128];
    int       n = 0, blen, userlen, mlen;
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
    /* the account's maximum privilege caps whatever the client asked for.
     * Only s->priv is clamped -- the role byte feeds the RAKP MACs. */
    {
        ipmi_user_t *uu = user_by_name(user);
        if (uu && role > uu->priv) {
            logmsg("rakp1: '%s' asked for privilege %u, capped to %u",
                   user, role, uu->priv);
            role = uu->priv;
        }
    }
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

    /* RAKP2 authcode = HMAC(password,
     *   RcID || RmID || Rc || Rm || GUID || role || userlen || user)
     * with the full digest length of the negotiated algorithm. */
    wr_le32(buf + 0,  s->rc_id);
    wr_le32(buf + 4,  s->rm_id);
    memcpy(buf + 8,  s->rc_rand, 16);
    memcpy(buf + 24, s->rm_rand, 16);
    memcpy(buf + 40, g_guid, 16);
    buf[56] = s->role_byte;
    buf[57] = (uint8_t)userlen;
    memcpy(buf + 58, user, userlen);
    blen = 58 + userlen;
    mlen = hmac_md(auth_md(s->auth_algo), (const uint8_t *)pass,
                   (int)strlen(pass), buf, blen, mac);
    memcpy(rsp + n, mac, (size_t)mlen); n += mlen;

    logmsg("rakp1 user=%s role=%u auth=%u", user, role, s->auth_algo);
    send_plain(fd, to, PT_RAKP2, s->rm_id, 0, rsp, n);
}

static void handle_rakp3(int fd, const struct sockaddr_in *to,
                         const uint8_t *pl, int plen)
{
    uint8_t   rsp[80], buf[128], expect[MAX_DIGEST], b2[40];
    int       n = 0, blen, ul, mlen;
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

    /* RAKP3 authcode = HMAC(password,
     *   Rm || RcID || role || userlen || user) */
    ul = (int)strlen(s->user);
    blen = 0;
    memcpy(buf + blen, s->rm_rand, 16); blen += 16;
    wr_le32(buf + blen, s->rc_id);      blen += 4;
    buf[blen++] = s->role_byte;
    buf[blen++] = (uint8_t)ul;
    memcpy(buf + blen, s->user, ul);    blen += ul;
    hmac_md(auth_md(s->auth_algo), (const uint8_t *)pass, (int)strlen(pass),
            buf, blen, expect);

    if (plen < 8 + s->dig_len || memcmp(expect, pl + 8, (size_t)s->dig_len) != 0) {
        logmsg("rakp3: bad password for '%s'", s->user);
        rakp_error(fd, to, pl[0], 0x01, s->rc_id, s->rm_id);  /* invalid session */
        drop_session(s);
        return;
    }

    /* RAKP4 authcode = HMAC(sik, Rc || RmID || GUID)[:icv length] */
    rsp[n++] = pl[0];
    rsp[n++] = RMCP_OK;
    rsp[n++] = 0x00;
    rsp[n++] = 0x00;
    wr_le32(rsp + n, s->rc_id); n += 4;
    memcpy(b2 + 0, s->rc_rand, 16);
    wr_le32(b2 + 16, s->rm_id);
    memcpy(b2 + 20, g_guid, 16);
    mlen = hmac_md(auth_md(s->auth_algo), s->sik, s->dig_len, b2, 36, expect);
    if (s->icv_len > mlen)
        s->icv_len = mlen;
    memcpy(rsp + n, expect, (size_t)s->icv_len); n += s->icv_len;

    logmsg("session established: user=%s rm=%08x priv=%u auth=%u integ=%u",
           s->user, s->rm_id, s->priv, s->auth_algo, s->integ_algo);
    send_plain(fd, to, PT_RAKP4, s->rm_id, 0, rsp, n);
}

/* the K1/K2 pair the client actually uses (see key_variant) */
static void session_keys(const session_t *s, const uint8_t **k1,
                         const uint8_t **k2)
{
    int alt = s->key_variant > 0;

    if (k1 != NULL)
        *k1 = alt ? s->k1_alt : s->k1;
    if (k2 != NULL)
        *k2 = alt ? s->k2_alt : s->k2;
}

/* wrap a payload into an encrypted, integrity protected RMCP+ packet */
static void send_session_payload(int fd, const struct sockaddr_in *to,
                                 session_t *s, uint8_t ptype,
                                 const uint8_t *payload, int plen)
{
    uint8_t out[MAX_PKT], iv[16], mac[MAX_DIGEST], enc[MAX_PKT + 32];
    uint8_t cipher[MAX_PKT + 32], *o;
    int i, pad, enclen, n, mlen, icv;
    const uint8_t *k1, *k2;

    if (plen <= 0 || plen + 17 > (int)sizeof(enc)) {
        logmsg("sol: payload too large (%d)", plen);
        return;
    }
    session_keys(s, &k1, &k2);
    pad = (plen + 1) % 16;
    if (pad)
        pad = 16 - pad;
    memcpy(enc, payload, (size_t)plen);
    /* The IPMI 2.0 confidentiality pad bytes are "don't care", but python-ipmi
     * validates them against the spec's example pattern 0x01, 0x02, ... so
     * use that: pyghmi and ipmitool ignore the values and accept it. */
    for (i = 0; i < pad; i++)
        enc[plen + i] = (uint8_t)(i + 1);
    enc[plen + pad] = (uint8_t)pad;
    enclen = plen + pad + 1;

    rand_bytes(iv, 16);
    aes_cbc(1, k2, iv, enc, enclen, cipher);

    o = rmcp_plus_hdr(out, (uint8_t)(ptype | 0x40 | 0x80), s->rc_id,
                      ++s->out_seq, 16 + enclen);
    memcpy(o, iv, 16); o += 16;
    memcpy(o, cipher, enclen); o += enclen;

    n = (int)(o - out) - 2;                         /* exclude length field */
    pad = n % 4;
    if (pad)
        pad = 4 - pad;
    for (i = 0; i < pad; i++)
        *o++ = 0xff;
    *o++ = (uint8_t)pad;
    *o++ = 0x07;
    icv = s->icv_len;
    mlen = hmac_md(integ_md(s->integ_algo), k1, s->dig_len,
                   out + 4, (int)(o - out) - 4, mac);
    if (icv > mlen)
        icv = mlen;
    memcpy(o, mac, (size_t)icv); o += icv;
    if (sendto(fd, out, (size_t)(o - out), 0,
               (const struct sockaddr *)to, sizeof(*to)) < 0)
        logmsg("sol: sendto(type %02x) failed: %s", ptype, strerror(errno));
}

/* drain the console socket into the per-session output buffer */
static void sol_pump(void)
{
    session_t *s = g_sol_owner;
    uint8_t buf[256];
    int n;

    if (s == NULL || s->sol_cfd < 0)
        return;
    for (;;) {
        int room = (int)sizeof(s->sol_outbuf) - s->sol_outlen;

        if (room <= 0)
            return;                 /* full: stop reading, the stream socket
                                     * provides the backpressure */
        if (room > (int)sizeof(buf))
            room = (int)sizeof(buf);
        n = (int)read(s->sol_cfd, buf, (size_t)room);
        if (n <= 0)
            return;
        memcpy(s->sol_outbuf + s->sol_outlen, buf, (size_t)n);
        s->sol_outlen += n;
    }
}

/* send whatever accumulated, respecting the acknowledgement window */
static void sol_flush(void)
{
    session_t *s = g_sol_owner;
    uint8_t pl[4 + SOL_MAX_OUT + 8];
    int len;

    if (s == NULL || s->sol_cfd < 0 || !s->have_peer)
        return;

    /* A BREAK from the host has to be reported even when there is no
     * character data to piggyback it on: send a bare header in that case. */
    if (g_hostbrk_pending > 0 && s->sol_outlen <= 0) {
        pl[0] = 0;
        pl[1] = s->sol_ack_seq;
        pl[2] = 0;
        pl[3] = 0x04;
        send_session_payload(g_ipmi_fd, &s->peer, s, PT_SOL, pl, 4);
        if (debug_on())
            logmsg("sol: reporting host BREAK (%d pending)",
                   g_hostbrk_pending);
        g_hostbrk_pending--;
        return;
    }
    if (s->sol_outlen <= 0)
        return;

    while (s->sol_outlen > 0 &&
           (s->sol_noack || s->sol_ninflight < SOL_WINDOW)) {
        sol_pkt_t *q;

        len = s->sol_outlen;
        if (len > SOL_MAX_OUT)
            len = SOL_MAX_OUT;

        pl[0] = s->sol_tx_seq;
        if (++s->sol_tx_seq == 0)
            s->sol_tx_seq = 1;                  /* 0 is reserved for ACKs */
        pl[1] = s->sol_ack_seq;
        pl[2] = (uint8_t)len;
        pl[3] = 0;
        if (!host_power_is_on())
            pl[3] |= 0x20;                      /* system power is off */
        if (g_hostbrk_pending > 0) {
            pl[3] |= 0x04;                      /* host BREAK detected */
            g_hostbrk_pending--;
        }
        memcpy(pl + 4, s->sol_outbuf, (size_t)len);

        send_session_payload(g_ipmi_fd, &s->peer, s, PT_SOL, pl, 4 + len);
        if (debug_on())
            logmsg("sol: tx %d chars seq=%u ack=%u inflight=%d",
                   len, pl[0], pl[1], s->sol_ninflight);

        if (!s->sol_noack) {
            q = &s->sol_inflight[s->sol_ninflight++];
            memcpy(q->data, pl, (size_t)(4 + len));
            q->len   = 4 + len;
            q->seq   = pl[0];
            q->tries = 0;
            if (s->sol_ninflight == 1)
                s->sol_retry_ms = now_ms() + SOL_RETRY_MS;
        }

        if (len < s->sol_outlen) {
            memmove(s->sol_outbuf, s->sol_outbuf + len,
                    (size_t)(s->sol_outlen - len));
            s->sol_outlen -= len;
        } else {
            s->sol_outlen = 0;
        }
    }
}

/* drop `count` entries from the front of the unacknowledged queue */
static void sol_queue_drop(session_t *s, int count)
{
    int i;

    if (count <= 0)
        return;
    if (count > s->sol_ninflight)
        count = s->sol_ninflight;
    for (i = count; i < s->sol_ninflight; i++)
        s->sol_inflight[i - count] = s->sol_inflight[i];
    s->sol_ninflight -= count;
    memset(&s->sol_inflight[s->sol_ninflight], 0, sizeof(sol_pkt_t));
    s->sol_retry_ms = s->sol_ninflight ? now_ms() + SOL_RETRY_MS : 0;
}

/* the client acknowledged up to `seq` (inclusive) */
static void sol_ack(session_t *s, int seq)
{
    int i;

    for (i = 0; i < s->sol_ninflight; i++) {
        if (s->sol_inflight[i].seq == (uint8_t)seq) {
            s->sol_acks_seen++;
            sol_queue_drop(s, i + 1);
            break;
        }
    }
}

/* retransmit the oldest unacknowledged packet, giving up after a few tries */
static void sol_retransmit(void)
{
    session_t *s = g_sol_owner;
    sol_pkt_t *q;

    if (s == NULL || s->sol_ninflight <= 0 || s->sol_retry_ms == 0)
        return;
    if (now_ms() < s->sol_retry_ms)
        return;
    q = &s->sol_inflight[0];
    if (q->tries >= (s->sol_acks_seen == 0 ? 2 : SOL_MAX_TRIES)) {
        if (s->sol_acks_seen == 0) {
            /* the client never acknowledges at all, so a window would stall
             * the console forever: fall back to plain streaming */
            logmsg("sol: no acknowledgements from client, streaming without "
                   "a window");
            s->sol_noack = 1;
            sol_queue_drop(s, s->sol_ninflight);
            s->sol_retry_ms = 0;
            sol_flush();
            return;
        }
        logmsg("sol: giving up on seq %u after %d tries", q->seq, q->tries);
        sol_queue_drop(s, 1);
        sol_flush();                    /* the freed slot may hold up output */
        return;
    }
    q->tries++;
    if (s->have_peer)
        send_session_payload(g_ipmi_fd, &s->peer, s, PT_SOL, q->data, q->len);
    s->sol_retry_ms = now_ms() + SOL_RETRY_MS;
    if (debug_on())
        logmsg("sol: retransmit seq=%u try=%d", q->seq, q->tries);
}

/* a SOL packet from the client: characters in, acknowledgement out */
static void sol_recv_payload(session_t *s, const uint8_t *pl, int plen)
{
    uint8_t ack[4];
    int seq, acked, n;

    if (plen < 4)
        return;
    seq   = pl[0] & 0x0f;
    acked = pl[1] & 0x0f;
    if (debug_on())
        logmsg("sol: rx seq=%d ack=%d len=%d flags=%02x", seq, acked, plen, pl[3]);

    if (seq != 0 && seq != s->sol_rx_seq && plen > 4 && s->sol_cfd >= 0) {
        s->sol_rx_seq = (uint8_t)seq;
        n = (int)write(s->sol_cfd, pl + 4, (size_t)(plen - 4));
        if (n < 0)
            logmsg("sol: console write: %s", strerror(errno));
        s->sol_ack_seq = (uint8_t)seq;
    }
    if (pl[3] & 0x10)
        sol_break_assert();

    if (acked != 0)
        sol_ack(s, acked);

    ack[0] = 0;                                     /* no data, pure ACK */
    ack[1] = (uint8_t)seq;
    ack[2] = (uint8_t)(plen > 4 ? plen - 4 : 0);
    ack[3] = 0;
    if (s->have_peer)
        send_session_payload(g_ipmi_fd, &s->peer, s, PT_SOL, ack, 4);

    sol_flush();                                    /* window may have opened */
}

static void handle_encapsulated(int fd, const struct sockaddr_in *to,
                                const uint8_t *pkt, int len)
{
    uint8_t   rspmsg[MAX_PKT], plain[MAX_PKT], iv[16], mac[MAX_DIGEST];
    int       plen, flen, padlen, msglen, rlen = 0;
    uint8_t   ptype;
    uint32_t  sid;
    session_t *s;

    if (len < 16)
        return;
    sid   = rd_le32(pkt + 6);
    plen  = pkt[14] | (pkt[15] << 8);
    ptype = (uint8_t)(pkt[5] & 0x3f);

    s = by_rm_id(sid);
    if (!s)
        return;
    if (16 + plen > len || plen < 17)
        return;

    s->peer = *to;                                  /* remember for async SOL */
    s->have_peer = 1;

    /* integrity: HMAC-SHA1-96 or HMAC-SHA256-128 over everything after the
     * RMCP header, depending on the negotiated cipher suite.  For SHA-256 the
     * K1 derivation is not agreed between clients, so the variant that
     * validates this first packet is latched for the rest of the session. */
    if (len - s->icv_len - 4 > 0) {
        int m2, vari = -1;

        if (s->key_variant < 0) {
            m2 = hmac_md(integ_md(s->integ_algo), s->k1, s->dig_len,
                         pkt + 4, len - s->icv_len - 4, mac);
            if (m2 >= s->icv_len &&
                memcmp(mac, pkt + len - s->icv_len, (size_t)s->icv_len) == 0) {
                vari = 0;
            } else {
                m2 = hmac_md(integ_md(s->integ_algo), s->k1_alt, s->dig_len,
                             pkt + 4, len - s->icv_len - 4, mac);
                if (m2 >= s->icv_len &&
                    memcmp(mac, pkt + len - s->icv_len,
                           (size_t)s->icv_len) == 0)
                    vari = 1;
            }
            if (vari < 0) {
                logmsg("integrity check failed, dropping packet");
                return;
            }
            s->key_variant = vari;
            logmsg("key derivation variant %d in use", vari);
        } else {
            const uint8_t *k1, *k2;
            session_keys(s, &k1, &k2);
            m2 = hmac_md(integ_md(s->integ_algo), k1, s->dig_len,
                         pkt + 4, len - s->icv_len - 4, mac);
            if (m2 < s->icv_len)
                return;
            if (memcmp(mac, pkt + len - s->icv_len, (size_t)s->icv_len) != 0) {
                logmsg("integrity check failed, dropping packet");
                return;
            }
        }
    }

    /* confidentiality: 16-byte IV then AES-CBC-128 */
    {
        const uint8_t *k2;
        int clen;

        session_keys(s, NULL, &k2);
        memcpy(iv, pkt + 16, 16);
        clen = plen - 16;
        if (clen <= 0 || clen > (int)sizeof(plain) || (clen % 16) != 0)
            return;
        flen = aes_cbc(0, k2, iv, pkt + 32, clen, plain);
        if (flen != clen) {
            logmsg("decrypt failed");
            return;
        }
    }
    padlen = plain[plen - 17];
    msglen = plen - 17 - padlen;
    if (msglen <= 0)
        return;

    if (ptype == PT_SOL) {
        sol_recv_payload(s, plain, msglen);
        s->last = time(NULL);
        return;
    }

    if (dispatch_ipmi(plain, msglen, rspmsg, &rlen, s) < 0) {
        logmsg("bad ipmi message");
        return;
    }
    send_session_payload(fd, to, s, PT_IPMI, rspmsg, rlen);
    s->last = time(NULL);
    if (s->close_pending)
        drop_session(s);
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
    case PT_SOL:      handle_encapsulated(fd, to, pkt, len);           break;
    default:
        logmsg("unhandled payload type 0x%02x", ptype);
        break;
    }
}

/* ASF Presence Ping/Pong, RMCP class 6.  Real BMCs answer this and some IPMI
 * clients insist on it before they will even start an RMCP+ session. */
static void handle_asf(int fd, const struct sockaddr_in *to,
                       const uint8_t *pkt, int len)
{
    uint8_t out[28];
    static const uint8_t pong[16] = {
        0x00, 0x00, 0x11, 0xbe,     /* ASF IANA enterprise number */
        0x00, 0x00, 0x00, 0x00,     /* OEM defined, must be zero */
        0x80,                       /* supported entities: IPMI */
        0x00,                       /* supported interactions */
        0, 0, 0, 0, 0, 0            /* reserved */
    };

    if (len < 12)
        return;
    if (pkt[4] != 0x00 || pkt[5] != 0x00 || pkt[6] != 0x11 || pkt[7] != 0xbe)
        return;                             /* not the ASF IANA */
    if (pkt[8] != 0x80)
        return;                             /* not a presence ping */

    out[0] = 0x06;                          /* RMCP version 1.0 */
    out[1] = 0x00;
    out[2] = 0xff;                          /* no RMCP acknowledgement */
    out[3] = 0x06;                          /* class: ASF */
    out[4] = 0x00; out[5] = 0x00; out[6] = 0x11; out[7] = 0xbe;
    out[8] = 0x40;                          /* presence pong */
    out[9] = pkt[9];                        /* echo the request tag */
    out[10] = 0x00;
    out[11] = (uint8_t)sizeof(pong);
    memcpy(out + 12, pong, sizeof(pong));
    if (sendto(fd, out, sizeof(out), 0,
               (const struct sockaddr *)to, sizeof(*to)) < 0)
        logmsg("asf: pong sendto: %s", strerror(errno));
    else if (debug_on())
        logmsg("asf: presence pong sent");
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
    /* freopen() gives the stream default (block) buffering; without this the
     * log only appears once 4 kB accumulate or the process exits */
    setvbuf(stderr, NULL, _IONBF, 0);
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
    /* needed to learn the local address a request was sent to, which is what
     * the LAN configuration parameters have to describe */
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));

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
    pef_load();
    alert_load();
    lanparams_load();
    /* LOG_USER, not LOG_DAEMON: rsyslog on this box drops daemon.* while
     * user.* lands in /var/log/messages, which is where an operator looks */
    openlog("ubmc-ipmi", LOG_PID, LOG_USER);

    sensors_scan();
    sdr_build();
    sdr_user_load();
    fru_build();
    boot_flags_load();
    for (i = 0; i < MAX_SESSIONS; i++)
        g_sessions[i].sol_cfd = -1;

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
            if (nfds == 1)
                snprintf(g_bind_dev, sizeof(g_bind_dev), "%s", p);
            logmsg("listening on udp/%d dev=%s", IPMI_PORT, *p ? p : "any");
        }
        p = comma ? comma + 1 : NULL;
    }

    if (nfds == 0) {
        logmsg("no listener could be created");
        return 1;
    }
    g_ipmi_fd = fds[0];                 /* SOL replies go out this socket */

    while (!g_stop) {
        fd_set rfds;
        struct timeval tv;
        int r, maxfd2 = maxfd;

        FD_ZERO(&rfds);
        for (i = 0; i < nfds; i++)
            FD_SET(fds[i], &rfds);
        if (g_sol_owner != NULL && g_sol_owner->sol_cfd >= 0) {
            FD_SET(g_sol_owner->sol_cfd, &rfds);
            if (g_sol_owner->sol_cfd > maxfd2)
                maxfd2 = g_sol_owner->sol_cfd;
        }
        /* a short tick while SOL is up keeps the character accumulate
         * interval close to the IPMI default of 100 ms */
        tv.tv_sec  = g_sol_owner ? 0 : 1;
        tv.tv_usec = g_sol_owner ? 20000 : 0;
        r = select(maxfd2 + 1, &rfds, NULL, NULL, &tv);
        if (r < 0)
            continue;
        if (r == 0) {
            sessions_expire();              /* idle tick: reap dead sessions */
            pef_poll();                     /* and look for new SEL events */
        }
        if (g_sol_owner != NULL && g_sol_owner->sol_cfd >= 0 &&
            FD_ISSET(g_sol_owner->sol_cfd, &rfds)) {
            sol_pump();
            sol_flush();
        } else if (g_sol_owner != NULL && r == 0) {
            sol_flush();
        }
        sol_retransmit();
        sol_break_service();
        if (g_sol_owner != NULL)
            sol_hostbreak_poll();
        if (r <= 0)
            continue;

        for (i = 0; i < nfds; i++) {
            uint8_t buf[MAX_PKT];
            struct sockaddr_in from;
            struct iovec iov;
            struct msghdr mh;
            struct cmsghdr *cm;
            char cbuf[256];
            int n;

            if (!FD_ISSET(fds[i], &rfds))
                continue;

            /* recvmsg rather than recvfrom so IP_PKTINFO tells us which local
             * address the packet was addressed to: the LAN configuration
             * parameters have to describe that interface, not eth1 blindly. */
            memset(&mh, 0, sizeof(mh));
            iov.iov_base = buf;
            iov.iov_len = sizeof(buf);
            mh.msg_name = &from;
            mh.msg_namelen = sizeof(from);
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = cbuf;
            mh.msg_controllen = sizeof(cbuf);
            n = (int)recvmsg(fds[i], &mh, 0);
            g_local_ip.s_addr = 0;
            for (cm = CMSG_FIRSTHDR(&mh); cm != NULL;
                 cm = CMSG_NXTHDR(&mh, cm)) {
                if (cm->cmsg_level == IPPROTO_IP &&
                    cm->cmsg_type == IP_PKTINFO) {
                    struct in_pktinfo pi;
                    memcpy(&pi, CMSG_DATA(cm), sizeof(pi));
                    g_local_ip = pi.ipi_addr;
                }
            }
            if (debug_on()) {
                char sa[32];
                inet_ntop(AF_INET, &from.sin_addr, sa, sizeof(sa));
                logmsg("rx %d bytes from %s:%u on %s", n, sa,
                       (unsigned)ntohs(from.sin_port), i == 0 ? "sock0" : "sockN");
            }
            if (n <= 3)
                continue;
            if (buf[0] != 0x06)
                continue;                        /* not RMCP */
            if (buf[3] == 0x06) {
                handle_asf(fds[i], &from, buf, n);
                continue;
            }
            if (buf[3] != 0x07)
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

