/*
 * SHEETS.C - Simple spreadsheet for BDS C 1.6 on CP/M / Altair 8800.
 *
 * Inspired by EDIT.C and CLOCK.C - same VT100 helpers, same
 * key-translation conventions (emulator maps arrow keys to the
 * WordStar diamond, which the editor uses as KUP/KDN/KLT/KRT).
 *
 * Grid:    26 columns (A..Z) by 99 rows
 * Viewport: 7 columns x 20 rows, scrolls with cursor
 * Cells:   sparse - char* per slot, NULL means empty
 * Values:  16-bit signed integers; text cells stored literally
 * Formula: leading '='; supports + - * / unary minus, parens,
 *          cell refs (A1..Z99), and the range functions
 *          SUM AVG MIN MAX COUNT - e.g. =SUM(A1:B5)
 *
 * Keys:
 *   Arrow keys    Move cursor
 *   Enter         Edit current cell (preserve current content)
 *   Any printable Start fresh edit with that character
 *   ESC           (in edit) cancel ; (in nav) quit
 *   Ctrl-K        Clear current cell
 *   Ctrl-O        Write file
 *   Ctrl-L        Reload file
 *   Ctrl-W        Help
 *   Ctrl-G        Go to cell (e.g. "C12")
 *   Ctrl-Q        Quit
 *
 * Build (CP/M):
 *      cc sheets
 *      clink sheets
 */

#include "stdio.h"
#include "string.h"

#define MAXCOL 26
#define MAXROW 99
#define CWID   10
#define VCOLS  7
#define VROWS  26
#define GUTW   4

#define TITR   1
#define HDRR   2
#define DATR   3
#define EDR    29
#define STR    29
#define HLPR   30
#define SCRW   80

#define ESC    27
#define CR     13
#define LF     10
#define BKSP   8
#define CTLH   8
#define DEL    127
#define KUP    5
#define KDN    24
#define KRT    4
#define KLT    19
#define CTLG   7
#define CTLK   11
#define CTLL   12
#define CTLN   14
#define CTLO   15
#define CTLQ   17
#define CTLR   18
#define CTLT   20
#define CTLV   22
#define CTLW   23

/* ---- Externals ---- */

int bdos();
int bios();
int fclose();
int fgetc();
int fputc();
FILE *fopen();
char *alloc();
int free();

/* From SDK/LONG.C (32-bit signed long math; longs are char[4]) */
char *itol();
int ltoi();
int lcomp();
char *ladd();
char *lsub();
char *lmul();
char *ldiv();
char *lmod();
char *atol();
char *ltoa();

/* ---- Grid state ---- */

char *cells[MAXROW][MAXCOL];
int crow, ccol;
int trow, tcol;
char ebuf[80];
char ename[16];
char mesg[64];
int dirty;
int rall;

/* Eval globals */
char *epos;
int eok;
int edepth;

/* Constant long buffers used by the evaluator; initialized in main(). */
char lzro[4];
char lten[4];

/* ---- VT100 helpers ---- */

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
    if (n < 0)
    {
        chout('-');
        n = -n;
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

int cls()
{
    chout(ESC);
    cput("[2J");
    curmv(1, 1);
    return 0;
}

int eol()
{
    chout(ESC);
    cput("[K");
    return 0;
}

int invon()
{
    chout(ESC);
    cput("[7m");
    return 0;
}

int invof()
{
    chout(ESC);
    cput("[0m");
    return 0;
}

int hidcr()
{
    chout(ESC);
    cput("[?25l");
    return 0;
}

int shwcr()
{
    chout(ESC);
    cput("[?25h");
    return 0;
}

int keywt()
{
    int c;

    c = 0;
    while (c == 0)
        c = bdos(6, 255) & 255;
    return c;
}

/* ---- Cell helpers ---- */

int setcel(r, c, s)
int r;
int c;
char *s;
{
    char *p;
    int n;

    if (r < 0 || r >= MAXROW || c < 0 || c >= MAXCOL)
        return -1;

    if (cells[r][c])
    {
        free(cells[r][c]);
        cells[r][c] = 0;
    }

    if (s == 0 || s[0] == 0)
        return 0;

    n = strlen(s);
    p = alloc(n + 1);
    if (p == 0)
        return -1;
    strcpy(p, s);
    cells[r][c] = p;
    return 0;
}

int clrcel(r, c)
int r;
int c;
{
    if (cells[r][c])
    {
        free(cells[r][c]);
        cells[r][c] = 0;
        dirty = 1;
    }
    return 0;
}

/* ---- Parser / evaluator ---- */

int eskp()
{
    while (*epos == ' ' || *epos == '\t')
        epos++;
    return 0;
}

int isdig(c)
int c;
{
    return (c >= '0' && c <= '9');
}

int isal(c)
int c;
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int upr(c)
int c;
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    return c;
}

/* parse a cell ref like A1 / z99, set *rp, *cp, advance epos */
int prsref(rp, cp)
int *rp;
int *cp;
{
    int col;
    int row;
    int c;

    eskp();
    if (!isal(*epos))
        return 0;
    col = upr(*epos) - 'A';
    epos++;
    if (!isdig(*epos))
        return 0;
    row = 0;
    while (isdig(*epos))
    {
        row = row * 10 + (*epos - '0');
        epos++;
    }
    row = row - 1;
    if (row < 0 || row >= MAXROW || col < 0 || col >= MAXCOL)
        return 0;
    *rp = row;
    *cp = col;
    return 1;
}

int evcell();
int expr();
int term();
int factor();

/* Forward-call wrapper: evaluate cell (r,c) into the 4-byte long
 * pointed to by vp. Recursion-guarded via edepth. */
int evcell(r, c, vp)
int r;
int c;
char *vp;
{
    char *s;
    char *sav;
    int ok;

    itol(vp, 0);
    if (r < 0 || r >= MAXROW || c < 0 || c >= MAXCOL)
        return 0;
    s = cells[r][c];
    if (s == 0 || s[0] == 0)
        return 1;
    if (s[0] != '=')
    {
        /* Plain text/number: take leading signed integer if any. */
        if (s[0] == '-' || isdig(s[0]))
            atol(vp, s);
        return 1;
    }
    if (edepth > 24)
        return 0;
    edepth++;
    sav = epos;
    epos = s + 1;
    eok = 1;
    ok = expr(vp);
    epos = sav;
    edepth--;
    if (!ok || !eok)
        return 0;
    return 1;
}

int factor(vp)
char *vp;
{
    int neg;
    int r, c, r2, c2;
    int i, j;
    int tag;
    int cnt;
    int gotn;
    char rv[4];
    char dig[4];
    char tmp[4];

    eskp();
    neg = 0;
    while (*epos == '-' || *epos == '+')
    {
        if (*epos == '-')
            neg = !neg;
        epos++;
        eskp();
    }

    if (*epos == '(')
    {
        epos++;
        if (!expr(vp))
            return 0;
        eskp();
        if (*epos != ')')
        {
            eok = 0;
            return 0;
        }
        epos++;
    }
    else if (isdig(*epos))
    {
        itol(vp, 0);
        while (isdig(*epos))
        {
            itol(dig, *epos - '0');
            lmul(vp, vp, lten);
            ladd(vp, vp, dig);
            epos++;
        }
    }
    else if ((upr(epos[0]) == 'S') && (upr(epos[1]) == 'U')
             && (upr(epos[2]) == 'M') && (epos[3] == '('))
    {
        tag = 0;
        epos = epos + 4;
        goto rng;
    }
    else if ((upr(epos[0]) == 'A') && (upr(epos[1]) == 'V')
             && (upr(epos[2]) == 'G') && (epos[3] == '('))
    {
        tag = 1;
        epos = epos + 4;
        goto rng;
    }
    else if ((upr(epos[0]) == 'M') && (upr(epos[1]) == 'I')
             && (upr(epos[2]) == 'N') && (epos[3] == '('))
    {
        tag = 2;
        epos = epos + 4;
        goto rng;
    }
    else if ((upr(epos[0]) == 'M') && (upr(epos[1]) == 'A')
             && (upr(epos[2]) == 'X') && (epos[3] == '('))
    {
        tag = 3;
        epos = epos + 4;
        goto rng;
    }
    else if ((upr(epos[0]) == 'C') && (upr(epos[1]) == 'O')
             && (upr(epos[2]) == 'U') && (upr(epos[3]) == 'N')
             && (upr(epos[4]) == 'T') && (epos[5] == '('))
    {
        tag = 4;
        epos = epos + 6;
rng:
        if (!prsref(&r, &c))
        {
            eok = 0;
            return 0;
        }
        eskp();
        if (*epos != ':')
        {
            eok = 0;
            return 0;
        }
        epos++;
        if (!prsref(&r2, &c2))
        {
            eok = 0;
            return 0;
        }
        eskp();
        if (*epos != ')')
        {
            eok = 0;
            return 0;
        }
        epos++;
        itol(vp, 0);
        cnt = 0;
        gotn = 0;
        for (i = r; i <= r2; i++)
        {
            for (j = c; j <= c2; j++)
            {
                if (tag == 4)
                {
                    if (cells[i][j] && cells[i][j][0])
                        cnt++;
                    continue;
                }
                if (!evcell(i, j, rv))
                {
                    eok = 0;
                    return 0;
                }
                cnt++;
                if (tag == 0 || tag == 1)
                {
                    ladd(vp, vp, rv);
                }
                else if (tag == 2)
                {
                    if (!gotn || lcomp(rv, vp) < 0)
                    {
                        vp[0] = rv[0];
                        vp[1] = rv[1];
                        vp[2] = rv[2];
                        vp[3] = rv[3];
                    }
                    gotn = 1;
                }
                else if (tag == 3)
                {
                    if (!gotn || lcomp(rv, vp) > 0)
                    {
                        vp[0] = rv[0];
                        vp[1] = rv[1];
                        vp[2] = rv[2];
                        vp[3] = rv[3];
                    }
                    gotn = 1;
                }
            }
        }
        if (tag == 1)
        {
            if (cnt == 0)
            {
                eok = 0;
                return 0;
            }
            itol(dig, cnt);
            ldiv(vp, vp, dig);
        }
        else if (tag == 4)
        {
            itol(vp, cnt);
        }
    }
    else if (isal(*epos))
    {
        if (!prsref(&r, &c))
        {
            eok = 0;
            return 0;
        }
        if (!evcell(r, c, vp))
        {
            eok = 0;
            return 0;
        }
    }
    else
    {
        eok = 0;
        return 0;
    }

    if (neg)
    {
        lsub(tmp, lzro, vp);
        vp[0] = tmp[0];
        vp[1] = tmp[1];
        vp[2] = tmp[2];
        vp[3] = tmp[3];
    }
    return 1;
}

int term(vp)
char *vp;
{
    char rhs[4];
    char op;

    if (!factor(vp))
        return 0;
    eskp();
    while (*epos == '*' || *epos == '/')
    {
        op = *epos;
        epos++;
        if (!factor(rhs))
            return 0;
        if (op == '*')
            lmul(vp, vp, rhs);
        else
        {
            if (lcomp(rhs, lzro) == 0)
            {
                eok = 0;
                return 0;
            }
            ldiv(vp, vp, rhs);
        }
        eskp();
    }
    return 1;
}

int expr(vp)
char *vp;
{
    char rhs[4];
    char op;

    if (!term(vp))
        return 0;
    eskp();
    while (*epos == '+' || *epos == '-')
    {
        op = *epos;
        epos++;
        if (!term(rhs))
            return 0;
        if (op == '+')
            ladd(vp, vp, rhs);
        else
            lsub(vp, vp, rhs);
        eskp();
    }
    return 1;
}

/* ---- Formatting ---- */

/* Render cell (r,c) into buf right-padded/truncated to CWID chars
 * plus a trailing NUL. Returns 0 always. */
int rndcel(r, c, buf)
int r;
int c;
char *buf;
{
    char *s;
    char tmp[16];
    char lv[4];
    int ok;
    int n, i, p;

    for (i = 0; i < CWID; i++)
        buf[i] = ' ';
    buf[CWID] = 0;

    s = cells[r][c];
    if (s == 0 || s[0] == 0)
        return 0;

    if (s[0] == '=')
    {
        edepth = 0;
        ok = evcell(r, c, lv);
        if (!ok)
        {
            strcpy(buf, "  #ERR    ");
            buf[CWID] = 0;
            return 0;
        }
        ltoa(tmp, lv);
        n = strlen(tmp);
        if (n > CWID)
        {
            for (i = 0; i < CWID; i++)
                buf[i] = '#';
            buf[CWID] = 0;
            return 0;
        }
        p = CWID - n;
        for (i = 0; i < n; i++)
            buf[p + i] = tmp[i];
        return 0;
    }

    /* Plain text or number: right-align numeric, left-align text. */
    if (s[0] == '-' || isdig(s[0]))
    {
        n = strlen(s);
        if (n > CWID)
        {
            for (i = 0; i < CWID; i++)
                buf[i] = '#';
            return 0;
        }
        p = CWID - n;
        for (i = 0; i < n; i++)
            buf[p + i] = s[i];
        return 0;
    }

    n = strlen(s);
    if (n > CWID)
        n = CWID;
    for (i = 0; i < n; i++)
        buf[i] = s[i];
    return 0;
}

/* ---- Drawing ---- */

int msg(s)
char *s;
{
    int i;

    i = 0;
    while (s[i] && i < 62)
    {
        mesg[i] = s[i];
        i++;
    }
    mesg[i] = 0;
    return 0;
}

int title()
{
    curmv(TITR, 1);
    /* bright white on Altair-panel blue */
    cput("\033[1;97;44m ALTAIR SHEETS  ");
    if (ename[0])
        cput(ename);
    else
        cput("[no file]");
    if (dirty)
        cput(" *");
    eol();
    cput("\033[0m");
    return 0;
}

int drwhdr()
{
    int i;
    int col;
    int pad;
    int j;

    curmv(HDRR, 1);
    /* black on amber (bright yellow) */
    cput("\033[30;103m");
    cput("    ");
    pad = (CWID - 1) / 2;
    for (i = 0; i < VCOLS; i++)
    {
        col = tcol + i;
        if (col >= MAXCOL)
        {
            cput("          ");
            continue;
        }
        for (j = 0; j < pad; j++)
            chout(' ');
        chout('A' + col);
        for (j = pad + 1; j < CWID; j++)
            chout(' ');
    }
    eol();
    cput("\033[0m");
    return 0;
}

int drwrow(vr)
int vr;
{
    char buf[CWID + 2];
    int r;
    int i;
    int c;
    int is_cur;

    r = trow + vr;
    curmv(DATR + vr, 1);

    if (r >= MAXROW)
    {
        cput("\033[0m");
        eol();
        return 0;
    }

    /* Row gutter: black on amber, 3-digit row number. */
    cput("\033[30;103m");
    if (r + 1 < 10)
        cput("  ");
    else if (r + 1 < 100)
        chout(' ');
    nump(r + 1);
    cput(" \033[0m");

    for (i = 0; i < VCOLS; i++)
    {
        c = tcol + i;
        if (c >= MAXCOL)
        {
            cput("          ");
            continue;
        }
        rndcel(r, c, buf);
        is_cur = (r == crow && c == ccol);
        if (is_cur)
            /* bright white on LED red */
            cput("\033[1;97;41m");
        else
            /* phosphor green */
            cput("\033[92m");
        cput(buf);
        cput("\033[0m");
    }
    eol();
    return 0;
}

int colltr(c)
int c;
{
    return 'A' + c;
}

int statln()
{
    char *s;
    int i;

    curmv(STR, 1);
    /* bright amber on blue bar */
    cput("\033[1;93;44m");
    chout(' ');
    chout(colltr(ccol));
    nump(crow + 1);
    cput("  ");
    s = cells[crow][ccol];
    if (s)
    {
        i = 0;
        while (s[i] && i < 50)
        {
            chout(s[i]);
            i++;
        }
        if (s[i])
            cput("...");
    }
    cput("    ");
    cput(mesg);
    eol();
    cput("\033[0m");
    return 0;
}

int helpbr()
{
    curmv(HLPR, 1);
    /* dim amber */
    cput("\033[2;33m");
    cput("^W Help ^O Save ^L Load ^G Goto ^N InsRow ^T InsCol ^R/V Page ^Q Quit");
    eol();
    cput("\033[0m");
    return 0;
}

int drwall()
{
    int i;

    hidcr();
    title();
    drwhdr();
    for (i = 0; i < VROWS; i++)
        drwrow(i);
    statln();
    helpbr();
    return 0;
}

/* Redraw only the parts that change while scrolling: column
 * header, all data rows, status.  Title and help bar are
 * static, so skipping them roughly halves the bytes shipped
 * to the terminal on a scroll - noticeable on a real ESP32. */
int drwgrd()
{
    int i;

    hidcr();
    drwhdr();
    for (i = 0; i < VROWS; i++)
        drwrow(i);
    statln();
    return 0;
}

/* ---- Scrolling / cursor ---- */

int fixvw()
{
    if (crow < trow)
        trow = crow;
    if (crow >= trow + VROWS)
        trow = crow - VROWS + 1;
    if (ccol < tcol)
        tcol = ccol;
    if (ccol >= tcol + VCOLS)
        tcol = ccol - VCOLS + 1;
    return 0;
}

/* ---- Edit prompt ---- */

int prompt(buf, pr, mx)
char *buf;
char *pr;
int mx;
{
    int c;
    int p;

    curmv(EDR, 1);
    invon();
    chout(' ');
    cput(pr);
    chout(' ');
    invof();
    cput(buf);
    eol();
    shwcr();

    p = strlen(buf);
    while (1)
    {
        c = keywt();
        if (c == CR || c == LF)
        {
            buf[p] = 0;
            hidcr();
            curmv(EDR, 1);
            eol();
            return 1;
        }
        if (c == ESC)
        {
            hidcr();
            curmv(EDR, 1);
            eol();
            return 0;
        }
        if (c == BKSP || c == DEL)
        {
            if (p > 0)
            {
                p--;
                chout(BKSP);
                chout(' ');
                chout(BKSP);
            }
            continue;
        }
        if (c >= 32 && c < 127 && p < mx - 1)
        {
            buf[p] = c;
            p++;
            chout(c);
        }
    }
}

int editcl(init)
int init;
{
    char *s;
    int n;

    ebuf[0] = 0;
    if (init == 0)
    {
        s = cells[crow][ccol];
        if (s)
        {
            n = strlen(s);
            if (n >= 78)
                n = 77;
            strncpy(ebuf, s, n);
            ebuf[n] = 0;
        }
    }
    else
    {
        ebuf[0] = init;
        ebuf[1] = 0;
    }

    if (prompt(ebuf, "Cell:", 79))
    {
        setcel(crow, ccol, ebuf);
        dirty = 1;
        msg("");
        if (crow < MAXROW - 1)
            crow++;
    }
    else
        msg("Edit cancelled");
    rall = 1;
    return 0;
}

/* ---- File I/O ---- */

int getfcb(d)
char *d;
{
    char *fcb;
    int i, j;

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

/* Save grid: one line per non-empty cell: "A1=text-or-formula" */
int savef()
{
    int r, c, i;
    char *s;
    FILE *fp;

    if (ename[0] == 0)
    {
        msg("No filename");
        return -1;
    }
    fp = fopen(ename, "w");
    if (fp == NULL)
    {
        msg("Cannot write");
        return -1;
    }
    for (r = 0; r < MAXROW; r++)
    {
        for (c = 0; c < MAXCOL; c++)
        {
            s = cells[r][c];
            if (s == 0 || s[0] == 0)
                continue;
            fputc(colltr(c), fp);
            /* row number */
            if (r + 1 >= 10)
                fputc(((r + 1) / 10) + '0', fp);
            fputc(((r + 1) % 10) + '0', fp);
            fputc('=', fp);
            i = 0;
            while (s[i])
            {
                fputc(s[i], fp);
                i++;
            }
            fputc(CR, fp);
            fputc(LF, fp);
        }
    }
    fclose(fp);
    dirty = 0;
    msg("Saved");
    return 0;
}

/* Clear all cells. */
int clrall()
{
    int r, c;

    for (r = 0; r < MAXROW; r++)
        for (c = 0; c < MAXCOL; c++)
        {
            if (cells[r][c])
            {
                free(cells[r][c]);
                cells[r][c] = 0;
            }
        }
    return 0;
}

/* Build a renumbered copy of formula 's' after an insertion.
 * dr >= 0 : a row was inserted at index dr; refs with row
 *           >= dr+1 (1-based) are bumped by +1.
 * dc >= 0 : a col was inserted at index dc; refs with col
 *           index >= dc are bumped by +1.
 * Use -1 on the axis that didn't change.  Refs that would
 * leave the grid are clamped at the last valid index.
 * Returns a freshly alloc'd string, or 0 on alloc failure.
 * Function names (SUM/AVG/MIN/MAX/COUNT) are skipped because
 * they are followed by '(' not a digit.
 */
char *bmpref(s, dr, dc)
char *s;
int dr;
int dc;
{
    char buf[160];
    char *p;
    char *out;
    int i;
    int col, row;
    int n;

    p = s;
    i = 0;
    while (*p && i < 155)
    {
        if (isal(*p) && p[1] >= '0' && p[1] <= '9')
        {
            col = upr(*p) - 'A';
            p++;
            row = 0;
            n = 0;
            while (*p >= '0' && *p <= '9' && n < 2)
            {
                row = row * 10 + (*p - '0');
                p++;
                n++;
            }
            if (dc >= 0 && col >= dc && col < MAXCOL - 1)
                col++;
            if (dr >= 0 && row >= dr + 1 && row < MAXROW)
                row++;
            if (col < 0) col = 0;
            if (col >= MAXCOL) col = MAXCOL - 1;
            if (row < 1) row = 1;
            if (row > MAXROW) row = MAXROW;
            buf[i++] = 'A' + col;
            if (row >= 10)
            {
                buf[i++] = '0' + (row / 10);
                buf[i++] = '0' + (row % 10);
            }
            else
            {
                buf[i++] = '0' + row;
            }
        }
        else
        {
            buf[i++] = *p++;
        }
    }
    buf[i] = 0;
    out = alloc(i + 1);
    if (out == 0) return 0;
    strcpy(out, buf);
    return out;
}

/* Apply bmpref to every formula cell in the grid. */
int bmpall(dr, dc)
int dr;
int dc;
{
    int r, c;
    char *s;
    char *nw;

    for (r = 0; r < MAXROW; r++)
    {
        for (c = 0; c < MAXCOL; c++)
        {
            s = cells[r][c];
            if (s == 0 || s[0] != '=')
                continue;
            nw = bmpref(s, dr, dc);
            if (nw == 0)
                continue;
            free(s);
            cells[r][c] = nw;
        }
    }
    return 0;
}

/* Insert a blank row at crow. Existing rows from crow..MAXROW-2
 * shift down by one; the bottom row's contents are discarded.
 * Formula cell references with row >= crow+1 are bumped. */
int insrow()
{
    int r, c;

    for (c = 0; c < MAXCOL; c++)
    {
        if (cells[MAXROW - 1][c])
        {
            free(cells[MAXROW - 1][c]);
            cells[MAXROW - 1][c] = 0;
        }
    }
    for (r = MAXROW - 1; r > crow; r--)
    {
        for (c = 0; c < MAXCOL; c++)
            cells[r][c] = cells[r - 1][c];
    }
    for (c = 0; c < MAXCOL; c++)
        cells[crow][c] = 0;
    bmpall(crow, -1);
    dirty = 1;
    rall = 1;
    msg("Row inserted");
    return 0;
}

/* Insert a blank column at ccol. Existing columns from
 * ccol..MAXCOL-2 shift right by one; the rightmost column's
 * contents are discarded. Formula refs with col >= ccol
 * are bumped. */
int inscol()
{
    int r, c;

    for (r = 0; r < MAXROW; r++)
    {
        if (cells[r][MAXCOL - 1])
        {
            free(cells[r][MAXCOL - 1]);
            cells[r][MAXCOL - 1] = 0;
        }
    }
    for (r = 0; r < MAXROW; r++)
    {
        for (c = MAXCOL - 1; c > ccol; c--)
            cells[r][c] = cells[r][c - 1];
        cells[r][ccol] = 0;
    }
    bmpall(-1, ccol);
    dirty = 1;
    rall = 1;
    msg("Column inserted");
    return 0;
}

int loadf()
{
    char ln[120];
    int c, p;
    int col, row;
    char *eq;
    int i;
    FILE *fp;

    if (ename[0] == 0)
    {
        msg("No filename");
        return -1;
    }
    fp = fopen(ename, "r");
    if (fp == NULL)
    {
        msg("New file");
        return 0;
    }

    clrall();
    p = 0;
    while ((c = fgetc(fp)) != EOF)
    {
        if (c == CPMEOF)
            break;
        if (c == CR)
            continue;
        if (c == LF)
        {
            ln[p] = 0;
            p = 0;
            if (ln[0] == 0)
                continue;
            if (!isal(ln[0]))
                continue;
            col = upr(ln[0]) - 'A';
            i = 1;
            row = 0;
            while (isdig(ln[i]))
            {
                row = row * 10 + (ln[i] - '0');
                i++;
            }
            row = row - 1;
            if (ln[i] != '=')
                continue;
            eq = &ln[i + 1];
            if (row >= 0 && row < MAXROW
                && col >= 0 && col < MAXCOL)
                setcel(row, col, eq);
            continue;
        }
        if (p < 118)
        {
            ln[p] = c;
            p++;
        }
    }
    fclose(fp);
    dirty = 0;
    msg("Loaded");
    rall = 1;
    return 0;
}

/* ---- Help screen ---- */

int helpsc()
{
    cls();
    curmv(2, 4);
    cput("SHEETS - simple BDS C spreadsheet");
    curmv(4, 4);
    cput("Grid: A..Z columns by 1..99 rows.");
    curmv(5, 4);
    cput("Cells hold text, integers, or =formulas.");
    curmv(7, 4);
    cput("Movement:   Arrow keys");
    curmv(8, 4);
    cput("Edit cell:  Enter (keep)  or any printable (replace)");
    curmv(9, 4);
    cput("Clear cell: Ctrl-K");
    curmv(10, 4);
    cput("Insert row: Ctrl-N  (shifts rows down, drops bottom)");
    curmv(11, 4);
    cput("Insert col: Ctrl-T  (shifts cols right, drops rightmost)");
    curmv(12, 4);
    cput("Save file:  Ctrl-O");
    curmv(13, 4);
    cput("Load file:  Ctrl-L");
    curmv(14, 4);
    cput("Goto cell:  Ctrl-G  (e.g. C12)");
    curmv(15, 4);
    cput("Page up/dn: Ctrl-R / Ctrl-V");
    curmv(16, 4);
    cput("Quit:       Ctrl-Q  or ESC");
    curmv(17, 4);
    cput("Formulas: =A1+B2*3   =-A1   =(A1+A2)/2");
    curmv(18, 4);
    cput("          =SUM(A1:A5)  =AVG(A1:C3)  =MIN/MAX/COUNT(A1:A9)");
    curmv(20, 4);
    cput("Integers only (32-bit, -2147483648..2147483647).  Refs are");
    curmv(21, 4);
    cput("renumbered automatically when rows/columns are inserted.");
    curmv(23, 4);
    cput("Press any key to return.");
    keywt();
    rall = 1;
    return 0;
}

/* ---- Goto ---- */

int gotocl()
{
    char buf[8];
    int r, c, i;

    buf[0] = 0;
    if (!prompt(buf, "Goto:", 7))
    {
        msg("");
        return 0;
    }
    if (!isal(buf[0]))
    {
        msg("Bad cell");
        return 0;
    }
    c = upr(buf[0]) - 'A';
    i = 1;
    r = 0;
    while (isdig(buf[i]))
    {
        r = r * 10 + (buf[i] - '0');
        i++;
    }
    r = r - 1;
    if (r < 0 || r >= MAXROW || c < 0 || c >= MAXCOL)
    {
        msg("Out of range");
        return 0;
    }
    crow = r;
    ccol = c;
    msg("");
    return 0;
}

/* ---- Confirm quit ---- */

int askqt()
{
    int c;

    if (!dirty)
        return 1;
    msg("Unsaved changes. Quit anyway? y/N");
    statln();
    c = keywt();
    if (c == 'y' || c == 'Y')
        return 1;
    msg("");
    return 0;
}

/* ---- Main ---- */

int usage()
{
    cput("\r\nSHEETS - simple BDS C spreadsheet\r\n");
    cput("Usage: sheets [filename]\r\n");
    cput("Ctrl-W inside the program shows help.\r\n");
    return 0;
}

int main(argc, argv)
int argc;
char **argv;
{
    int k;
    int r, c;
    int quit;
    int orow, ocol, otrow, otcol;

    ename[0] = 0;
    mesg[0] = 0;
    dirty = 0;
    crow = 0;
    ccol = 0;
    trow = 0;
    tcol = 0;
    rall = 1;

    itol(lzro, 0);
    itol(lten, 10);

    for (r = 0; r < MAXROW; r++)
        for (c = 0; c < MAXCOL; c++)
            cells[r][c] = 0;

    if (!getfcb(ename))
    {
        cput("\r\nSHEETS - simple BDS C spreadsheet\r\n");
        cput("Usage: SHEETS filename\r\n");
        cput("A filename is required so Ctrl-O can save.\r\n");
        return 0;
    }

    loadf();

    cls();
    drwall();

    quit = 0;
    while (!quit)
    {
        orow = crow;
        ocol = ccol;
        otrow = trow;
        otcol = tcol;

        k = keywt();

        if (k == KUP)
        {
            if (crow > 0) crow--;
        }
        else if (k == KDN)
        {
            if (crow < MAXROW - 1) crow++;
        }
        else if (k == KLT)
        {
            if (ccol > 0) ccol--;
        }
        else if (k == KRT)
        {
            if (ccol < MAXCOL - 1) ccol++;
        }
        else if (k == CTLR)
        {
            crow = crow - (VROWS - 1);
            if (crow < 0) crow = 0;
            trow = crow;
        }
        else if (k == CTLV)
        {
            crow = crow + (VROWS - 1);
            if (crow > MAXROW - 1) crow = MAXROW - 1;
            trow = crow - VROWS + 1;
            if (trow < 0) trow = 0;
        }
        else if (k == CR || k == LF)
        {
            editcl(0);
        }
        else if (k == CTLK)
        {
            clrcel(crow, ccol);
            msg("Cleared");
            rall = 1;
        }
        else if (k == CTLN)
        {
            insrow();
        }
        else if (k == CTLT)
        {
            inscol();
        }
        else if (k == CTLO)
        {
            savef();
            rall = 1;
        }
        else if (k == CTLL)
        {
            loadf();
        }
        else if (k == CTLG)
        {
            gotocl();
        }
        else if (k == CTLW || k == CTLH)
        {
            helpsc();
        }
        else if (k == CTLQ || k == ESC)
        {
            if (askqt())
                quit = 1;
        }
        else if (k >= 32 && k < 127)
        {
            editcl(k);
        }

        fixvw();
        if (rall)
        {
            drwall();
            rall = 0;
        }
        else if (trow != otrow || tcol != otcol)
        {
            drwgrd();
        }
        else if (crow != orow || ccol != ocol)
        {
            /* Redraw old + new rows so highlight moves. */
            drwrow(orow - trow);
            drwrow(crow - trow);
            statln();
        }
        else
        {
            statln();
        }
    }

    cls();
    shwcr();
    cput("\r\nSHEETS done.\r\n");
    return 0;
}
