/* ============================================================
 * DXENV - ESP32-backed environment variable library
 * ============================================================
 * Thin BDS C wrapper around Intel 8080 I/O ports handled by
 * port_drivers/environment_io.c on the ESP32 emulator.
 * ============================================================
 */

#include "stdio.h"

#define E_KEYSZ  17
#define E_VALSZ  128

#define E_OK     0
#define E_EOPEN  -1
#define E_EREAD  -2
#define E_EWRIT  -3
#define E_EFULL  -4
#define E_ENOTF  -5

#define E_CPORT  71
#define E_DPORT  72
#define E_RPORT  200

#define EC_RST   0
#define EC_INIT  1
#define EC_GET   2
#define EC_SET   3
#define EC_DEL   4
#define EC_LIST  5
#define EC_CNT   6
#define EC_CLR   7
#define EC_EXST  8
#define EC_EXEC  9

int inp();
int outp();
int putchar();

/* ------------------------------------------------------------
 * e_beg - Reset request buffer
 * ------------------------------------------------------------ */
int e_beg()
{
    outp(E_CPORT, EC_RST);
    return 0;
}

/* ------------------------------------------------------------
 * e_stat - Read last status code
 * ------------------------------------------------------------ */
int e_stat()
{
    int c;

    c = inp(E_CPORT);
    if (c > 127)
        c = c - 256;
    return c;
}

/* ------------------------------------------------------------
 * e_sstr - Send NUL-terminated string to request buffer
 * ------------------------------------------------------------ */
int e_sstr(s)
char *s;
{
    while (*s) {
        outp(E_DPORT, *s);
        s++;
    }
    outp(E_DPORT, 0);
    return 0;
}

/* ------------------------------------------------------------
 * e_sval - Send value string, capped at ENV value size
 * ------------------------------------------------------------ */
int e_sval(s)
char *s;
{
    int i;

    i = 0;
    while (*s && i < E_VALSZ - 1) {
        outp(E_DPORT, *s);
        s++;
        i++;
    }
    outp(E_DPORT, 0);
    return 0;
}

/* ------------------------------------------------------------
 * e_rstr - Read response string from shared response port
 * ------------------------------------------------------------ */
int e_rstr(dst, maxlen)
char *dst;
int maxlen;
{
    int i;
    int c;

    i = 0;
    while (i < maxlen - 1) {
        c = inp(E_RPORT);
        if (c == 0)
            break;
        dst[i] = c;
        i++;
    }
    dst[i] = 0;
    return i;
}

/* ------------------------------------------------------------
 * e_atoi - Small positive integer parser
 * ------------------------------------------------------------ */
int e_atoi(s)
char *s;
{
    int n;

    n = 0;
    while (*s >= '0' && *s <= '9') {
        n = (n * 10) + (*s - '0');
        s++;
    }
    return n;
}

/* ------------------------------------------------------------
 * e_init - Initialize host-side environment storage
 * ------------------------------------------------------------ */
int e_init()
{
    e_beg();
    outp(E_CPORT, EC_INIT);
    return e_stat();
}


/* ------------------------------------------------------------
 * e_get - Read environment variable value
 * ------------------------------------------------------------ */
int e_get(key, val)
char *key;
char *val;
{
    int rc;

    e_beg();
    e_sstr(key);
    outp(E_CPORT, EC_GET);
    rc = e_stat();
    if (rc == E_OK)
        e_rstr(val, E_VALSZ);
    else
        val[0] = 0;
    return rc;
}

/* ------------------------------------------------------------
 * e_set - Set environment variable value
 * ------------------------------------------------------------ */
int e_set(key, val)
char *key;
char *val;
{
    e_beg();
    e_sstr(key);
    e_sval(val);
    outp(E_CPORT, EC_SET);
    return e_stat();
}

/* ------------------------------------------------------------
 * e_del - Delete environment variable
 * ------------------------------------------------------------ */
int e_del(key)
char *key;
{
    e_beg();
    e_sstr(key);
    outp(E_CPORT, EC_DEL);
    return e_stat();
}

/* ------------------------------------------------------------
 * e_list - List variables through callback
 * ------------------------------------------------------------ */
int e_list(cb)
int (*cb)();
{
    char key[E_KEYSZ];
    char val[E_VALSZ];
    int ki;
    int vi;
    int cnt;
    int c;
    int st;

    e_beg();
    outp(E_CPORT, EC_LIST);
    if (e_stat() != E_OK)
        return 0;

    ki = 0;
    vi = 0;
    cnt = 0;
    st = 0;
    key[0] = 0;
    val[0] = 0;

    while ((c = inp(E_RPORT)) != 0) {
        if (c == '\r')
            continue;
        if (c == '=' && st == 0) {
            key[ki] = 0;
            st = 1;
            continue;
        }
        if (c == '\n') {
            val[vi] = 0;
            if (key[0]) {
                if (cb)
                    (*cb)(key, val);
                cnt++;
            }
            ki = 0;
            vi = 0;
            st = 0;
            key[0] = 0;
            val[0] = 0;
            continue;
        }
        if (st == 0) {
            if (ki < E_KEYSZ - 1) {
                key[ki] = c;
                ki++;
            }
        } else {
            if (vi < E_VALSZ - 1) {
                val[vi] = c;
                vi++;
            }
        }
    }

    if (key[0]) {
        val[vi] = 0;
        if (cb)
            (*cb)(key, val);
        cnt++;
    }
    return cnt;
}

/* ------------------------------------------------------------
 * e_count - Count active environment variables
 * ------------------------------------------------------------ */
int e_count()
{
    char buf[8];

    e_beg();
    outp(E_CPORT, EC_CNT);
    if (e_stat() != E_OK)
        return 0;
    e_rstr(buf, 8);
    return e_atoi(buf);
}

/* ------------------------------------------------------------
 * e_clear - Delete all environment variables
 * ------------------------------------------------------------ */
int e_clear()
{
    e_beg();
    outp(E_CPORT, EC_CLR);
    return e_stat();
}

/* ------------------------------------------------------------
 * e_exst - Check whether key exists
 * ------------------------------------------------------------ */
int e_exst(key)
char *key;
{
    e_beg();
    e_sstr(key);
    outp(E_CPORT, EC_EXST);
    return (e_stat() == E_OK) ? 1 : 0;
}

/* ------------------------------------------------------------
 * e_exec - Execute ENV command line on ESP32 and print reply
 * ------------------------------------------------------------ */
int e_exec(argc, argv)
int argc;
char *argv[];
{
    int i;
    int c;
    char *s;

    e_beg();
    for (i = 1; i < argc; i++) {
        if (i > 1)
            outp(E_DPORT, ' ');
        s = argv[i];
        while (*s) {
            outp(E_DPORT, *s);
            s++;
        }
    }
    outp(E_DPORT, 0);
    outp(E_CPORT, EC_EXEC);

    while ((c = inp(E_RPORT)) != 0)
        putchar(c);

    return e_stat();
}
