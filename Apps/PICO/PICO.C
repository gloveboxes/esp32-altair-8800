/*
 * pico.c - Pico Stats Display Tool
 *
 * Displays system, lwIP network and Remote FS statistics
 * from the emulator's onboard sensors using I/O ports.
 *
 * To compile with BDS C:
 * cc pico
 * clink pico

 * Pico Stats support for Altair 8800
 * BDS C 1.6 on CP/M
 *
 * Rewritten for BDS C constraints:
 *  - All symbols unique within first 7 characters
 *  - K&R style definitions only
 *  - No support for casts
 *  - No support for goto labels named 'end'
 */

#include <stdio.h>

/* VT100/ANSI control */
#define ESC 27

/* Screen layout */
#define TOP 2
#define LEFT 3
#define WID 76
#define HGT 27
#define TX 7

/* LWIP Stats port and data values */
#define STPORT 50
#define STHEAP 0
#define STPBUF 1
#define STSEG 2
#define STPCB 3

/* RFS Stats port and data values */
#define RFPORT 51 /* Remote FS stats port */
#define RFTYPE 0  /* Cache stats type */

/* Global buffer */
char buffer[256];

/* Function prototypes */
int main();
int rdstr();
int cputs();
int chput();
int pnum();
int cur();
int cls();
int hide();
int show();
int rst();
int setfg();
int setbg();
int cbg();
int box();
int title();
int sect();
int fld();
int statln();
int bios(), bdos(), inp(), outp();
int atol(), itol(), ldiv(), lmod(), ltoa();

/* Long integer helper variables */
char luptime[4];
char l3600[4], l60[4];
char lhours[4], lrem[4], lmins[4];
char bufnum[16];

int main()
{
    cls();
    hide();
    box();
    title();

    sect(7, "System");

    /* Get Device ID - Port 46, data 0 */
    outp(46, 0);
    rdstr(buffer, 255);
    fld(8, "Hostname", buffer);

    /* Get WiFi IP Address - Port 46, data 1 */
    outp(46, 1);
    rdstr(buffer, 255);
    fld(9, "WiFi IP", buffer);

    /* Get Physical Device ID - Port 46, data 2 */
    outp(46, 2);
    rdstr(buffer, 255);
    fld(10, "Device ID", buffer);

    /* Get Emulator Version - Port 70 */
    outp(70, 0);
    rdstr(buffer, 255);
    fld(11, "Emulator", buffer);

    /* Get Uptime - Port 41 (returns seconds string) */
    outp(41, 1);
    rdstr(buffer, 255);
    sect(13, "Uptime");
    fld(14, "Seconds", buffer);

    /* Parse uptime to long for calculation */
    atol(luptime, buffer);
    itol(l3600, 3600);
    itol(l60, 60);

    /* Calculate Hours: luptime / 3600 */
    ldiv(lhours, luptime, l3600);

    /* Calculate Remainder: luptime % 3600 */
    lmod(lrem, luptime, l3600);

    /* Calculate Minutes: remainder / 60 */
    ldiv(lmins, lrem, l60);

    cur(15, TX);
    setfg(36);
    cputs("Hours:mins");
    rst();
    cur(15, TX + 17);
    setfg(37);
    ltoa(bufnum, lhours);
    cputs(bufnum);
    chput(':');

    /* Format minutes with leading zero if needed */
    ltoa(bufnum, lmins);
    if (bufnum[1] == 0) /* Single digit? */
    {
        chput('0');
    }
    cputs(bufnum);
    rst();

    /* lwIP Network Statistics */
    sect(18, "lwIP Network");

    /* Heap stats */
    statln(19, STHEAP);

    /* PBUF pool stats */
    statln(20, STPBUF);

    /* TCP Segment stats */
    statln(21, STSEG);

    /* TCP PCB stats */
    statln(22, STPCB);

    /* Remote FS Statistics */
    sect(24, "Remote FS");

    /* RFS Cache stats */
    cur(25, TX);
    setfg(37);
    outp(RFPORT, RFTYPE);
    rdstr(buffer, 255);
    cputs(buffer);
    rst();

    cur(30, 1);
    show();

    return 0;
}

/*
 * Read string data from port 200 until null character
 */
int rdstr(buf, max)
char* buf;
int max;
{
    int i, ch;

    i = 0;
    ch = inp(200);

    while (ch != 0 && i < max - 1)
    {
        buf[i] = ch;
        i++;
        ch = inp(200);
    }

    buf[i] = 0; /* Null terminate */
    return i;
}

/*
 * Output a string to console
 */
int cputs(s)
char* s;
{
    while (*s)
    {
        chput(*s);
        s++;
    }
    return 0;
}

/*
 * Output a character to console using BIOS
 */
int chput(c)
char c;
{
    return bios(4, c);
}

/* Print a non-negative decimal number. */
int pnum(n)
int n;
{
    char b[6];
    int i;

    if (n == 0)
    {
        chput('0');
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
        chput(b[i]);
    }
    return 0;
}

/* Move cursor to one-based row and column. */
int cur(r, c)
int r;
int c;
{
    chput(ESC);
    cputs("[");
    pnum(r);
    cputs(";");
    pnum(c);
    cputs("H");
    return 0;
}

/* Clear screen and reset attributes. */
int cls()
{
    chput(ESC);
    cputs("[0m");
    chput(ESC);
    cputs("[2J");
    cur(1, 1);
    return 0;
}

/* Hide terminal cursor. */
int hide()
{
    chput(ESC);
    cputs("[?25l");
    return 0;
}

/* Show terminal cursor. */
int show()
{
    chput(ESC);
    cputs("[?25h");
    return 0;
}

/* Reset terminal attributes. */
int rst()
{
    chput(ESC);
    cputs("[0m");
    return 0;
}

/* Set ANSI foreground color. */
int setfg(c)
int c;
{
    chput(ESC);
    cputs("[");
    pnum(c);
    cputs("m");
    return 0;
}

/* Set ANSI background color. */
int setbg(c)
int c;
{
    chput(ESC);
    cputs("[");
    pnum(c);
    cputs("m");
    return 0;
}

/* Checker color for the cabinet border. */
int cbg(r, c)
int r;
int c;
{
    if (((r / 2) + (c / 2)) & 1)
        return 100;
    return 107;
}

/* Draw a white and gray checked dashboard frame. */
int box()
{
    int r;
    int c;

    for (r = 0; r < HGT; r++)
    {
        cur(TOP + r, LEFT);
        for (c = 0; c < WID; c++)
        {
            if (r < 2 || r >= HGT - 2 || c < 2 || c >= WID - 2)
            {
                setbg(cbg(r, c));
                chput(' ');
            }
            else
            {
                rst();
                chput(' ');
            }
        }
    }
    rst();
    return 0;
}

/* Draw the screen title. */
int title()
{
    cur(4, TX);
    setfg(36);
    cputs("PICO");
    rst();
    cputs(" Stats");
    setfg(33);
    cputs("  Altair 8800");
    rst();

    cur(5, TX);
    setfg(37);
    cputs("Onboard system, network and Remote FS status");
    rst();
    return 0;
}

/* Draw a section heading. */
int sect(row, txt)
int row;
char *txt;
{
    cur(row, TX);
    setfg(33);
    cputs(txt);
    rst();
    cur(row, TX + 13);
    setfg(37);
    cputs("-------------------------------------------------------");
    rst();
    return 0;
}

/* Draw a labelled field. */
int fld(row, lab, val)
int row;
char *lab;
char *val;
{
    cur(row, TX);
    setfg(36);
    cputs(lab);
    rst();
    cur(row, TX + 17);
    setfg(37);
    cputs(val);
    rst();
    return 0;
}

/* Read and draw one lwIP stats line. */
int statln(row, typ)
int row;
int typ;
{
    cur(row, TX);
    setfg(37);
    outp(STPORT, typ);
    rdstr(buffer, 255);
    cputs(buffer);
    rst();
    return 0;
}
