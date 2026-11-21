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
#include <fcntl.h>
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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
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
#define RMCP_INVALID_AUTH_ALGO 0x03
#define RMCP_INVALID_USER      0x0D

typedef struct {
    char name[17];
    char pass[21];
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
    uint8_t  sik[20];
    uint8_t  k1[20];
    uint8_t  k2[20];
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

/* vendor slots are 20 bytes; IPMI wants standard 16-byte records */
static int sel_load(uint8_t *out, int maxrec)
{
    FILE *f = fopen(SEL_FILE, "rb");
    uint8_t slot[SEL_SLOT];
    int n = 0;

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
    return n;
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
    int mo = 0, dy = 0, yy = 0, hh = 0, mi = 0, i;
    int32_t days;
    static const int mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    if (sscanf(s, "%d/%d/%d %d:%d", &mo, &dy, &yy, &hh, &mi) < 3)
        return 0;
    if (mo < 1 || mo > 12 || yy < 1996)
        return 0;
    days = (yy - 1996) * 365 + (yy - 1996 + 3) / 4 + mdays[mo - 1] + (dy - 1);
    if (mo > 2 && ((yy % 4) == 0))
        days++;
    (void)i;
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

        case 0x3B:                                  /* Set Session Priv Level */
            if ((reqlen - 7) < 1 || req[6] > 4) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            if (s)
                s->priv = req[6];
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

        case 0x20:                                  /* Get SDR Repository Info */
            rsp[n++] = 0x51;
            rsp[n++] = (uint8_t)(g_nsensors & 0xff);
            rsp[n++] = (uint8_t)(g_nsensors >> 8);
            rsp[n++] = 0xff; rsp[n++] = 0xff;       /* free space: n/a */
            rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0;
            rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0; rsp[n++] = 0;
            rsp[n++] = 0x02;                        /* reserve supported */
            break;

        case 0x22:                                  /* Reserve SDR Repository */
            rsp[n++] = 0x00; rsp[n++] = 0x00;
            break;

        case 0x23: {                                /* Get SDR */
            int idx, off, want, avail, chunk;
            uint16_t rid, next;

            if ((reqlen - 7) < 6) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            rid  = (uint16_t)(req[8] | (req[9] << 8));
            off  = req[10];
            want = req[11];
            if (rid == 0x0000 || rid == 0xffff)
                rid = 0x0001;                       /* first record */
            idx = (int)rid - 1;
            if (idx < 0 || idx >= g_nsensors || want == 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            avail = g_sdr_len[idx] - off;
            if (avail <= 0) {
                rsp[6] = CC_INVALID_DATA;
                break;
            }
            chunk = want < avail ? want : avail;
            next  = (uint16_t)((idx + 1 < g_nsensors) ? idx + 2 : 0xffff);
            rsp[n++] = (uint8_t)(next & 0xff);
            rsp[n++] = (uint8_t)(next >> 8);
            memcpy(rsp + n, g_sdr_blob + g_sdr_off[idx] + off, (size_t)chunk);
            n += chunk;
            break;
        }

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

static session_t *alloc_session(void)
{
    int i, oldest = 0;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use)
            return &g_sessions[i];
        if (g_sessions[i].last < g_sessions[oldest].last)
            oldest = i;
    }
    drop_session(&g_sessions[oldest]);
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
    uint8_t   auth_algo = 1;                        /* HMAC-SHA1 */
    session_t *s;

    if (plen < 16)
        return;

    /* Algorithm payloads start at offset 8: type(1) rsvd(2) len(2) algo(1) ... */
    auth_algo = pl[12];

    if (auth_algo != 1) {
        /* Only cipher suite 3 (RAKP-HMAC-SHA1 + HMAC-SHA1-96 + AES-CBC-128) is
         * offered.  Report it as an unsupported algorithm rather than with the
         * catch-all code, so a client that asked for SHA-256 (pyipmi does by
         * default) says so instead of failing on some unrelated step. */
        status = RMCP_INVALID_AUTH_ALGO;
        logmsg("open request rejected: auth algo %u, only SHA-1 (cipher "
               "suite 3) is supported", auth_algo);
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

/* wrap a payload into an encrypted, integrity protected RMCP+ packet */
static void send_session_payload(int fd, const struct sockaddr_in *to,
                                 session_t *s, uint8_t ptype,
                                 const uint8_t *payload, int plen)
{
    uint8_t out[MAX_PKT], iv[16], mac[20], enc[MAX_PKT + 32];
    uint8_t cipher[MAX_PKT + 32], *o;
    int i, pad, enclen, n;

    if (plen <= 0 || plen + 17 > (int)sizeof(enc)) {
        logmsg("sol: payload too large (%d)", plen);
        return;
    }
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
    aes_cbc(1, s->k2, iv, enc, enclen, cipher);

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
    hmac_sha1(s->k1, 20, out + 4, (int)(o - out) - 4, mac);
    memcpy(o, mac, AUTHCODE_LEN); o += AUTHCODE_LEN;
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

    if (s == NULL || s->sol_cfd < 0 || s->sol_outlen <= 0 || !s->have_peer)
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
    uint8_t   rspmsg[MAX_PKT], plain[MAX_PKT], iv[16], mac[20];
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

    sensors_scan();
    sdr_build();
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

        if (g_sol_owner != NULL && g_sol_owner->sol_cfd >= 0 &&
            FD_ISSET(g_sol_owner->sol_cfd, &rfds)) {
            sol_pump();
            sol_flush();
        } else if (g_sol_owner != NULL && r == 0) {
            sol_flush();
        }
        sol_retransmit();
        sol_break_service();
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

