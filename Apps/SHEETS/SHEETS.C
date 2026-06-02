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
 *          cell refs (A1..Z99), Excel-style absolute refs
 *          ($A$1, $A1, A$1), and the range functions
 *          SUM AVG MIN MAX COUNT - e.g. =SUM(A1:B5).
 *          RAND() / RAND(n) draws a hardware random number; unlike
 *          Excel RAND() it is frozen to a fixed value when entered.
 *          Relative refs shift on copy/paste and row/col
 *          insert; a ref pushed off the grid renders as #REF!.
 *          A formula that refers back to its own cell (directly or
 *          through other cells, including a range that spans it)
 *          is detected as a circular reference and renders #CIRC.
 *
 * Keys:
 *   Arrow keys    Move cursor
 *   Enter         Edit current cell (preserve current content)
 *   Any printable Start fresh edit with that character
 *   ESC           (in edit) cancel ; (in nav) quit
 *   Ctrl-K        Clear current cell
 *   Ctrl-C        Copy current cell
 *   Ctrl-P        Paste into current cell (formula refs shift)
 *   Ctrl-O        Write file
 *   Ctrl-W        Help
 *   Ctrl-G        Go to cell (e.g. "C12")
 *   Ctrl-Q        Quit
 *
 * Build (CP/M): compiled as two units (SHEETS.C + SHEETC.C) so
 * each fits the BDS C 1.6 parser; shared state lives in SHEETS.H.
 *      cc sheets
 *      cc sheetc
 *      clink sheets sheetc string long
 */

#include "stdio.h"
#include "string.h"
#include "sheets.h"

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

/* ---- Cell storage, formula engine, and rendering live in
 * SHEETC.C (split out so each unit fits the BDS C compiler). ---- */

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
    cput("^W Help ^O Save ^G Goto ^C Copy ^P Paste ^R/V Page ^Q Quit");
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
 * Use -1 on the axis that didn't change.  Excel-style absolute
 * markers ('$') are preserved, and absolute refs are still
 * shifted by a structural insert (as Excel does).  A ref pushed
 * off the grid becomes "#REF!".  Returns a freshly alloc'd
 * string, or 0 on alloc failure.  Function names are skipped
 * because they are followed by '(' not a digit.
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
    int colabs, rowabs;
    int isref;

    p = s;
    i = 0;
    while (*p && i < 150)
    {
        isref = 0;
        if (*p == '$' && isal(p[1]))
            isref = 1;
        else if (isal(*p) && (p[1] == '$' || (p[1] >= '0' && p[1] <= '9')))
            isref = 1;

        if (!isref)
        {
            buf[i++] = *p++;
            continue;
        }

        colabs = 0;
        rowabs = 0;
        if (*p == '$')
        {
            colabs = 1;
            p++;
        }
        col = upr(*p) - 'A';
        p++;
        if (*p == '$')
        {
            rowabs = 1;
            p++;
        }
        row = 0;
        n = 0;
        while (*p >= '0' && *p <= '9' && n < 2)
        {
            row = row * 10 + (*p - '0');
            p++;
            n++;
        }

        if (dc >= 0 && col >= dc)
            col++;
        if (dr >= 0 && row >= dr + 1)
            row++;

        if (col < 0 || col >= MAXCOL || row < 1 || row > MAXROW)
        {
            buf[i++] = '#';
            buf[i++] = 'R';
            buf[i++] = 'E';
            buf[i++] = 'F';
            buf[i++] = '!';
            continue;
        }

        if (colabs)
            buf[i++] = '$';
        buf[i++] = 'A' + col;
        if (rowabs)
            buf[i++] = '$';
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
    cput("Clear cell: Ctrl-K  (or Backspace/Del)");
    curmv(10, 4);
    cput("Insert row: Ctrl-N  (shifts rows down, drops bottom)");
    curmv(11, 4);
    cput("Insert col: Ctrl-T  (shifts cols right, drops rightmost)");
    curmv(12, 4);
    cput("Save file:  Ctrl-O              Goto cell: Ctrl-G (e.g. C12)");
    curmv(13, 4);
    cput("Page up/dn: Ctrl-R / Ctrl-V      Quit:      Ctrl-Q or ESC");
    curmv(15, 4);
    cput("Copy/paste: Ctrl-C copies the current cell, Ctrl-P pastes it.");
    curmv(16, 4);
    cput("On paste, relative refs shift by how far you moved the cursor:");
    curmv(17, 4);
    cput("copy =A1 in B2, paste in B3 -> =A2.  Pinned ($) parts stay put.");
    curmv(18, 4);
    cput("Formulas: =A1+B2*3   =-A1   =(A1+A2)/2");
    curmv(19, 4);
    cput("          =SUM(A1:A5)  =AVG(A1:C3)  =MIN/MAX/COUNT(A1:A9)");
    curmv(20, 4);
    cput("          =RAND() 0..32767  =RAND(6) 0..5 (fixed when typed)");
    curmv(21, 4);
    cput("Absolute refs: $A$1 pins both, $A1 pins col, A$1 pins row.");
    curmv(22, 4);
    cput("Relative refs shift on copy/paste; off-grid refs show #REF!.");
    curmv(23, 4);
    cput("A cell in its own range/formula shows #CIRC (circular ref).");
    curmv(24, 4);
    cput("Press any key to return.");
    keywt();
    rall = 1;
    return 0;
}

/* ---- Copy / paste ---- */

/* Copy current cell's contents into the clipboard buffer. */
int copycl()
{
    char *s;

    s = cells[crow][ccol];
    if (s == 0 || s[0] == 0)
    {
        hasclip = 0;
        clip[0] = 0;
        msg("Nothing to copy");
        return 0;
    }
    strncpy(clip, s, 78);
    clip[78] = 0;
    hasclip = 1;
    cliprow = crow;
    clipcol = ccol;
    msg("Copied");
    return 0;
}

/* Build a copy of formula 's' with every relative cell reference
 * shifted by (dr, dc) - the row/column distance from the copy
 * source to the paste destination. Excel-style absolute markers
 * are honoured: a '$' before the column pins the column, a '$'
 * before the row pins the row, and pinned parts are not shifted.
 * A relative reference that moves outside the grid is written as
 * "#REF!" (matching Excel) instead of being clamped. Function
 * names (SUM/AVG/MIN/MAX/COUNT) are left alone because they are
 * followed by '(' not a digit. Returns a freshly alloc'd string,
 * or 0 on alloc failure. */
char *reloc(s, dr, dc)
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
    int colabs, rowabs;
    int isref;

    p = s;
    i = 0;
    while (*p && i < 150)
    {
        isref = 0;
        if (*p == '$' && isal(p[1]))
            isref = 1;
        else if (isal(*p) && (p[1] == '$' || (p[1] >= '0' && p[1] <= '9')))
            isref = 1;

        if (!isref)
        {
            buf[i++] = *p++;
            continue;
        }

        colabs = 0;
        rowabs = 0;
        if (*p == '$')
        {
            colabs = 1;
            p++;
        }
        col = upr(*p) - 'A';
        p++;
        if (*p == '$')
        {
            rowabs = 1;
            p++;
        }
        row = 0;
        n = 0;
        while (*p >= '0' && *p <= '9' && n < 2)
        {
            row = row * 10 + (*p - '0');
            p++;
            n++;
        }

        if (!colabs)
            col = col + dc;
        if (!rowabs)
            row = row + dr;

        if (col < 0 || col >= MAXCOL || row < 1 || row > MAXROW)
        {
            buf[i++] = '#';
            buf[i++] = 'R';
            buf[i++] = 'E';
            buf[i++] = 'F';
            buf[i++] = '!';
            continue;
        }

        if (colabs)
            buf[i++] = '$';
        buf[i++] = 'A' + col;
        if (rowabs)
            buf[i++] = '$';
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
    buf[i] = 0;
    out = alloc(i + 1);
    if (out == 0) return 0;
    strcpy(out, buf);
    return out;
}

/* Paste the clipboard buffer into the current cell. If the
 * clipboard holds a formula, its references are shifted relative
 * to how far the cursor has moved from the copy source. */
int pastcl()
{
    char *nw;

    if (!hasclip)
    {
        msg("Clipboard empty");
        return 0;
    }
    if (clip[0] == '=')
    {
        nw = reloc(clip, crow - cliprow, ccol - clipcol);
        if (nw == 0)
        {
            msg("Out of memory");
            return 0;
        }
        setcel(crow, ccol, nw);
        free(nw);
    }
    else
    {
        setcel(crow, ccol, clip);
    }
    dirty = 1;
    msg("Pasted");
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

int main()
{
    int k;
    int r, c;
    int quit;
    int orow, ocol, otrow, otcol;

    ename[0] = 0;
    mesg[0] = 0;
    clip[0] = 0;
    hasclip = 0;
    cliprow = 0;
    clipcol = 0;
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
        else if (k == CTLK || k == BKSP || k == DEL)
        {
            clrcel(crow, ccol);
            msg("Cleared");
            rall = 1;
        }
        else if (k == CTLC)
        {
            copycl();
        }
        else if (k == CTLP)
        {
            pastcl();
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
