/*
 * CLOCK.C - 70s 7-segment psychedelic clock
 * BDS C 1.6 on CP/M / Altair 8800 emulator
 *
 * Reads ESP32 UTC wall clock through port 42, ticks every 50 ms,
 * redraws HH:MM as chunky VT100 character blocks shaped like
 * classic 7-segment digits. The colon blinks every second.
 *
 * The host port driver (port_drivers/time_io.c) fills its
 * request buffer with a 19-char "YYYY-MM-DDTHH:MM:SS" UTC
 * string when port 42 is written; the app reads byte-by-byte
 * from port 200 until it sees a 0, then adds the signed-hours
 * env variable OFFSET (loaded via DXENV) to convert to local
 * time. Set with:  ENV OFFSET=-5
 *
 * Press ESC or Ctrl-C to quit.
 */

#include "stdio.h"

#define T2H 28
#define T2L 29
#define TPRT 42
#define UPRT 41
#define RPRT 200

#define E_OK 0

#define ESC 27
#define KCC 3

#define BROW 10
#define BCOL 13

#define SCRW 80
#define SCRH 30

#define DWID 6
#define DHGT 7

int inp();
int outp();
int bdos();
int bios();
int e_init();
int e_get();
int atol();
int itol();
int ldiv();
int lmod();
int ltoi();

/* signed UTC->local offset in hours (from env OFFSET) */
int offset;

/* previous shown digits (-1 = force redraw) */
int ph0, ph1;
int pm0, pm1;

/* time string buffer */
char tbuf[24];

/* DXENV value buffer: e_get writes up to 111 chars plus nul */
char ebuf[112];

/* current parsed display digits */
int gh0, gh1;
int gm0, gm1;

/* blink phase for the colon (toggles each parsed second) */
int blink;

/* active time text: HH:MM */
char timtxt[6];

/* Seven-segment-ish rows. Each glyph is 5 chars wide. */
char *dig0[7];
char *dig1[7];
char *dig2[7];
char *dig3[7];
char *dig4[7];
char *dig5[7];
char *dig6[7];
char *dig7[7];
char *dig8[7];
char *dig9[7];
char *digc[7];

/* ---- VT100 helpers (short names for BDS C) ---- */

int chout(c)
int c;
{
    return bios(4, c);
}

int cput(s)
char *s;
{
    while (*s)
    {
        chout(*s);
        s++;
    }
    return 0;
}

int nump(n)
int n;
{
    char b[6];
    int i;

    if (n == 0)
    {
        chout('0');
        return 0;
    }

    i = 0;
    while (n > 0 && i < 6)
    {
        b[i] = (n % 10) + '0';
        i++;
        n = n / 10;
    }

    while (i > 0)
    {
        i--;
        chout(b[i]);
    }
    return 0;
}

int curmv(r, c)
int r;
int c;
{
    chout(ESC);
    cput("[");
    nump(r);
    cput(";");
    nump(c);
    cput("H");
    return 0;
}

int setsgr(c)
int c;
{
    chout(ESC);
    cput("[");
    nump(c);
    cput("m");
    return 0;
}

int setblk(c)
int c;
{
    chout(ESC);
    cput("[");
    nump(c);
    cput("m");
    return 0;
}

int rstcol()
{
    chout(ESC);
    cput("[0m");
    return 0;
}

int hidecr()
{
    chout(ESC);
    cput("[?25l");
    return 0;
}

int shocr()
{
    chout(ESC);
    cput("[?25h");
    return 0;
}

int cls()
{
    rstcol();
    chout(ESC);
    cput("[2J");
    curmv(1, 1);
    return 0;
}

/* ---- Timer port helpers ---- */

int tset(ms)
unsigned ms;
{
    char hi, lo;

    hi = ms >> 8;
    outp(T2H, hi);
    lo = ms & 0xFF;
    outp(T2L, lo);
    return 0;
}

int texp()
{
    return (inp(T2L) == 0);
}

/* ---- Time string read ---- */

/*
 * Trigger the host time driver and pull the resulting ISO
 * string into tbuf. Returns the string length.
 */
int getime()
{
    int i;
    int ch;

    outp(TPRT, 0);
    i = 0;
    ch = inp(RPRT);
    while (ch && i < 23)
    {
        tbuf[i] = ch;
        i = i + 1;
        ch = inp(RPRT);
    }
    tbuf[i] = 0;
    return i;
}

/*
 * Parse a signed decimal integer from s. Returns the value;
 * stops at first non-digit. Accepts optional leading +/-.
 */
int parsi(s)
char *s;
{
    int v;
    int sign;
    int i;

    v = 0;
    sign = 1;
    i = 0;
    if (s[0] == '-')
    {
        sign = -1;
        i = 1;
    }
    else if (s[0] == '+')
    {
        i = 1;
    }
    while (s[i] >= '0' && s[i] <= '9')
    {
        v = v * 10 + (s[i] - '0');
        i = i + 1;
    }
    return v * sign;
}

/*
 * Load env OFFSET into the global `offset` variable. Defaults
 * to 0 if env file or variable is missing or unparseable.
 */
int lofset()
{
    offset = 0;
    if (e_init() != E_OK)
        return 0;
    if (e_get("OFFSET", ebuf) != E_OK)
        return 0;
    offset = parsi(ebuf);
    /* sanity clamp */
    if (offset < -23 || offset > 23)
        offset = 0;
    return offset;
}

/*
 * Apply the signed `offset` hours to hour hh. Wraps 0..23.
 */
int adjhr(hh)
int hh;
{
    hh = hh + offset;
    while (hh < 0)  hh = hh + 24;
    while (hh > 23) hh = hh - 24;
    return hh;
}

/*
 * Read NUL-terminated reply from port 200 into buf.
 */
int rdstr(buf, max)
char *buf;
int max;
{
    int i, ch;

    i = 0;
    ch = inp(RPRT);
    while (ch != 0 && i < max - 1)
    {
        buf[i] = ch;
        i = i + 1;
        ch = inp(RPRT);
    }
    buf[i] = 0;
    return i;
}

/*
 * Read uptime seconds from port 41 and print as HH:MM:SS at
 * (row, col). Uses BDS C long-int library.
 */
char upbuf[32];

int shoupt(row, col)
int row;
int col;
{
    char lup[4], l3600[4], l60[4];
    char lhr[4], lrem[4], lmn[4], lsc[4];
    int hrs, mns, scs;

    outp(UPRT, 1);
    rdstr(upbuf, 31);
    atol(lup, upbuf);
    itol(l3600, 3600);
    itol(l60, 60);
    ldiv(lhr, lup, l3600);
    lmod(lrem, lup, l3600);
    ldiv(lmn, lrem, l60);
    lmod(lsc, lrem, l60);
    hrs = ltoi(lhr);
    mns = ltoi(lmn);
    scs = ltoi(lsc);

    curmv(row, col);
    printf("\033[1;93mUptime: %02d:%02d:%02d\033[0m", hrs, mns, scs);
    return 0;
}

/* ---- Drawing primitives ---- */

/* cbg(r,c) - Mancala-style checker border color. */
int cbg(r, c)
int r;
int c;
{
    if (((r / 2) + (c / 2)) & 1)
    {
        if (r & 1)
            return 42;  /* green   */
        return 43;      /* yellow  */
    }
    if (c & 1)
        return 41;      /* red     */
    return 44;          /* blue    */
}

/* brdr() - Draw a solid blue border around the screen. */
int brdr()
{
    int r;
    int c;

    setsgr(44);  /* blue bg */

    curmv(1, 1);
    for (c = 0; c < SCRW; c = c + 1)
        chout(' ');
    curmv(SCRH, 1);
    for (c = 0; c < SCRW; c = c + 1)
        chout(' ');

    for (r = 2; r < SCRH; r = r + 1)
    {
        curmv(r, 1);
        chout(' ');
        chout(' ');
        curmv(r, SCRW - 1);
        chout(' ');
        chout(' ');
    }
    rstcol();
    return 0;
}

int cell(on, col)
int on;
int col;
{
    if (on)
        setsgr(col);
    else
        rstcol();
    chout(' ');
    chout(' ');
    return 0;
}

char *glyph(ch, row)
int ch;
int row;
{
    if (ch == '0') return dig0[row];
    if (ch == '1') return dig1[row];
    if (ch == '2') return dig2[row];
    if (ch == '3') return dig3[row];
    if (ch == '4') return dig4[row];
    if (ch == '5') return dig5[row];
    if (ch == '6') return dig6[row];
    if (ch == '7') return dig7[row];
    if (ch == '8') return dig8[row];
    if (ch == '9') return dig9[row];
    if (ch == ':') return digc[row];
    return "     ";
}

int drg2(s, col)
char *s;
int col;
{
    int i;

    for (i = 0; i < 5; i = i + 1)
        cell(s[i] != ' ', col);
    rstcol();
    chout(' ');
    return 0;
}

/* ---- Layout ---- */

/* per-position bg colors: HH : MM  (colon gets white) */
int dcol[5];

int drall()
{
    int r;
    int i;

    timtxt[0] = gh0 + '0';
    timtxt[1] = gh1 + '0';
    timtxt[2] = ':';
    timtxt[3] = gm0 + '0';
    timtxt[4] = gm1 + '0';
    timtxt[5] = 0;

    /* Blink: hide colon on alternating seconds. */
    if (blink)
        timtxt[2] = ' ';

    for (r = 0; r < 7; r = r + 1)
    {
        curmv(BROW + r, BCOL);
        for (i = 0; i < 5; i = i + 1)
            drg2(glyph(timtxt[i], r), dcol[i]);
    }
    return 0;
}

/* ---- Setup ---- */

int setup()
{
    dig0[0] = " ### "; dig0[1] = "#   #"; dig0[2] = "#   #";
    dig0[3] = "#   #"; dig0[4] = "#   #"; dig0[5] = "#   #";
    dig0[6] = " ### ";
    dig1[0] = "  #  "; dig1[1] = " ##  "; dig1[2] = "  #  ";
    dig1[3] = "  #  "; dig1[4] = "  #  "; dig1[5] = "  #  ";
    dig1[6] = " ### ";
    dig2[0] = " ### "; dig2[1] = "#   #"; dig2[2] = "    #";
    dig2[3] = "  ## "; dig2[4] = " #   "; dig2[5] = "#    ";
    dig2[6] = "#####";
    dig3[0] = "#### "; dig3[1] = "    #"; dig3[2] = "    #";
    dig3[3] = " ### "; dig3[4] = "    #"; dig3[5] = "    #";
    dig3[6] = "#### ";
    dig4[0] = "#   #"; dig4[1] = "#   #"; dig4[2] = "#   #";
    dig4[3] = "#####"; dig4[4] = "    #"; dig4[5] = "    #";
    dig4[6] = "    #";
    dig5[0] = "#####"; dig5[1] = "#    "; dig5[2] = "#    ";
    dig5[3] = "#### "; dig5[4] = "    #"; dig5[5] = "    #";
    dig5[6] = "#### ";
    dig6[0] = " ### "; dig6[1] = "#    "; dig6[2] = "#    ";
    dig6[3] = "#### "; dig6[4] = "#   #"; dig6[5] = "#   #";
    dig6[6] = " ### ";
    dig7[0] = "#####"; dig7[1] = "    #"; dig7[2] = "   # ";
    dig7[3] = "  #  "; dig7[4] = " #   "; dig7[5] = " #   ";
    dig7[6] = " #   ";
    dig8[0] = " ### "; dig8[1] = "#   #"; dig8[2] = "#   #";
    dig8[3] = " ### "; dig8[4] = "#   #"; dig8[5] = "#   #";
    dig8[6] = " ### ";
    dig9[0] = " ### "; dig9[1] = "#   #"; dig9[2] = "#   #";
    dig9[3] = " ####"; dig9[4] = "    #"; dig9[5] = "    #";
    dig9[6] = " ### ";
    digc[0] = "     "; digc[1] = "  #  "; digc[2] = "  #  ";
    digc[3] = "     "; digc[4] = "  #  "; digc[5] = "  #  ";
    digc[6] = "     ";

    /* Rainbow gradient: walks the spectrum left to right.
     *   H1 red, H2 yellow, : white, M1 green, M2 cyan
     */
    dcol[0] = 101;  /* bright red     */
    dcol[1] = 103;  /* bright yellow  */
    dcol[2] = 107;  /* bright white   */
    dcol[3] = 102;  /* bright green   */
    dcol[4] = 106;  /* bright cyan    */

    ph0 = -1; ph1 = -1;
    pm0 = -1; pm1 = -1;
    blink = 0;

    return 0;
}

/* ---- Main loop ---- */

int main()
{
    int chgh, chgm, chgs;
    int key;
    int n;
    int tick;
    int hh;
    int sec;
    int nblink;

    setup();
    lofset();
    cls();
    hidecr();
    brdr();

    curmv(BROW + DHGT + 4, 30);
    if (offset >= 0)
        printf("\033[1;93mUTC offset: +%d h\033[0m", offset);
    else
        printf("\033[1;93mUTC offset: %d h\033[0m", offset);
    curmv(BROW + DHGT + 8, 30);
    printf("\033[1;93mPress ESC to quit\033[0m");

    tick = 0;
    tset(50);

    while (1)
    {
        if (texp())
        {
            tick = tick + 1;
            n = getime();

            /* require full ISO format with 'T' separator */
            if (n >= 19 && tbuf[10] == 'T')
            {
                gh0 = tbuf[11] - '0';
                gh1 = tbuf[12] - '0';
                gm0 = tbuf[14] - '0';
                gm1 = tbuf[15] - '0';
                sec = (tbuf[17] - '0') * 10 + (tbuf[18] - '0');

                /* UTC -> local using env OFFSET */
                hh = adjhr(gh0 * 10 + gh1);
                gh0 = hh / 10;
                gh1 = hh - gh0 * 10;

                chgh = (gh0 != ph0) || (gh1 != ph1);
                chgm = (gm0 != pm0) || (gm1 != pm1);
                nblink = sec & 1;
                chgs = (nblink != blink);
                blink = nblink;

                if (chgh || chgm || chgs)
                {
                    drall();
                    ph0 = gh0; ph1 = gh1;
                    pm0 = gm0; pm1 = gm1;
                }

                /* Refresh uptime once per second (when blink toggles). */
                if (chgs || chgm || chgh)
                    shoupt(BROW + DHGT + 6, 30);
            }
            else
            {
                /* No real wall-clock yet (SNTP not synced). Show
                 * what the host actually returned so the user can
                 * tell whether WiFi/NTP needs attention.
                 */
                curmv(BROW + 3, BCOL);
                printf("\033[1;91mWaiting for SNTP: \033[0m\033[K");
                printf("\033[1;97m%s\033[0m", tbuf);
            }
            tset(50);
        }

        key = bdos(6, 0xFF) & 0xFF;
        if (key == ESC || key == KCC)
            break;
    }

    rstcol();
    cls();
    shocr();
    return 0;
}
