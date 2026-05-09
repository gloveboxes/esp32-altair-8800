/*
 * onboard.c - Onboard sensor monitoring for Altair 8800
 * Version 1.2
 *
 * Reads temperature, pressure, humidity and system information
 * from the emulator's onboard sensors using I/O ports.
 * Also displays lwIP network stack statistics.
 * Real-time display using VT100/ANSI escape sequences.
 *
 * Usage: Press ESC to quit
 *
 * Based on onboard.bas - converted to BDS C 1.6
 *
 * BDS C Compiler and Libraries with Long Integer support
 * https://msxhub.com/BDSC
 *
 * To compile with BDS C:
 * cc onboard
 * clink onboard long
 */

#include <stdio.h>

/* VT100/ANSI Control */
#define ESC 27

/* Screen layout */
#define TOP 2
#define LEFT 3
#define WID 76
#define HGT 27
#define TX 7

/* Timer configuration */
#define TMRID 0    /* Use timer 0 */
#define DLYMS 5000 /* 5 second delay */

/* LWIP Stats port and data values */
#define STPORT 50
#define STHEAP 0
#define STPBUF 1
#define STSEG 2
#define STPCB 3

/* RFS Stats port and data values */
#define RFPORT 51 /* Remote FS stats port */
#define RFTYPE 0  /* Cache stats type */

/* Function prototypes */
int main();
int rdstr();
int cputs();
int chput();
int putnum();
int cls();
int cur();
int hide();
int show();
int getkey();
int dsplay();
int initd();
int rst();
int setfg();
int setbg();
int cbg();
int box();
int title();
int sect();
int fld();
int statln();

/* Timer library functions */
int x_delay();
int x_tset();
int x_texp();
int x_tact();

/* I/O port functions */
int bdos(), bios(), inp(), outp();

/* Global variables for sensor data */
char sbuf[256];
char pad[41]; /* Pre-allocated padding string */

/* Main program */
int main()
{
    int key, quit;
    char lcount[4];     /* Long reading counter */
    char lone[4];       /* Long constant 1 */
    char cntbuf[16];    /* Buffer for ltoa */

    itol(lcount, 0); /* Initialize counter to 0 */
    itol(lone, 1);   /* Initialize increment value */
    quit = 0;

    /* Initialize display and padding string */
    initd();

    while (!quit)
    {
        /* Check for quit key before doing any screen updates */
        key = getkey();
        if (key == ESC)
        {
            quit = 1;
            break; /* Exit main loop immediately */
        }

        /* Start accelerometer and increment counter */
        outp(64, 5);
        ladd(lcount, lcount, lone);

        /* Update reading count */
        cur(7, TX);
        setfg(36);
        cputs("Reading");
        rst();
        cur(7, TX + 17);
        setfg(37);
        ltoa(cntbuf, lcount);
        cputs(cntbuf);
        cputs(pad);
        rst();

        /* Display all sensor data */
        dsplay();

        /* Update status line */
        cur(26, TX);
        setfg(33);
        cputs("Status");
        rst();
        cur(26, TX + 17);
        setfg(37);
        cputs("Monitoring continuously... (5 sec delay)");
        cputs(pad);
        rst();

        /* Sleep using timer library with keyboard checking */
        x_tset(TMRID, DLYMS); /* Start timer with configured delay */
        while (x_tact(TMRID) && !quit)
        {
            /* Direct non-blocking read keeps ESC responsive during sleep */
            key = getkey();
            if (key == ESC)
            {
                quit = 1;
                break; /* Exit timer loop immediately */
            }
        }
    }

    /* Final status update */
    cur(26, TX + 17);
    setfg(37);
    cputs("User requested quit. Stopping...");
    cputs(pad);
    rst();

    /* Stop the accelerometer */
    outp(64, 4);

    /* Restore cursor and move to bottom */
    cur(30, 1);
    show();
    cputs("Sensor monitoring stopped by user.\r\n");

    return 0;
}

/*
 * Read string data from port 200 until null character
 * This is equivalent to the GOSUB 1600 subroutine in BASIC
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

/*
 * Output a number as ASCII string
 */
int putnum(n)
int n;
{
    char buffer[16];
    int i, digit, started;

    if (n == 0)
    {
        chput('0');
        return 0;
    }

    if (n < 0)
    {
        chput('-');
        n = -n;
    }

    /* Convert number to string (reverse order) */
    i = 0;
    while (n > 0)
    {
        buffer[i] = '0' + (n % 10);
        n = n / 10;
        i++;
    }

    /* Output digits in correct order */
    while (i > 0)
    {
        i--;
        chput(buffer[i]);
    }

    return 0;
}

/*
 * VT100/ANSI Screen Control Functions
 */

/* Clear entire screen and home cursor */
int cls()
{
    chput(ESC);
    cputs("[0m");
    chput(ESC);
    cputs("[2J");
    cur(1, 1);
    return 0;
}

/* Move cursor to row, col (1-based) */
int cur(row, col)
int row;
int col;
{
    chput(ESC);
    cputs("[");
    putnum(row);
    cputs(";");
    putnum(col);
    cputs("H");
    return 0;
}

/* Hide cursor */
int hide()
{
    chput(ESC);
    cputs("[?25l");
    return 0;
}

/* Show cursor */
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
    putnum(c);
    cputs("m");
    return 0;
}

/* Set ANSI background color. */
int setbg(c)
int c;
{
    chput(ESC);
    cputs("[");
    putnum(c);
    cputs("m");
    return 0;
}

/*
 * Keyboard Input Functions (Non-blocking)
 */

/* Get a key from keyboard without blocking. */
int getkey()
{
    return (bdos(6, 0xFF) & 0xFF);
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
    cputs("ONBOARD");
    rst();
    cputs(" Monitor");
    setfg(33);
    cputs("  Altair 8800");
    rst();
    cputs("  ");
    setfg(37);
    cputs("Press ESC to quit");
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
    cputs(pad);
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
    rdstr(sbuf, 255);
    cputs(sbuf);
    cputs(pad);
    rst();
    return 0;
}

/*
 * Optimized Functions
 */

/* Initialize display and pre-fill padding string */
int initd()
{
    int i;

    /* Initialize padding string once */
    for (i = 0; i < 8; i++)
    {
        pad[i] = ' ';
    }
    pad[8] = 0; /* Null terminate */

    /* Set up display */
    cls();
    hide();

    box();
    title();
    sect(9, "System");
    sect(12, "Uptime");
    sect(16, "lwIP Network");
    sect(23, "Remote FS");

    return 0;
}

/* Optimized sensor data display */
int dsplay()
{
    char luptime[4];
    char l3600[4], l60[4];
    char lhours[4], lrem[4];
    char lmins[4];
    char bhours[16], bmins[16];

    /* Get and display emulator version */
    outp(70, 0);
    rdstr(sbuf, 255);
    fld(10, "Emulator", sbuf);

    /* Get and display system uptime in seconds */
    outp(41, 1);
    rdstr(sbuf, 255);
    fld(13, "Seconds", sbuf);

    /* Calculate and display uptime in hours:minutes format using unsigned long */
    atol(luptime, sbuf);
    itol(l3600, 3600);
    itol(l60, 60);
    ldiv(lhours, luptime, l3600); /* lhours = luptime / 3600 */
    lmod(lrem, luptime, l3600);   /* lrem   = luptime % 3600 */

    ldiv(lmins, lrem, l60); /* lmins = (luptime % 3600) / 60 */

    cur(14, TX);
    setfg(36);
    cputs("Hours:mins");
    rst();
    cur(14, TX + 17);
    setfg(37);
    ltoa(bhours, lhours);
    ltoa(bmins, lmins);
    cputs(bhours);
    cputs(":");

    /* Pad minutes with leading zero if less than 10 */
    if (atoi(bmins) < 10)
    {
        cputs("0");
    }
    cputs(bmins);
    cputs(pad);
    rst();

    /* Get and display lwIP network statistics */
    statln(17, STHEAP);
    statln(18, STPBUF);
    statln(19, STSEG);
    statln(20, STPCB);

    /* RFS Cache stats */
    cur(24, TX);
    setfg(37);
    outp(RFPORT, RFTYPE);
    rdstr(sbuf, 255);
    cputs(sbuf);
    cputs(pad);
    rst();

    return 0;
}
