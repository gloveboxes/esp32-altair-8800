/*
 * MANCALA.C - Mancala / African Bean Game for Altair 8800 CP/M.
 *
 * BDS C 1.6, VT100/xterm.js display, 30 rows by 80 columns.
 * Player vs computer, Kalah-style rules.
 *
 * Keys:
 *   1/2/3 at startup   Easy / Medium / Hard
 *   Left/Right arrows  Select pit
 *   Space or Return    Sow selected pit
 *   Q, ESC, Ctrl-C     Quit
 */

#include "dxterm.h"
#include "dxsys.h"

#define PITS 6
#define TOT 14
#define HST 6
#define CST 13

#define SCRW 80
#define SCRH 30
#define MINC 3
#define MAXC 78

#define BROW 7
#define BCOL 18
#define PCW 7
#define PCH 4
#define PNLT 5
#define PNLL 5
#define PNLW 72
#define PNLH 21

#define KEYQ 81
#define KEYq 113
#define KEYR 13

#define KNON 0
#define KLT 1
#define KRT 2
#define KPLC 3
#define KQUI 4

int pit[TOT];
int sel;
int quit;
int over;
int escst;
int lvl;

/* pstr(s) - Print a NUL-terminated string. */
int pstr(s)
char *s;
{
    while (*s)
    {
        x_cout(*s);
        s++;
    }
    return 0;
}

/* num2(n) - Print a two-column number. */
int num2(n)
int n;
{
    if (n < 10)
        x_cout(' ');
    x_numpr(n);
    return 0;
}

/* wood() - Select the board's dark wood color. */
int wood()
{
    x_setc(44);
    x_setc(97);
    return 0;
}

/* hole() - Select recessed pit color. */
int hole()
{
    x_setc(41);
    x_setc(97);
    return 0;
}

/* cbg(r,c) - African-inspired checker border color. */
int cbg(r, c)
int r;
int c;
{
    if (((r / 2) + (c / 2)) & 1)
    {
        if (r & 1)
            return 42;
        return 43;
    }
    if (c & 1)
        return 41;
    return 44;
}

/* brdr() - Draw the checker outline. */
int brdr()
{
    int r;
    int c;

    for (c = 0; c < SCRW; c += 2)
    {
        x_setc(cbg(0, c / 2));
        x_curmv(2, c + 1);
        x_cout(' ');
        x_cout(' ');
        x_setc(cbg(SCRH - 1, c / 2));
        x_curmv(SCRH, c + 1);
        x_cout(' ');
        x_cout(' ');
    }

    for (r = 3; r < SCRH; r++)
    {
        x_setc(cbg(r - 2, 0));
        x_curmv(r, 1);
        x_cout(' ');
        x_cout(' ');
        x_setc(cbg(r - 2, (SCRW / 2) - 1));
        x_curmv(r, SCRW - 1);
        x_cout(' ');
        x_cout(' ');
    }
    x_rstc();
    return 0;
}

/* stat() - Draw score stores and turn. */
int stat()
{
    x_rstc();
    x_setc(37);
    x_curmv(1, 3);
    pstr("MANCALA - THE AFRICAN BEAN GAME");
    x_curmv(1, 49);
    pstr("YOU:");
    num2(pit[HST]);
    pstr("  CPU:");
    num2(pit[CST]);
    pstr("  ");
    if (lvl == 0)
        pstr("EASY");
    else if (lvl == 1)
        pstr("MED");
    else
        pstr("HARD");
    x_ereol();
    return 0;
}

/* pcol(i) - Screen column for pit i. */
int pcol(i)
int i;
{
    int c;

    if (i < HST)
        c = BCOL + (i * (PCW + 1));
    else
        c = BCOL + ((12 - i) * (PCW + 1));
    return c;
}

/* prow(i) - Screen row for pit i. */
int prow(i)
int i;
{
    if (i < HST)
        return BROW + 10;
    return BROW + 1;
}

/* bean(n) - Draw bean dots for a pit. */
int bean(n)
int n;
{
    int i;
    int m;

    m = n;
    if (m > 5)
        m = 5;
    for (i = 0; i < m; i++)
        x_cout('o');
    for (; i < 6; i++)
        x_cout(' ');
    return 0;
}

/* drpit(i,hi) - Draw one small pit. */
int drpit(i, hi)
int i;
int hi;
{
    int r;
    int c;
    int row;

    r = prow(i);
    c = pcol(i);
    for (row = 0; row < PCH; row++)
    {
        x_curmv(r + row, c);
        if (hi)
        {
            x_setc(43);
            x_setc(30);
        }
        else
            hole();
        if (row == 0 || row == PCH - 1)
            pstr("       ");
        else if (row == 1)
        {
            x_cout(' ');
            num2(pit[i]);
            pstr("    ");
        }
        else
        {
            x_cout(' ');
            if (pit[i] > 0)
                bean(pit[i]);
            else
                pstr("      ");
        }
        x_rstc();
    }
    return 0;
}

/* drstor(i) - Draw one store. */
int drstor(i)
int i;
{
    int r;
    int c;
    int row;

    if (i == HST)
        c = 67;
    else
        c = 8;
    r = BROW + 4;
    for (row = 0; row < 8; row++)
    {
        x_curmv(r + row, c);
        hole();
        if (row == 0 || row == 7)
            pstr("       ");
        else if (row == 2)
        {
            pstr("  ");
            num2(pit[i]);
            pstr("   ");
        }
        else if (row == 4)
        {
            pstr(" ");
            if (i == HST)
                pstr("YOU ");
            else
                pstr("CPU ");
            pstr("  ");
        }
        else
            pstr("       ");
        x_rstc();
    }
    return 0;
}

/* clrnot(r) - Clear a message row without touching border. */
int clrnot(r)
int r;
{
    int i;

    x_rstc();
    x_curmv(r, 5);
    for (i = 0; i < 72; i++)
        x_cout(' ');
    return 0;
}

/* panel() - Draw the carved wooden board panel. */
int panel()
{
    int r;
    int c;

    for (r = 0; r < PNLH; r++)
    {
        x_curmv(PNLT + r, PNLL);
        wood();
        for (c = 0; c < PNLW; c++)
            x_cout(' ');
        x_rstc();
    }
    for (r = PNLT + 3; r < PNLT + PNLH - 2; r += 4)
    {
        x_curmv(r, PNLL + 4);
        wood();
        x_setc(33);
        pstr("................................................................");
        x_rstc();
    }
    return 0;
}

/* grain() - Add carved board lines. */
int grain()
{
    int r;

    x_setc(33);
    for (r = 9; r <= 23; r += 4)
    {
        x_curmv(r, 13);
        pstr("......................................................");
    }
    x_rstc();
    return 0;
}

/* labels() - Draw pit labels. */
int labels()
{
    int i;

    x_rstc();
    wood();
    x_setc(93);
    x_curmv(6, 30);
    pstr("COMPUTER SIDE");
    x_rstc();
    wood();
    x_setc(93);
    x_curmv(24, 33);
    pstr("YOUR SIDE");
    for (i = 0; i < PITS; i++)
    {
        wood();
        x_setc(93);
        x_curmv(BROW + 15, pcol(i) + 3);
        x_cout('1' + i);
        x_rstc();
    }
    x_rstc();
    return 0;
}

/* note(s) - Draw status message. */
int note(s)
char *s;
{
    clrnot(27);
    x_setc(37);
    x_curmv(27, 5);
    pstr(s);
    x_rstc();
    return 0;
}

/* drall() - Draw board and pits. */
int drall()
{
    int i;

    x_clrsc();
    x_hidcr();
    brdr();
    stat();
    panel();
    labels();
    drstor(CST);
    drstor(HST);
    for (i = 0; i < HST; i++)
        drpit(i, i == sel);
    for (i = 7; i < CST; i++)
        drpit(i, 0);
    note("ARROWS OR 1-6 SELECT, SPACE SOWS, Q QUITS");
    return 0;
}

/* drpos() - Redraw pit and store positions only. */
int drpos()
{
    int i;

    stat();
    drstor(CST);
    drstor(HST);
    for (i = 0; i < HST; i++)
        drpit(i, i == sel);
    for (i = 7; i < CST; i++)
        drpit(i, 0);
    return 0;
}

/* redraw(i) - Redraw pit or store by index. */
int redraw(i)
int i;
{
    if (i == HST || i == CST)
        drstor(i);
    else if (i < HST)
        drpit(i, i == sel);
    else
        drpit(i, 0);
    stat();
    return 0;
}

/* init() - Initialize game state. */
int init()
{
    int i;

    for (i = 0; i < TOT; i++)
        pit[i] = 4;
    pit[HST] = 0;
    pit[CST] = 0;
    sel = 0;
    quit = 0;
    over = 0;
    escst = 0;
    lvl = 2;
    return 0;
}

/* picklv() - Select computer difficulty. */
int picklv()
{
    int c;

    x_clrsc();
    x_hidcr();
    brdr();
    x_setc(37);
    x_curmv(9, 22);
    pstr("MANCALA - SELECT COMPUTER LEVEL");
    x_curmv(12, 28);
    pstr("1  EASY    RANDOM PLAY");
    x_curmv(14, 28);
    pstr("2  MEDIUM  MINIMAX DEPTH 2");
    x_curmv(16, 28);
    pstr("3  HARD    MINIMAX DEPTH 4");
    x_curmv(20, 24);
    pstr("PRESS 1, 2, OR 3   (RETURN = HARD)");
    x_rstc();
    while (1)
    {
        c = x_keyrd();
        if (c == '1')
        {
            lvl = 0;
            return 0;
        }
        if (c == '2')
        {
            lvl = 1;
            return 0;
        }
        if (c == '3' || c == KEYR)
        {
            lvl = 2;
            return 0;
        }
        if (c == KEYQ || c == KEYq || c == XK_ESC || x_iscc(c))
        {
            quit = 1;
            return 0;
        }
    }
    return 0;
}

/* keymap(c) - Decode raw or translated control keys. */
int keymap(c)
int c;
{
    if (escst == 2)
    {
        if (c == 0)
            return KNON;
        escst = 0;
        if (c == 'C')
            return KRT;
        if (c == 'D')
            return KLT;
        return KNON;
    }

    if (escst == 1)
    {
        if (c == 0)
            return KNON;
        escst = 0;
        if (c == '[')
        {
            escst = 2;
            return KNON;
        }
        return KQUI;
    }

    if (c == 0)
        return KNON;
    if (c == XK_ESC)
    {
        escst = 1;
        return KNON;
    }
    if (x_iscc(c) || c == KEYQ || c == KEYq)
        return KQUI;
    if (x_islt(c))
        return KLT;
    if (x_isrt(c))
        return KRT;
    if (x_isspc(c) || c == KEYR)
        return KPLC;
    return KNON;
}

/* getact() - Read and decode one pending action. */
int getact()
{
    int c;

    c = x_keyrd();
    return keymap(c);
}

/* sumh() - Count beans on human side. */
int sumh()
{
    int i;
    int s;

    s = 0;
    for (i = 0; i < HST; i++)
        s += pit[i];
    return s;
}

/* sumc() - Count beans on computer side. */
int sumc()
{
    int i;
    int s;

    s = 0;
    for (i = 7; i < CST; i++)
        s += pit[i];
    return s;
}

/* colend() - Collect remaining beans at game end. */
int colend()
{
    int i;

    for (i = 0; i < HST; i++)
    {
        pit[HST] += pit[i];
        pit[i] = 0;
    }
    for (i = 7; i < CST; i++)
    {
        pit[CST] += pit[i];
        pit[i] = 0;
    }
    over = 1;
    return 0;
}

/* opp(i) - Return opposite pit index. */
int opp(i)
int i;
{
    return 12 - i;
}

/* owned(i,p) - Test whether pit i belongs to player p. */
int owned(i, p)
int i;
int p;
{
    if (p == 0 && i >= 0 && i < HST)
        return 1;
    if (p == 1 && i > HST && i < CST)
        return 1;
    return 0;
}

/* store(p) - Return store index for player p. */
int store(p)
int p;
{
    if (p == 0)
        return HST;
    return CST;
}

/* skip(i,p) - Test whether index i is opponent store. */
int skip(i, p)
int i;
int p;
{
    if (p == 0 && i == CST)
        return 1;
    if (p == 1 && i == HST)
        return 1;
    return 0;
}

/* move(i,p) - Sow from pit i for player p. */
int move(i, p)
int i;
int p;
{
    int cnt;
    int pos;
    int op;
    int st;

    if (!owned(i, p) || pit[i] == 0)
        return 0;
    cnt = pit[i];
    pit[i] = 0;
    redraw(i);
    pos = i;
    while (cnt > 0)
    {
        pos++;
        if (pos >= TOT)
            pos = 0;
        if (!skip(pos, p))
        {
            pit[pos]++;
            cnt--;
            redraw(pos);
        }
    }

    st = store(p);
    if (owned(pos, p) && pit[pos] == 1)
    {
        op = opp(pos);
        if (pit[op] > 0)
        {
            pit[st] += pit[op] + 1;
            pit[op] = 0;
            pit[pos] = 0;
            redraw(op);
            redraw(pos);
            redraw(st);
            if (p == 0)
                note("CAPTURE! THE OPPOSITE PIT FALLS TO YOU");
            else
                note("COMPUTER CAPTURES FROM THE OPPOSITE PIT");
        }
    }

    if (sumh() == 0 || sumc() == 0)
    {
        colend();
        drpos();
        return 0;
    }
    if (pos == st)
        return 2;
    return 1;
}

/* legal(p) - Return non-zero if player has a move. */
int legal(p)
int p;
{
    int i;

    if (p == 0)
    {
        for (i = 0; i < HST; i++)
            if (pit[i] > 0)
                return 1;
    }
    else
    {
        for (i = 7; i < CST; i++)
            if (pit[i] > 0)
                return 1;
    }
    return 0;
}

/* bcopy() - Copy a simulated board. */
int bcopy(dst, src)
int dst[];
int src[];
{
    int i;

    for (i = 0; i < TOT; i++)
        dst[i] = src[i];
    return 0;
}

/* bsumh() - Count simulated human beans. */
int bsumh(b)
int b[];
{
    int i;
    int s;

    s = 0;
    for (i = 0; i < HST; i++)
        s += b[i];
    return s;
}

/* bsumc() - Count simulated computer beans. */
int bsumc(b)
int b[];
{
    int i;
    int s;

    s = 0;
    for (i = 7; i < CST; i++)
        s += b[i];
    return s;
}

/* blegal() - Test simulated legal move. */
int blegal(b, p)
int b[];
int p;
{
    int i;

    if (p == 0)
    {
        for (i = 0; i < HST; i++)
            if (b[i] > 0)
                return 1;
    }
    else
    {
        for (i = 7; i < CST; i++)
            if (b[i] > 0)
                return 1;
    }
    return 0;
}

/* bfin() - Sweep simulated end-game beans. */
int bfin(b)
int b[];
{
    int i;

    for (i = 0; i < HST; i++)
    {
        b[HST] += b[i];
        b[i] = 0;
    }
    for (i = 7; i < CST; i++)
    {
        b[CST] += b[i];
        b[i] = 0;
    }
    return 0;
}

/* bmov() - Sow on a simulated board. */
int bmov(b, i, p)
int b[];
int i;
int p;
{
    int cnt;
    int pos;
    int op;
    int st;

    if (!owned(i, p) || b[i] == 0)
        return 0;
    cnt = b[i];
    b[i] = 0;
    pos = i;
    while (cnt > 0)
    {
        pos++;
        if (pos >= TOT)
            pos = 0;
        if (!skip(pos, p))
        {
            b[pos]++;
            cnt--;
        }
    }

    st = store(p);
    if (owned(pos, p) && b[pos] == 1)
    {
        op = opp(pos);
        if (b[op] > 0)
        {
            b[st] += b[op] + 1;
            b[op] = 0;
            b[pos] = 0;
        }
    }
    if (bsumh(b) == 0 || bsumc(b) == 0)
    {
        bfin(b);
        return 0;
    }
    if (pos == st)
        return 2;
    return 1;
}

/* bmob() - Count legal simulated moves. */
int bmob(b, p)
int b[];
int p;
{
    int i;
    int n;

    n = 0;
    if (p == 0)
    {
        for (i = 0; i < HST; i++)
            if (b[i] > 0)
                n++;
    }
    else
    {
        for (i = 7; i < CST; i++)
            if (b[i] > 0)
                n++;
    }
    return n;
}

/* bscore() - Evaluate a simulated board for the computer. */
int bscore(b)
int b[];
{
    int cdif;
    int pdif;
    int mdif;

    if (bsumh(b) == 0 || bsumc(b) == 0)
        return ((b[CST] + bsumc(b)) - (b[HST] + bsumh(b))) * 1000;
    cdif = b[CST] - b[HST];
    pdif = bsumc(b) - bsumh(b);
    mdif = bmob(b, 1) - bmob(b, 0);
    return (cdif * 100) + (pdif * 4) + (mdif * 3);
}

/* mmx() - Minimax score for simulated play. */
int mmx(b, p, dep)
int b[];
int p;
int dep;
{
    int i;
    int res;
    int val;
    int best;
    int nb[TOT];

    if (dep <= 0 || bsumh(b) == 0 || bsumc(b) == 0)
        return bscore(b);
    if (!blegal(b, p))
        return bscore(b);

    if (p == 1)
        best = -30000;
    else
        best = 30000;

    if (p == 0)
    {
        for (i = 0; i < HST; i++)
        {
            if (b[i] == 0)
                continue;
            bcopy(nb, b);
            res = bmov(nb, i, p);
            if (res == 2)
                val = mmx(nb, p, dep - 1);
            else
                val = mmx(nb, 1, dep - 1);
            if (val < best)
                best = val;
        }
    }
    else
    {
        for (i = 7; i < CST; i++)
        {
            if (b[i] == 0)
                continue;
            bcopy(nb, b);
            res = bmov(nb, i, p);
            if (res == 2)
                val = mmx(nb, p, dep - 1);
            else
                val = mmx(nb, 0, dep - 1);
            if (val > best)
                best = val;
        }
    }
    return best;
}

/* rpick() - Pick a random legal computer pit. */
int rpick()
{
    int i;
    int n;
    int pick;
    unsigned r;

    n = 0;
    for (i = 7; i < CST; i++)
        if (pit[i] > 0)
            n++;
    if (n == 0)
        return 7;
    r = x_rand();
    pick = r % n;
    for (i = 7; i < CST; i++)
    {
        if (pit[i] > 0)
        {
            if (pick == 0)
                return i;
            pick--;
        }
    }
    return 7;
}

/* nxsel(d) - Move selected human pit. */
int nxsel(d)
int d;
{
    int old;

    old = sel;
    sel += d;
    if (sel < 0)
        sel = PITS - 1;
    if (sel >= PITS)
        sel = 0;
    drpit(old, 0);
    drpit(sel, 1);
    return 0;
}

/* setsel(n) - Select human pit by number. */
int setsel(n)
int n;
{
    int old;

    if (n < 0 || n >= PITS)
        return 0;
    old = sel;
    sel = n;
    drpit(old, 0);
    drpit(sel, 1);
    return 0;
}

/* human() - Process the human turn. */
int human()
{
    int k;
    int c;
    int res;

    note("YOUR TURN: CHOOSE A PIT WITH BEANS");
    while (!quit && !over)
    {
        c = x_keyrd();
        k = keymap(c);
        if (c >= '1' && c <= '6')
        {
            setsel(c - '1');
            continue;
        }
        if (k == KNON)
            continue;
        if (k == KQUI)
        {
            quit = 1;
            return 0;
        }
        if (k == KLT)
            nxsel(-1);
        else if (k == KRT)
            nxsel(1);
        else if (k == KPLC)
        {
            if (pit[sel] == 0)
                note("THAT PIT IS EMPTY");
            else
            {
                res = move(sel, 0);
                if (res == 2 && !over)
                    note("YOU LANDED IN YOUR STORE: GO AGAIN");
                else
                    return 0;
            }
        }
    }
    return 0;
}

/* eval(i) - Score a computer move. */
int eval(i)
int i;
{
    int cnt;
    int pos;
    int val;
    int op;

    if (pit[i] == 0)
        return -1;
    cnt = pit[i];
    pos = i;
    val = pit[i];
    while (cnt > 0)
    {
        pos++;
        if (pos >= TOT)
            pos = 0;
        if (pos != HST)
            cnt--;
    }
    if (pos == CST)
        val += 60;
    if (owned(pos, 1) && pit[pos] == 0)
    {
        op = opp(pos);
        if (pit[op] > 0)
            val += pit[op] + 30;
    }
    if (i == 7 || i == 12)
        val += 2;
    return val;
}

/* cpick() - Pick the computer's pit. */
int cpick()
{
    int i;
    int best;
    int bval;
    int val;
    int dep;
    int res;
    int nb[TOT];

    if (lvl == 0)
        return rpick();

    if (lvl == 1)
        dep = 2;
    else
        dep = 4;

    best = 7;
    bval = -30000;
    for (i = 7; i < CST; i++)
    {
        if (pit[i] == 0)
            continue;
        bcopy(nb, pit);
        res = bmov(nb, i, 1);
        if (res == 2)
            val = mmx(nb, 1, dep - 1);
        else
            val = mmx(nb, 0, dep - 1);
        val += eval(i);
        if (val > bval)
        {
            bval = val;
            best = i;
        }
    }
    return best;
}

/* pause() - Small thinking pause. */
int pause()
{
    int i;
    int j;

    for (i = 0; i < 250; i++)
        for (j = 0; j < 120; j++)
            ;
    return 0;
}

/* comp() - Run computer turns. */
int comp()
{
    int i;
    int res;

    while (!quit && !over)
    {
        note("COMPUTER IS THINKING...");
        pause();
        i = cpick();
        note("COMPUTER SOWS");
        res = move(i, 1);
        if (res == 2 && !over)
        {
            note("COMPUTER LANDED IN ITS STORE: AGAIN");
            pause();
        }
        else
            return 0;
    }
    return 0;
}

/* final() - Show winner. */
int final()
{
    clrnot(26);
    clrnot(27);
    clrnot(28);
    x_setc(37);
    x_curmv(26, 5);
    if (quit)
        pstr("GAME QUIT");
    else if (pit[HST] > pit[CST])
        pstr("YOU WIN! MORE BEANS IN YOUR STORE");
    else if (pit[HST] < pit[CST])
        pstr("COMPUTER WINS THIS HARVEST");
    else
        pstr("DRAW GAME");
    x_curmv(27, 5);
    pstr("FINAL  YOU:");
    num2(pit[HST]);
    pstr("  CPU:");
    num2(pit[CST]);
    x_rstc();
    return 0;
}

/* main() - Game entry point. */
int main()
{
    init();
    picklv();
    if (quit)
    {
        x_shwcr();
        x_rstc();
        pstr("\r\n");
        return 0;
    }
    drall();

    while (!quit && !over)
    {
        if (legal(0))
            human();
        if (!quit && !over && legal(1))
            comp();
        if (!legal(0) || !legal(1))
        {
            colend();
            drpos();
        }
    }

    final();
    x_curmv(SCRH, 1);
    x_shwcr();
    x_rstc();
    pstr("\r\n");
    return 0;
}