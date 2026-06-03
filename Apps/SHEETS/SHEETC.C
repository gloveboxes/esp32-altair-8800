/*
 * SHEETC.C - formula engine for SHEETS, split out from SHEETS.C so
 * each translation unit fits the BDS C 1.6 compiler (the combined
 * source overflowed the part-I parser). This module holds the cell
 * storage helpers, the formula parser/evaluator, and cell rendering.
 *
 * Shared state and externals live in SHEETS.H. stdio.h must remain
 * the first include so the BDS C COMMON external layout matches
 * SHEETS.C.
 */

#include "stdio.h"
#include "string.h"
#include "sheets.h"

/* ---- Function-name dispatch ----
 * A single place that recognises a built-in function name, so the
 * parser no longer carries hand-unrolled per-character compares.
 * fnat() tests for "NAME(" at pointer p (case-insensitive; NAME is
 * supplied in upper case), tolerating spaces/tabs between the name
 * and the '(' so "SUM (a1:b1)" parses like "SUM(a1:b1)"; on a match
 * it returns the offset of the character just past the '(' , else 0.
 * fnmatch() applies that at the parser cursor epos and, on a match,
 * advances epos past the '(' so the argument handler can run. The
 * table of names and handler pointers (dispatched by dofn) lives
 * further down, after evcell(), because the handlers call it. */
int fnat(p, name)
char *p;
char *name;
{
    int i;

    i = 0;
    while (name[i])
    {
        if (upr(p[i]) != name[i])
            return 0;
        i++;
    }
    while (p[i] == ' ' || p[i] == '\t')
        i++;
    if (p[i] != '(')
        return 0;
    return i + 1;
}

int fnmatch(name)
char *name;
{
    int n;

    n = fnat(epos, name);
    if (n == 0)
        return 0;
    epos = epos + n;
    return 1;
}

/* Return 1 if formula string 's' uses the volatile RAND() function
 * somewhere, so setcel knows to freeze it to a fixed value. */
int hasrnd(s)
char *s;
{
    while (*s)
    {
        if (fnat(s, "RAND"))
            return 1;
        s++;
    }
    return 0;
}

/* Normalise a formula to a compact canonical form: drop every space
 * and tab, upper-case cell-reference letters, and lower-case built-in
 * function names, so "= Sum ( a1 : b1 )" is stored as "=sum(A1:B1)".
 * Safe because the formula grammar has no tokens in which whitespace
 * is significant and letter case never changes meaning (there are no
 * string literals). A run of letters that is followed (after optional
 * spaces) by '(' is a function name -> lower case; any other letter
 * run is a column reference -> upper case. Copies src into dst, which
 * must hold at least strlen(src)+1 chars; returns the compacted len. */
int normfm(dst, src)
char *dst;
char *src;
{
    int i;
    int j;
    int ch;
    char run[16];

    i = 0;
    while (*src)
    {
        if (*src == ' ' || *src == '\t')
        {
            src++;
            continue;
        }
        if (isal(*src))
        {
            j = 0;
            while (isal(*src))
            {
                if (j < 15)
                    run[j++] = *src;
                src++;
            }
            run[j] = 0;
            while (*src == ' ' || *src == '\t')
                src++;
            j = 0;
            if (*src == '(')
            {
                while (run[j])
                {
                    ch = run[j++];
                    if (ch >= 'A' && ch <= 'Z')
                        ch = ch + 32;
                    dst[i++] = ch;
                }
            }
            else
            {
                while (run[j])
                    dst[i++] = upr(run[j++]);
            }
        }
        else
        {
            dst[i++] = *src++;
        }
    }
    dst[i] = 0;
    return i;
}

int setcel(r, c, s)
int r;
int c;
char *s;
{
    char *p;
    char *sav;
    char lv[4];
    char nbuf[16];
    char nf[84];
    int n;
    int ok;

    if (r < 0 || r >= MAXROW || c < 0 || c >= MAXCOL)
        return -1;

    if (cells[r][c])
    {
        free(cells[r][c]);
        cells[r][c] = 0;
    }

    if (s == 0 || s[0] == 0)
        return 0;

    /* Trim leading and trailing whitespace so " =3+1 " stores as
     * "=3+1" and is recognised as a formula (detection keys on the
     * first character). A blank/all-space entry clears the cell. */
    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    if (n == 0)
        return 0;

    /* Normalise a formula (leading '=') to compact canonical form -
     * no interior spaces/tabs, upper-cased cell refs and lower-cased
     * function names - so "= Sum ( a1 : b1 )" is stored and displayed
     * as "=sum(A1:B1)". Plain text labels are left exactly as typed.
     * nf is 84 bytes; the entry buffer is 80, so any real formula
     * fits. (Avoid sizeof here - BDS C 1.6 mishandles it on arrays.) */
    if (s[0] == '=' && n < 83)
    {
        s[n] = 0;
        normfm(nf, s);
        s = nf;
        n = strlen(s);
    }

    /* Freeze volatile RAND(): unlike Excel's RAND() (which re-rolls
     * on every recalc), this grid re-evaluates every visible cell
     * on each cursor move, so a stored "=RAND()" would change as you
     * scroll over it. Evaluate the formula once here and store the
     * resulting integer, so the random value is fixed when entered
     * (like typing =RAND() in Excel then pressing F9 / paste-values).
     * If evaluation fails the formula is kept and renders #ERR. */
    if (s[0] == '=' && hasrnd(s))
    {
        edepth = 0;
        sav = epos;
        epos = s + 1;
        eok = 1;
        ok = expr(lv);
        epos = sav;
        if (ok && eok)
        {
            ltoa(nbuf, lv);
            s = nbuf;
            n = strlen(s);
        }
    }

    p = alloc(n + 1);
    if (p == 0)
        return -1;
    strncpy(p, s, n);
    p[n] = 0;
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

/* parse a cell ref like A1 / z99, optionally with Excel-style
 * absolute markers ($A$1, $A1, A$1); the '$' signs are accepted
 * and skipped here (they only matter when relocating formulas).
 * Set *rp, *cp, advance epos. */
int prsref(rp, cp)
int *rp;
int *cp;
{
    int col;
    int row;

    eskp();
    if (*epos == '$')
        epos++;
    if (!isal(*epos))
        return 0;
    col = upr(*epos) - 'A';
    epos++;
    if (*epos == '$')
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

/* Read a 16-bit value from the emulator hardware RNG: pulse port
 * 45 to generate, then read the low and high bytes from port 200.
 * Masked to 0..32767 so the result is always a non-negative int. */
int rndnum()
{
    int r;

    outp(45, 1);
    r = inp(200) & 255;
    r = r | ((inp(200) & 255) << 8);
    return r & 0x7FFF;
}

/* Return 1 if cell (r,c) is already on the evaluation stack, i.e.
 * we are in the middle of evaluating it further up the call chain.
 * That means the formula refers back to itself directly or through
 * other cells - a circular reference. */
int oncyc(r, c)
int r;
int c;
{
    int k;

    for (k = 0; k < edepth; k++)
        if (evrow[k] == r && evcol[k] == c)
            return 1;
    return 0;
}

/* Forward-call wrapper: evaluate cell (r,c) into the 4-byte long
 * pointed to by vp. Cycle-guarded via the evrow/evcol stack and
 * depth-guarded via edepth. */
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
    if (oncyc(r, c))
    {
        ecirc = 1;
        eok = 0;
        return 0;
    }
    if (edepth >= EDMAX)
        return 0;
    evrow[edepth] = r;
    evcol[edepth] = c;
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

/* ---- Built-in function handlers ----
 * Each handler is entered with epos positioned just past the '(' of
 * its call. It parses its own arguments up to and including the
 * closing ')', writes the 4-byte long result through vp, and returns
 * 1 on success or 0 on error (after setting eok = 0). The dispatch
 * table fnnam[]/fnfn[] pairs each name with its handler, so adding a
 * function is one table row plus its handler - no parser edits. */

/* =RAND() -> 0..32767; =RAND(n) -> 0..n-1. Frozen when entered
 * (see setcel), unlike Excel RAND(). */
int fnrand(vp)
char *vp;
{
    char rv[4];
    int rn;

    eskp();
    if (*epos == ')')
    {
        epos++;
        itol(vp, rndnum());
        return 1;
    }
    if (!expr(rv))
        return 0;
    eskp();
    if (*epos != ')')
    {
        eok = 0;
        return 0;
    }
    epos++;
    rn = ltoi(rv);
    if (rn <= 0)
        itol(vp, rndnum());
    else
        itol(vp, rndnum() % rn);
    return 1;
}

/* SUM/AVG/MIN/MAX/COUNT share one A1:B5 range fold, selected by
 * 'tag' (0=SUM 1=AVG 2=MIN 3=MAX 4=COUNT). */
int dorange(vp, tag)
char *vp;
int tag;
{
    int r, c, r2, c2;
    int i, j;
    int cnt;
    int gotn;
    char rv[4];
    char dig[4];

    if (!prsref(&r, &c)) { eok = 0; return 0; }
    eskp();
    if (*epos != ':') { eok = 0; return 0; }
    epos++;
    if (!prsref(&r2, &c2)) { eok = 0; return 0; }
    eskp();
    if (*epos != ')') { eok = 0; return 0; }
    epos++;
    itol(vp, 0);
    cnt = 0;
    gotn = 0;
    for (i = r; i <= r2; i++)
    {
        for (j = c; j <= c2; j++)
        {
            /* A range that spans the formula's own cell (or any
             * cell currently being evaluated) is a circular
             * reference - reject it instead of counting/adding
             * itself. This also stops COUNT from counting its own
             * cell. */
            if (oncyc(i, j))
            {
                ecirc = 1;
                eok = 0;
                return 0;
            }
            if (tag == 4)
            {
                if (cells[i][j] && cells[i][j][0])
                    cnt++;
                continue;
            }
            if (!evcell(i, j, rv)) { eok = 0; return 0; }
            /* Only count cells that hold a numeric value (a number
             * or a formula) so AVG ignores empty / text cells. */
            if (cells[i][j] && (cells[i][j][0] == '='
                || cells[i][j][0] == '-'
                || isdig(cells[i][j][0])))
                cnt++;
            if (tag == 0 || tag == 1)
            {
                ladd(vp, vp, rv);
            }
            else if (tag == 2)
            {
                if (!gotn || lcomp(rv, vp) < 0)
                {
                    vp[0] = rv[0]; vp[1] = rv[1];
                    vp[2] = rv[2]; vp[3] = rv[3];
                }
                gotn = 1;
            }
            else if (tag == 3)
            {
                if (!gotn || lcomp(rv, vp) > 0)
                {
                    vp[0] = rv[0]; vp[1] = rv[1];
                    vp[2] = rv[2]; vp[3] = rv[3];
                }
                gotn = 1;
            }
        }
    }
    if (tag == 1)
    {
        if (cnt == 0) { eok = 0; return 0; }
        itol(dig, cnt);
        ldiv(vp, vp, dig);
    }
    else if (tag == 4)
        itol(vp, cnt);
    return 1;
}

int fnsum(vp)
char *vp;
{
    return dorange(vp, 0);
}

int fnavg(vp)
char *vp;
{
    return dorange(vp, 1);
}

int fnmin(vp)
char *vp;
{
    return dorange(vp, 2);
}

int fnmax(vp)
char *vp;
{
    return dorange(vp, 3);
}

int fncnt(vp)
char *vp;
{
    return dorange(vp, 4);
}

/* Dispatch table: parallel arrays pairing each function name with
 * its handler pointer. BDS C 1.6 has no aggregate initialisers
 * (no "= { ... }"), and a shared uninitialised external array would
 * collide with SHEETS.C's FORTRAN-COMMON layout, so the table is
 * filled locally each call. To add a function, append a name/handler
 * pair and bump the count in the loop. */
int dofn(vp)
char *vp;
{
    char *nm[6];
    int (*fn[6])();
    int i;

    nm[0] = "RAND";  fn[0] = fnrand;
    nm[1] = "SUM";   fn[1] = fnsum;
    nm[2] = "AVG";   fn[2] = fnavg;
    nm[3] = "MIN";   fn[3] = fnmin;
    nm[4] = "MAX";   fn[4] = fnmax;
    nm[5] = "COUNT"; fn[5] = fncnt;
    for (i = 0; i < 6; i++)
    {
        if (fnmatch(nm[i]))
            return (*fn[i])(vp);
    }
    return -1;
}

int factor(vp)
char *vp;
{
    int neg;
    int r, c;
    int rc;
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
    else if ((rc = dofn(vp)) >= 0)
    {
        /* A built-in function name matched at epos; dofn ran its
         * handler. rc == 1 means success (fall through to the sign
         * fix-up below); rc == 0 means the handler failed. */
        if (rc == 0)
            return 0;
    }
    else if (isal(*epos) || *epos == '$')
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

/* Return 1 if formula string 's' contains a "#REF!" token left
 * behind by a relocation that pushed a reference off the grid. */
int hsref(s)
char *s;
{
    while (*s)
    {
        if (s[0] == '#' && s[1] == 'R' && s[2] == 'E'
            && s[3] == 'F' && s[4] == '!')
            return 1;
        s++;
    }
    return 0;
}

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
        if (hsref(s))
        {
            strcpy(buf, "  #REF!   ");
            buf[CWID] = 0;
            return 0;
        }
        edepth = 0;
        ecirc = 0;
        ok = evcell(r, c, lv);
        if (!ok)
        {
            if (ecirc)
                strcpy(buf, "  #CIRC   ");
            else
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
