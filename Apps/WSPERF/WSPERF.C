/*
 * WSPERF.C - WebSocket / 80-column terminal throughput test
 *
 * BDS C 1.6 port of WSPERF.BAS. The goal is to push rainbow
 * text to the console as fast as the link allows, then report
 * lines/second using a host stopwatch (port 37).
 *
 * Timing:
 *   OUT 37, 0 -> start stopwatch 0
 *   OUT 37, 1 -> latch elapsed seconds as a 4-byte unsigned long
 *   port 200  -> read those 4 bytes back (big-endian, BDS C long order)
 *
 * The elapsed value is an unsigned 32-bit second count, read
 * directly into a BDS C long (no string conversion needed).
 *
 * Output is written with raw BIOS conout so the VT100 color
 * escapes and CR/LF go out as exact bytes (no cooking).
 *
 * Usage: B>wsperf
 */

#include "stdio.h"

#define SWPRT 37          /* stopwatch 0 port                 */
#define RPRT  200         /* request/string read-back port    */
#define ESC   27          /* VT100 escape                     */

#define WIDTH  60         /* printable phrase chars per line  */
#define COLORS 6          /* rainbow cycle length             */
#define COUNT  10000      /* lines to send (<= 65535)         */

#define PHRASE "The quick brown fox jumps over the lazy dog "

int outp();

/* precomputed color prefix per color: ESC[..m + phrase + " # " */
char pre[COLORS][80];
int prelen;

/* VT100 reset + CR LF suffix, written after the counter digits */
char suff[8];

/* long (32-bit) working values */
char lelap[4];            /* elapsed seconds                   */
char lcnt[4];             /* line count                        */
char lrate[4];            /* lines per second                  */
char abuf[16];            /* ltoa output buffer                */

/* counter digit buffer (unsigned, max 65535 -> 5 digits) */
char cbuf[8];

/*
 * Start stopwatch 0 (port 37) so elapsed time is measured from now.
 */
int swstrt()
{
    outp(SWPRT, 0);
    return 0;
}

/*
 * Latch the stopwatch's elapsed seconds and read the 4-byte
 * unsigned long back from the request port into the long at dst.
 * The driver emits the value big-endian (dst[0] = MSB), which is
 * exactly the BDS C long byte layout.
 */
int swread(dst)
char *dst;
{
    outp(SWPRT, 1);
    dst[0] = inp(RPRT);
    dst[1] = inp(RPRT);
    dst[2] = inp(RPRT);
    dst[3] = inp(RPRT);
    return 0;
}

/*
 * Build the color prefix for color index k into dst:
 * Colors 0..5 map to 31..36 (red, green, yellow, blue, magenta, cyan).
 * Returns the prefix length (same for every color).
 */
int mkpre(dst, k)
char *dst;
int k;
{
    char *p;
    char *ph;
    int n;

    p = dst;
    *p++ = ESC;
    *p++ = '[';
    *p++ = '1';
    *p++ = ';';
    *p++ = '3';
    *p++ = '1' + k;
    *p++ = 'm';

    ph = PHRASE;
    n = 0;
    while (n < WIDTH) {
        if (*ph == 0)
            ph = PHRASE;
        *p++ = *ph++;
        n = n + 1;
    }

    *p++ = ' ';
    *p++ = '#';
    *p++ = ' ';
    *p = 0;
    return p - dst;
}

/*
 * Convert unsigned n to decimal text in buf (NUL terminated).
 * Returns the number of digits written.
 */
int udec(buf, n)
char *buf;
unsigned n;
{
    char tmp[8];
    int i, j;

    i = 0;
    if (n == 0)
        tmp[i++] = '0';
    while (n > 0) {
        tmp[i++] = (n % 10) + '0';
        n = n / 10;
    }
    j = 0;
    while (i > 0)
        buf[j++] = tmp[--i];
    buf[j] = 0;
    return j;
}

/*
 * Write a NUL-terminated string to the console via raw BIOS
 * conout (BIOS function 4) so every byte goes out verbatim.
 */
int wstr(s)
char *s;
{
    while (*s) {
        bios(4, *s);
        s = s + 1;
    }
    return 0;
}

int main()
{
    unsigned i;
    int k;
    char *p;

    /* precompute the six rainbow prefixes once */
    for (k = 0; k < COLORS; k++)
        prelen = mkpre(pre[k], k);

    /* suffix: ESC [ 0 m  CR LF */
    p = suff;
    *p++ = ESC;
    *p++ = '[';
    *p++ = '0';
    *p++ = 'm';
    *p++ = '\r';
    *p++ = '\n';
    *p = 0;

    printf("WS THROUGHPUT TEST - %u LINES\r\n", COUNT);

    /* start the stopwatch */
    swstrt();

    /* the hot loop: color prefix + counter + reset/CRLF */
    k = 0;
    for (i = 1; i <= COUNT; i++) {
        wstr(pre[k]);
        udec(cbuf, i);
        wstr(cbuf);
        wstr(suff);
        k = k + 1;
        if (k >= COLORS)
            k = 0;
    }

    /* latch elapsed seconds (unsigned 32-bit) */
    swread(lelap);
    ltoa(abuf, lelap);

    printf("\r\n");
    printf("LINES          : %u\r\n", COUNT);
    printf("ELAPSED SECONDS: %s\r\n", abuf);

    if (lelap[0] == 0 && lelap[1] == 0 && lelap[2] == 0 && lelap[3] == 0) {
        printf("LINES/SEC      : (too fast - raise COUNT)\r\n");
    } else {
        /* rate = lines / elapsed_sec */
        utol(lcnt, COUNT);
        ldiv(lrate, lcnt, lelap);
        ltoa(abuf, lrate);
        printf("LINES/SEC      : %s\r\n", abuf);
    }

    return 0;
}
