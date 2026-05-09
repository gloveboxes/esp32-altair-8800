/*
 * EDIT.C - Small nano-inspired text editor for BDS C.
 *
 * Runs on CP/M under the Altair 8800 emulator using VT100/xterm.js
 * terminal control.  Built for BDS C 1.6 constraints.
 * Terminal: 80 columns x 30 rows.
 *
 * Uses alloc/free for line storage so average-length lines
 * use much less memory than a fixed 2D array.  Supports
 * files up to 2000 lines.
 *
 * To compile with BDS C:
 * cc edit
 * clink edit
 *
 * Keys:
 *   Arrow keys       Move cursor
 *   Ctrl-O          Write file
 *   ESC/Ctrl-Q      Exit
 *   Ctrl-K          Cut current line
 *   Ctrl-C          Copy current line
 *   Ctrl-U          Paste (uncut) line
 *   Ctrl-F          Find
 *   Ctrl-N          Find next
 *   Ctrl-W          Help
 *   Ctrl-R/V        Page up / Page down
 *   Ctrl-T/B        Go to top / bottom
 *   Backspace       Delete left
 *   Delete          Delete right
 *   Enter           Split line
 */

#include <stdio.h>

#define COLS 80
#define TEXTY 2
#define TEXTN 27
#define STATY 29
#define HELPY 30

#define MAXLN 2000
#define MAXCL 126
#define GUTW 4

#define ESC 27
#define CTLF 6
#define CTLN 14
#define CTLW 23
#define CTLK 11
#define CTLO 15
#define CTLU 21
#define CTLC 3
#define CTLQ 17
#define BKSP 8
#define TAB 9
#define DEL 127
#define CPMEOF 26
#define CR 13
#define LF 10
#define KUP 5
#define KDN 24
#define KRT 4
#define KLT 19
#define KDEL 7
#define KPGU 18
#define KPGD 22
#define KTOP 20
#define KBOT 2
#define KINS 9

char *ln[MAXLN];
char cutb[MAXCL + 1];
char sbuf[MAXCL + 1];
char ebuf[MAXCL + 2];
char name[32];
char mesg[70];

int lcnt;
int crow;
int ccol;
int top;
int lft;
int dirt;
int quit;
int rall;
int dlin;
int srow;
int scol;
int slcnt;
int dtitle;
int dstat;
int dhelp;
int ovrm;
int evsav;
int eflg;
unsigned used;

int bdos();
int bios();
int fclose();
int fgetc();
int fputc();
int strlen();
FILE *fopen();
char *strcpy();
char *strcat();
char *alloc();
int free();

/* ndig(n) - Count decimal digits in a positive number. */
int ndig(n)
int n;
{
    int d;

    d = 1;
    while (n >= 10)
    {
        d++;
        n = n / 10;
    }
    return d;
}

/* chout(c) - Write one console character. */
int chout(c)
char c;
{
    return bios(4, c);
}

/* cput(s) - Write a console string. */
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

/* nump(n) - Print a positive decimal number. */
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

/* lnpad(n) - Print line number right-justified in gutter. */
int lnpad(n)
int n;
{
    if (n < 10)
        cput("  ");
    else if (n < 100)
        chout(' ');
    nump(n);
    chout(' ');
    return 0;
}

/* curmv(r,c) - Move cursor to one-based row and column. */
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

/* clrs() - Clear the terminal. */
int clrs()
{
    chout(ESC);
    cput("[2J");
    curmv(1, 1);
    return 0;
}

/* eol() - Erase from cursor to end of line. */
int eol()
{
    chout(ESC);
    cput("[K");
    return 0;
}

/* invon() - Start reverse video. */
int invon()
{
    chout(ESC);
    cput("[7m");
    return 0;
}

/* invof() - End reverse video. */
int invof()
{
    chout(ESC);
    cput("[0m");
    return 0;
}

/* shwcr() - Show cursor as a steady block. */
int shwcr()
{
    chout(ESC);
    cput("[?25h");
    return 0;
}

/* hidcr() - Hide the cursor. */
int hidcr()
{
    chout(ESC);
    cput("[?25l");
    return 0;
}

/* xalloc(s) - Allocate and copy a string, track usage. */
char *xalloc(s)
char *s;
{
    char *p;
    int len;

    len = strlen(s);
    p = alloc(len + 1);
    if (p == 0)
        return 0;
    strcpy(p, s);
    used = used + len + 3;
    return p;
}

/* xfree(p) - Free a line and track usage. */
int xfree(p)
char *p;
{
    if (p)
    {
        used = used - strlen(p) - 3;
        free(p);
    }
    return 0;
}

/* fremem() - Approximate free heap bytes. */
unsigned fremem()
{
    unsigned tot;

    tot = topofmem() - 0x4325;
    if (used >= tot)
        return 0;
    return tot - used;
}

/* fmshow() - Print free memory label to console. */
int fmshow()
{
    unsigned f;

    f = fremem();
    cput(" Free ");
    if (f >= 1024)
    {
        nump(f / 1024);
        cput("K");
    }
    else
    {
        nump(f);
        cput("B");
    }
    return 0;
}

/* lnset(r,s) - Set line r to a copy of string s. */
int lnset(r, s)
int r;
char *s;
{
    char *p;

    p = xalloc(s);
    if (p == 0)
    {
        msg("Out of memory");
        return -1;
    }
    if (ln[r])
        xfree(ln[r]);
    ln[r] = p;
    return 0;
}

/* msg(s) - Set the status message. */
int msg(s)
char *s;
{
    int i;
    int same;

    same = 1;
    i = 0;
    while (i < 68)
    {
        if (mesg[i] != s[i])
        {
            same = 0;
            break;
        }
        if (s[i] == 0)
            break;
        i++;
    }

    if (same)
        return 0;

    i = 0;
    while (s[i] && i < 68)
    {
        mesg[i] = s[i];
        i++;
    }
    mesg[i] = 0;
    dstat = 1;
    return 0;
}

/* keywt() - Wait for and return one raw key byte. */
int keywt()
{
    int c;

    c = 0;
    while (c == 0)
    {
        c = bdos(6, 255) & 255;
    }
    return c;
}

/* keyrd() - Read one key. Emulator translates arrows to WordStar. */
int keyrd()
{
    return keywt();
}

/* zerob() - Create an empty text buffer. */
int zerob()
{
    int i;

    for (i = 0; i < MAXLN; i++)
    {
        if (ln[i])
            xfree(ln[i]);
        ln[i] = 0;
    }

    ebuf[0] = 0;
    used = 0;
    ln[0] = xalloc("");
    lcnt = 1;
    crow = 0;
    ccol = 0;
    top = 0;
    lft = 0;
    dirt = 0;
    rall = 1;
    dlin = -1;
    srow = -1;
    scol = -1;
    slcnt = -1;
    dtitle = 1;
    dstat = 1;
    dhelp = 1;
    ovrm = 0;
    evsav = 0;
    eflg = 0;
    cutb[0] = 0;
    sbuf[0] = 0;
    return 0;
}

/* setdirt(v) - Update modified state and title redraw. */
int setdirt(v)
int v;
{
    if (dirt != v)
        dtitle = 1;
    dirt = v;
    return 0;
}

/* getfcb(d) - Build filename from CP/M default FCB at 0x5C. */
int getfcb(d)
char *d;
{
    char *fcb;
    int i;
    int j;

    fcb = 0x5C;
    j = 0;

    for (i = 1; i <= 8; i++)
    {
        if (fcb[i] != ' ')
            d[j++] = fcb[i];
    }

    if (fcb[9] != ' ')
    {
        d[j++] = '.';
        for (i = 9; i <= 11; i++)
        {
            if (fcb[i] != ' ')
                d[j++] = fcb[i];
        }
    }

    d[j] = 0;
    return (j > 0);
}

/* mkbakn(dst,src) - Build .BAK filename from src into dst. */
int mkbakn(dst, src)
char *dst;
char *src;
{
    int i;
    int j;

    j = 0;
    for (i = 0; src[i] && src[i] != '.'; i++)
    {
        if (j < 8)
            dst[j++] = src[i];
    }
    dst[j++] = '.';
    dst[j++] = 'B';
    dst[j++] = 'A';
    dst[j++] = 'K';
    dst[j] = 0;
    return 0;
}

/* mkbak() - Rename current file to .BAK. */
int mkbak()
{
    char fcb[36];
    int i;
    int j;

    for (i = 0; i < 36; i++)
        fcb[i] = 0;

    j = 0;
    for (i = 0; name[i] && name[i] != '.'; i++)
    {
        if (j < 8)
            fcb[1 + j++] = name[i];
    }
    while (j < 8)
        fcb[1 + j++] = ' ';

    if (name[i] == '.')
    {
        i++;
        j = 0;
        while (name[i] && j < 3)
        {
            fcb[9 + j++] = name[i++];
        }
    }
    while (j < 3)
        fcb[9 + j++] = ' ';

    for (i = 1; i <= 8; i++)
        fcb[16 + i] = fcb[i];

    fcb[25] = 'B';
    fcb[26] = 'A';
    fcb[27] = 'K';

    bdos(19, fcb + 16);
    bdos(23, fcb);
    return 0;
}

/* unbak() - Rename .BAK back to original filename. */
int unbak()
{
    char fcb[36];
    int i;
    int j;

    for (i = 0; i < 36; i++)
        fcb[i] = 0;

    /* Source: .BAK name */
    j = 0;
    for (i = 0; name[i] && name[i] != '.'; i++)
    {
        if (j < 8)
            fcb[1 + j++] = name[i];
    }
    while (j < 8)
        fcb[1 + j++] = ' ';

    fcb[9] = 'B';
    fcb[10] = 'A';
    fcb[11] = 'K';

    /* Dest: original name */
    j = 0;
    for (i = 0; name[i] && name[i] != '.'; i++)
    {
        if (j < 8)
            fcb[17 + j++] = name[i];
    }
    while (j < 8)
        fcb[17 + j++] = ' ';

    if (name[i] == '.')
    {
        i++;
        j = 0;
        while (name[i] && j < 3)
        {
            fcb[25 + j++] = name[i++];
        }
    }
    while (j < 3)
        fcb[25 + j++] = ' ';

    bdos(23, fcb);
    return 0;
}

/* loadf(fn) - Load a text file into the line buffer. */
int loadf(fn)
char *fn;
{
    FILE *fp;
    int c;
    int p;
    char bfn[16];

    zerob();

    mkbak();
    mkbakn(bfn, fn);
    fp = fopen(bfn, "r");

    if (fp == NULL)
    {
        msg("New file");
        return 0;
    }

    p = 0;
    lcnt = 0;
    ebuf[0] = 0;

    while ((c = fgetc(fp)) != EOF)
    {
        if (c == CPMEOF)
            break;
        if (c == CR)
            continue;
        if (c == LF)
        {
            ebuf[p] = 0;
            if (lcnt < MAXLN)
            {
                if (lnset(lcnt, ebuf) < 0)
                {
                    msg("Truncated - out of memory");
                    break;
                }
                lcnt++;
            }
            p = 0;
            ebuf[0] = 0;
        }
        else if (c == TAB)
        {
            do
            {
                if (p < MAXCL)
                {
                    ebuf[p] = ' ';
                    p++;
                }
            } while ((p % 8) != 0 && p < MAXCL);
            ebuf[p] = 0;
        }
        else
        {
            if (p < MAXCL)
            {
                ebuf[p] = c;
                p++;
                ebuf[p] = 0;
            }
        }
    }

    if (p > 0 || lcnt == 0)
    {
        ebuf[p] = 0;
        if (lcnt < MAXLN)
        {
            if (lnset(lcnt, ebuf) < 0)
            {
                fclose(fp);
                setdirt(0);
                msg("Truncated - out of memory");
                return 0;
            }
            lcnt++;
        }
    }

    fclose(fp);
    setdirt(0);
    msg("File loaded");
    return 0;
}

/* savef() - Write the buffer to disk. */
int savef()
{
    FILE *fp;
    int r;
    int c;

    if (name[0] == 0)
    {
        msg("No filename");
        return -1;
    }

    fp = fopen(name, "w");
    if (fp == NULL)
    {
        msg("Cannot write file");
        return -1;
    }

    for (r = 0; r < lcnt; r++)
    {
        c = 0;
        while (ln[r][c])
        {
            if (fputc(ln[r][c], fp) == EOF)
            {
                fclose(fp);
                msg("Disk full - save failed");
                return -1;
            }
            c++;
        }
        fputc(CR, fp);
        fputc(LF, fp);
    }

    fclose(fp);
    setdirt(0);
    evsav = 1;
    msg("Wrote file");
    return 0;
}

/* fixcur() - Clamp cursor and scrolling state. */
int fixcur()
{
    int len;

    if (crow < 0)
        crow = 0;
    if (crow >= lcnt)
        crow = lcnt - 1;

    len = strlen(ln[crow]);
    if (ccol < 0)
        ccol = 0;
    if (ccol > len)
        ccol = len;

    if (crow < top)
        top = crow;
    if (crow >= top + TEXTN)
        top = crow - TEXTN + 1;
    if (top < 0)
        top = 0;

    if (ccol < lft)
        lft = ccol;
    if (ccol >= lft + COLS - GUTW)
        lft = ccol - (COLS - GUTW) + 1;
    if (lft < 0)
        lft = 0;

    return 0;
}

/* title() - Draw the top editor title bar. */
int title()
{
    curmv(1, 1);
    invon();
    cput(" EDIT v4 ");
    if (name[0])
        cput(name);
    else
        cput("[No Name]");
    if (dirt)
        cput(" *");
    eol();
    invof();
    return 0;
}

/* drawln(n) - Draw one visible editor row. */
int drawln(n)
int n;
{
    int l;
    int p;
    int x;

    l = top + n;
    curmv(TEXTY + n, 1);

    if (l >= lcnt)
    {
        eol();
        return 0;
    }

    lnpad(l + 1);
    p = lft;
    x = 0;
    while (ln[l][p] && x < COLS - GUTW)
    {
        if (ln[l][p] == TAB)
        {
            do
            {
                chout(' ');
                x++;
            } while ((x % 8) != 0 && x < COLS - GUTW);
        }
        else
        {
            chout(ln[l][p]);
            x++;
        }
        p++;
    }
    eol();
    return 0;
}

/* statln() - Draw the full status bar. */
int statln()
{
    curmv(STATY, 1);
    invon();
    cput(" ");
    cput(mesg);
    cput("  Ln ");
    nump(crow + 1);
    cput("/");
    nump(lcnt);
    cput(" Col ");
    nump(ccol + 1);
    if (ovrm)
        cput(" OVR");
    else
        cput(" INS");
    fmshow();
    eol();
    invof();
    return 0;
}

/* statcol() - Update only the changing column field. */
int statcol()
{
    int col;

    col = 13 + strlen(mesg) + ndig(crow + 1) + ndig(lcnt);
    curmv(STATY, col);
    invon();
    nump(ccol + 1);
    if (ovrm)
        cput(" OVR");
    else
        cput(" INS");
    fmshow();
    eol();
    invof();
    return 0;
}

/* helpbar() - Draw the shortcut bar. */
int helpbar()
{
    curmv(HELPY, 1);
    cput("^W Help ^O Write ^Q Exit ^F Find ^N Next ^K Cut ^C Copy ^U Paste");
    eol();
    return 0;
}

/* redrw() - Repaint the full screen. */
int redrw()
{
    int i;
    int ot;
    int ol;
    int sm;

    ot = top;
    ol = lft;
    fixcur();

    sm = 0;
    if (dstat || crow != srow || lcnt != slcnt)
        sm = 1;
    else if (ccol != scol)
        sm = 2;

    if (rall || top != ot || lft != ol)
    {
        hidcr();
        if (dtitle)
        {
            title();
            dtitle = 0;
        }
        for (i = 0; i < TEXTN; i++)
            drawln(i);
        rall = 0;
        dlin = -1;
    }
    else
    {
        if (dlin >= top && dlin < top + TEXTN)
        {
            hidcr();
            drawln(dlin - top);
        }
        dlin = -1;

        if (dtitle)
        {
            title();
            dtitle = 0;
        }
    }

    if (sm == 1)
    {
        statln();
        dstat = 0;
    }
    else if (sm == 2)
    {
        statcol();
    }

    if (dhelp)
    {
        helpbar();
        dhelp = 0;
    }

    srow = crow;
    scol = ccol;
    slcnt = lcnt;
    curmv(TEXTY + crow - top, ccol - lft + 1 + GUTW);
    shwcr();
    return 0;
}

/* insrow(r) - Insert an empty row before r. */
int insrow(r)
int r;
{
    int i;
    char *p;

    if (lcnt >= MAXLN)
        return -1;

    p = xalloc("");
    if (p == 0)
        return -1;

    for (i = lcnt; i > r; i--)
        ln[i] = ln[i - 1];

    ln[r] = p;
    lcnt++;
    return 0;
}

/* delrow(r) - Delete row r. */
int delrow(r)
int r;
{
    int i;
    char *p;

    if (lcnt <= 1)
    {
        p = xalloc("");
        if (p == 0)
        {
            msg("Out of memory");
            return -1;
        }
        if (ln[0])
            xfree(ln[0]);
        ln[0] = p;
        ccol = 0;
        return 0;
    }

    xfree(ln[r]);
    for (i = r; i < lcnt - 1; i++)
        ln[i] = ln[i + 1];

    ln[lcnt - 1] = 0;
    lcnt--;
    if (crow >= lcnt)
        crow = lcnt - 1;
    return 0;
}

/* insch(c) - Insert printable character at cursor. */
int insch(c)
int c;
{
    int len;
    int i;

    len = strlen(ln[crow]);
    if (len >= MAXCL)
    {
        msg("Line too long");
        return -1;
    }

    strcpy(ebuf, ln[crow]);
    for (i = len; i >= ccol; i--)
        ebuf[i + 1] = ebuf[i];

    ebuf[ccol] = c;
    if (lnset(crow, ebuf) < 0)
        return -1;
    ccol++;
    setdirt(1);
    msg("");
    return 0;
}

/* delch() - Delete character under cursor. */
int delch()
{
    int len;
    int i;

    len = strlen(ln[crow]);
    if (ccol < len)
    {
        strcpy(ebuf, ln[crow]);
        for (i = ccol; i < len; i++)
            ebuf[i] = ebuf[i + 1];
        lnset(crow, ebuf);
        setdirt(1);
        msg("");
        return 0;
    }

    if (crow + 1 < lcnt)
    {
        if (len + strlen(ln[crow + 1]) <= MAXCL)
        {
            strcpy(ebuf, ln[crow]);
            strcat(ebuf, ln[crow + 1]);
            if (lnset(crow, ebuf) < 0)
                return -1;
            delrow(crow + 1);
            setdirt(1);
            msg("");
        }
        else
            msg("Join would be too long");
    }
    return 0;
}

/* baksp() - Backspace/delete left. */
int baksp()
{
    int old;

    if (ccol > 0)
    {
        ccol--;
        delch();
        return 0;
    }

    if (crow > 0)
    {
        old = strlen(ln[crow - 1]);
        if (old + strlen(ln[crow]) <= MAXCL)
        {
            strcpy(ebuf, ln[crow - 1]);
            strcat(ebuf, ln[crow]);
            if (lnset(crow - 1, ebuf) < 0)
                return -1;
            crow--;
            delrow(crow + 1);
            ccol = old;
            setdirt(1);
            msg("");
        }
        else
            msg("Join would be too long");
    }
    return 0;
}

/* newln() - Split the current line. */
int newln()
{
    int len;

    if (lcnt >= MAXLN)
    {
        msg("Buffer full");
        return -1;
    }

    len = strlen(ln[crow]);
    if (insrow(crow + 1) < 0)
    {
        msg("Out of memory");
        return -1;
    }

    if (lnset(crow + 1, &ln[crow][ccol]) < 0)
    {
        delrow(crow + 1);
        return -1;
    }
    strcpy(ebuf, ln[crow]);
    ebuf[ccol] = 0;
    if (lnset(crow, ebuf) < 0)
    {
        delrow(crow + 1);
        return -1;
    }

    crow++;
    ccol = 0;
    setdirt(1);
    msg("");
    return 0;
}

/* cutln() - Cut the current line. */
int cutln()
{
    strcpy(cutb, ln[crow]);
    delrow(crow);
    ccol = 0;
    setdirt(1);
    msg("Cut line");
    return 0;
}

/* cpyln() - Copy the current line to cut buffer. */
int cpyln()
{
    strcpy(cutb, ln[crow]);
    msg("Copied line");
    return 0;
}

/* uncut() - Insert the cut buffer above the current line. */
int uncut()
{
    if (cutb[0] == 0)
    {
        msg("Cut buffer empty");
        return 0;
    }

    if (insrow(crow) == 0)
    {
        lnset(crow, cutb);
        ccol = 0;
        setdirt(1);
        msg("Uncut line");
    }
    else
        msg("Buffer full");

    return 0;
}

/* prompt(dst,pr) - Read a string from the status line. */
int prompt(dst, pr)
char *dst;
char *pr;
{
    int c;
    int p;

    curmv(STATY, 1);
    invon();
    cput(" ");
    cput(pr);
    eol();
    invof();
    curmv(STATY, strlen(pr) + 2);
    shwcr();

    p = strlen(dst);
    cput(dst);

    while (1)
    {
        c = keywt();
        if (c == CR || c == LF)
        {
            dst[p] = 0;
            dstat = 1;
            return 1;
        }
        if (c == ESC || c == CTLQ)
        {
            dstat = 1;
            return 0;
        }
        if (c == BKSP || c == DEL)
        {
            if (p > 0)
            {
                p--;
                dst[p] = 0;
                curmv(STATY, strlen(pr) + 2);
                cput(dst);
                eol();
            }
        }
        else if (c >= 32 && c <= 126 && p < MAXCL)
        {
            dst[p] = c;
            p++;
            dst[p] = 0;
            chout(c);
        }
    }
}

/* strmch(hay,ndl) - Find needle in haystack, return offset or -1. */
int strmch(hay, ndl)
char *hay;
char *ndl;
{
    int i;
    int j;
    int nlen;

    nlen = strlen(ndl);
    if (nlen == 0)
        return -1;

    i = 0;
    while (hay[i])
    {
        j = 0;
        while (ndl[j] && hay[i + j] == ndl[j])
            j++;
        if (j == nlen)
            return i;
        i++;
    }
    return -1;
}

/* findnx() - Find next occurrence of sbuf from cursor, wrapping. */
int findnx()
{
    int r;
    int c;
    int m;
    int wrap;

    if (sbuf[0] == 0)
        return 0;

    r = crow;
    c = ccol + 1;
    wrap = 0;

    while (1)
    {
        if (r >= lcnt)
        {
            if (wrap)
            {
                msg("Not found");
                return 0;
            }
            r = 0;
            c = 0;
            wrap = 1;
        }

        if (wrap && r > crow)
        {
            msg("Not found");
            return 0;
        }

        if (wrap && r == crow && c > ccol)
        {
            msg("Not found");
            return 0;
        }

        if (c <= strlen(ln[r]))
        {
            m = strmch(&ln[r][c], sbuf);
            if (m >= 0)
            {
                crow = r;
                ccol = c + m;
                if (wrap)
                    msg("Wrapped");
                else
                    msg("");
                rall = 1;
                return 1;
            }
        }

        r++;
        c = 0;
    }
}

/* dofind() - Prompt for search string. */
int dofind()
{
    char old[MAXCL + 1];

    strcpy(old, sbuf);
    sbuf[0] = 0;
    if (!prompt(sbuf, "Find:"))
    {
        strcpy(sbuf, old);
        msg("");
        return 0;
    }

    if (sbuf[0] == 0)
    {
        strcpy(sbuf, old);
        msg("");
        return 0;
    }

    ccol = -1;
    findnx();
    return 0;
}

/* help() - Show a compact help page. */
int help()
{
    clrs();
    curmv(2, 4);
    cput("EDIT help");
    curmv(4, 4);
    cput("Arrow keys   Move cursor");
    curmv(5, 4);
    cput("Ctrl-R/V     Page up / Page down");
    curmv(6, 4);
    cput("Ctrl-T/B     Go to top / bottom");
    curmv(7, 4);
    cput("Ctrl-O       Write file to disk");
    curmv(8, 4);
    cput("ESC/Ctrl-Q   Exit editor");
    curmv(9, 4);
    cput("Ctrl-K       Cut current line");
    curmv(10, 4);
    cput("Ctrl-C       Copy current line");
    curmv(11, 4);
    cput("Ctrl-U       Paste (uncut) line");
    curmv(12, 4);
    cput("Ctrl-F       Find");
    curmv(13, 4);
    cput("Ctrl-N       Find next");
    curmv(14, 4);
    cput("Backspace    Delete char left");
    curmv(15, 4);
    cput("Delete       Delete char right");
    curmv(16, 4);
    cput("Enter        Split line");
    curmv(18, 4);
    cput("Press any key to return.");
    keywt();
    clrs();
    dtitle = 1;
    dstat = 1;
    dhelp = 1;
    return 0;
}

/* movek(c) - Apply cursor motion key. */
int movek(c)
int c;
{
    int len;

    if (c == KUP)
        crow--;
    else if (c == KDN)
        crow++;
    else if (c == KLT)
    {
        if (ccol > 0)
            ccol--;
        else if (crow > 0)
        {
            crow--;
            ccol = strlen(ln[crow]);
        }
    }
    else if (c == KRT)
    {
        len = strlen(ln[crow]);
        if (ccol < len)
            ccol++;
        else if (crow + 1 < lcnt)
        {
            crow++;
            ccol = 0;
        }
    }
    return 0;
}

/* askxit() - Confirm exit when modified. */
int askxit()
{
    int c;

    if (!dirt)
        return 1;

    msg("Modified buffer. Exit without saving? y/N");
    redrw();
    c = keywt();
    if (c == 'y' || c == 'Y')
        return 1;

    msg("Exit cancelled");
    return 0;
}

/* dobin(c) - Dispatch one key. */
int dobin(c)
int c;
{
    /* Consume stray escape sequences (ESC [ A/B/C/D) */
    if (eflg == 2)
    {
        eflg = 0;
        if (c == 'A') { movek(KUP); return 0; }
        if (c == 'B') { movek(KDN); return 0; }
        if (c == 'C') { movek(KRT); return 0; }
        if (c == 'D') { movek(KLT); return 0; }
        /* Not a recognized sequence letter, process normally */
    }
    if (eflg == 1)
    {
        eflg = 0;
        if (c == '[')
        {
            eflg = 2;
            return 0;
        }
        /* Not '[', process normally */
    }

    if (c == CTLW)
    {
        help();
        msg("");
        rall = 1;
    }
    else if (c == CTLF)
    {
        dofind();
        rall = 1;
    }
    else if (c == CTLN)
    {
        if (sbuf[0])
            findnx();
        else
            msg("No search term - use Ctrl-F");
        rall = 1;
    }
    else if (c == CTLO)
    {
        msg("Writing...");
        redrw();
        savef();
        rall = 1;
    }
    else if (c == ESC || c == CTLQ)
    {
        if (askxit())
            quit = 1;
        rall = 1;
    }
    else if (c == CTLK)
    {
        cutln();
        rall = 1;
    }
    else if (c == CTLC)
        cpyln();
    else if (c == CTLU)
    {
        uncut();
        rall = 1;
    }
    else if (c == BKSP || c == DEL)
    {
        if (ccol > 0)
        {
            baksp();
            dlin = crow;
        }
        else
        {
            baksp();
            rall = 1;
        }
    }
    else if (c == KDEL)
    {
        if (ccol < strlen(ln[crow]))
        {
            delch();
            dlin = crow;
        }
        else
        {
            delch();
            rall = 1;
        }
    }
    else if (c == CR || c == LF)
    {
        newln();
        rall = 1;
    }
    else if (c == KUP || c == KDN || c == KLT || c == KRT)
        movek(c);
    else if (c == KPGU)
    {
        crow = crow - TEXTN;
        rall = 1;
    }
    else if (c == KPGD)
    {
        crow = crow + TEXTN;
        rall = 1;
    }
    else if (c == KTOP)
    {
        crow = 0;
        ccol = 0;
        rall = 1;
    }
    else if (c == KBOT)
    {
        crow = lcnt - 1;
        ccol = 0;
        rall = 1;
    }
    else if (c == KINS)
    {
        ovrm = !ovrm;
        dstat = 1;
    }
    else if (c >= 32 && c <= 126)
    {
        if (ovrm)
        {
            if (ccol < strlen(ln[crow]))
            {
                strcpy(ebuf, ln[crow]);
                ebuf[ccol] = c;
                lnset(crow, ebuf);
                ccol++;
                setdirt(1);
                msg("");
            }
            else
                insch(c);
        }
        else
            insch(c);
        dlin = crow;
    }
    return 0;
}

/* usage() - Print command line help. */
int usage()
{
    cput("\r\nEDIT - nano-inspired BDS C editor\r\n");
    cput("Usage: edit filename\r\n");
    cput("Use Ctrl-W inside the editor for help.\r\n");
    return 0;
}

/* main(argc,argv) - Program entry. */
int main(argc, argv)
int argc;
char **argv;
{
    int c;

    name[0] = 0;
    mesg[0] = 0;

    if (!getfcb(name))
    {
        usage();
        return 0;
    }

    loadf(name);
    clrs();
    quit = 0;

    while (!quit)
    {
        redrw();
        c = keyrd();
        dobin(c);
    }

    if (!evsav)
        unbak();

    clrs();
    cput("\r\n");
    return 0;
}
