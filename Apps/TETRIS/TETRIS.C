/*
 * Tetris for Altair 8800 - VT100/xterm.js compatible
 * BDS C 1.6 on CP/M
 *
 * Fresh game engine with the original bright block-terminal theme:
 *  - 7-bag piece supply
 *  - Simple wall-kick rotation
 *  - Soft drop, hard drop, scoring, levels
 *  - EDIT.C-style ESC [ A/B/C/D cursor sequence handling
 */

int bdos();
int bios();
int inp();
int outp();

#define TMRID 2
#define TMRMS 25
#define TMRHI 28
#define TMRLO 29
#define ESC 27
#define KUP 5
#define KDN 24
#define KLT 19
#define KRT 4
#define KSP 32

#define BWID 10
#define BHGT 20
#define BROW 4
#define BCOL 30

#define NROW 6
#define NCOL 54

#define PNUL 0
#define PI 1
#define PO 2
#define PT 3
#define PS 4
#define PZ 5
#define PJ 6
#define PL 7

#define RUN 0
#define OVER 1
#define QUIT 2

#define KNON 0
#define KQUI 1
#define KLEF 2
#define KRIG 3
#define KROT 4
#define KSOF 5
#define KHAR 6

int grd[BHGT][BWID];
int shp[8][4];
int bag[7];
int bagn;

int actp;
int actr;
int actx;
int acty;
int nxtp;

int gstat;
int score;
int lines;
int lvl;
int tick;
int grav;

int prvp;
int prvr;
int prvx;
int prvy;
int prvo;

int escst;
int escct;

/* outch(c) - Write one console character. */
int outch(c)
int c;
{
    return bios(4, c);
}

/* pstr(s) - Print a NUL-terminated string. */
int pstr(s)
char *s;
{
    while (*s)
    {
        outch(*s);
        s++;
    }
    return 0;
}

/* pnum(n) - Print a non-negative decimal number. */
int pnum(n)
int n;
{
    char b[6];
    int i;

    if (n == 0)
    {
        outch('0');
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
        outch(b[i]);
    }
    return 0;
}

/* cur(r,c) - Move cursor to one-based row and column. */
int cur(r, c)
int r;
int c;
{
    outch(ESC);
    pstr("[");
    pnum(r);
    pstr(";");
    pnum(c);
    pstr("H");
    return 0;
}

/* cls() - Clear screen and reset attributes. */
int cls()
{
    outch(ESC);
    pstr("[0m");
    outch(ESC);
    pstr("[2J");
    cur(1, 1);
    return 0;
}

/* hide() - Hide the terminal cursor. */
int hide()
{
    outch(ESC);
    pstr("[?25l");
    return 0;
}

/* show() - Show the terminal cursor. */
int show()
{
    outch(ESC);
    pstr("[?25h");
    return 0;
}

/* rst() - Reset terminal attributes. */
int rst()
{
    outch(ESC);
    pstr("[0m");
    return 0;
}

/* setbg(c) - Set ANSI background color. */
int setbg(c)
int c;
{
    outch(ESC);
    pstr("[");
    pnum(c);
    pstr("m");
    return 0;
}

/* setfg(c) - Set ANSI foreground color. */
int setfg(c)
int c;
{
    outch(ESC);
    pstr("[");
    pnum(c);
    pstr("m");
    return 0;
}

/* keyrd() - Read raw key without waiting. */
int keyrd()
{
    return bdos(6, 0xFF) & 0xFF;
}

/* tset(ms) - Start timer 2 for ms milliseconds. */
int tset(ms)
int ms;
{
    outp(TMRHI, ms >> 8);
    outp(TMRLO, ms & 0xFF);
    return 0;
}

/* tact() - Return non-zero while timer 2 is active. */
int tact()
{
    return inp(TMRLO);
}

/* rnd() - Read a random word from emulator ports. */
unsigned rnd()
{
    unsigned r;

    outp(45, 1);
    r = inp(200);
    r = r | (inp(200) << 8);
    return r;
}

/* pbg(p) - Return block background color for a piece. */
int pbg(p)
int p;
{
    if (p == PI) return 46;
    if (p == PO) return 43;
    if (p == PT) return 45;
    if (p == PS) return 42;
    if (p == PZ) return 41;
    if (p == PJ) return 44;
    if (p == PL) return 103;
    return 47;
}

/* cbg(r,c) - Checker color for the cabinet border. */
int cbg(r, c)
int r;
int c;
{
    if (((r / 2) + (c / 2)) & 1)
        return 100;
    return 107;
}

/* inishp() - Build 4x4 rotation masks. */
int inishp()
{
    shp[PI][0] = 0x0F00; shp[PI][1] = 0x2222;
    shp[PI][2] = 0x00F0; shp[PI][3] = 0x4444;

    shp[PO][0] = 0x0660; shp[PO][1] = 0x0660;
    shp[PO][2] = 0x0660; shp[PO][3] = 0x0660;

    shp[PT][0] = 0x0E40; shp[PT][1] = 0x4C40;
    shp[PT][2] = 0x4E00; shp[PT][3] = 0x4640;

    shp[PS][0] = 0x06C0; shp[PS][1] = 0x4620;
    shp[PS][2] = 0x06C0; shp[PS][3] = 0x4620;

    shp[PZ][0] = 0x0C60; shp[PZ][1] = 0x2640;
    shp[PZ][2] = 0x0C60; shp[PZ][3] = 0x2640;

    shp[PJ][0] = 0x08E0; shp[PJ][1] = 0x6440;
    shp[PJ][2] = 0x0E20; shp[PJ][3] = 0x44C0;

    shp[PL][0] = 0x02E0; shp[PL][1] = 0x4460;
    shp[PL][2] = 0x0E80; shp[PL][3] = 0x0C44;
    return 0;
}

/* cell(p,r,y,x) - Test one cell in a piece mask. */
int cell(p, r, y, x)
int p;
int r;
int y;
int x;
{
    int bit;

    if (p < PI || p > PL)
        return 0;
    bit = y * 4 + x;
    return (shp[p][r & 3] >> (15 - bit)) & 1;
}

/* filbag() - Refill and shuffle the piece bag. */
int filbag()
{
    int i;
    int j;
    int t;

    for (i = 0; i < 7; i++)
        bag[i] = i + 1;

    for (i = 6; i > 0; i--)
    {
        j = rnd() % (i + 1);
        t = bag[i];
        bag[i] = bag[j];
        bag[j] = t;
    }

    bagn = 7;
    return 0;
}

/* newpcs() - Take the next piece from the bag. */
int newpcs()
{
    if (bagn <= 0)
        filbag();
    bagn--;
    return bag[bagn];
}

/* fit(p,r,x,y) - Return non-zero if a piece placement is legal. */
int fit(p, r, x, y)
int p;
int r;
int x;
int y;
{
    int py;
    int px;
    int by;
    int bx;

    for (py = 0; py < 4; py++)
    {
        for (px = 0; px < 4; px++)
        {
            if (cell(p, r, py, px))
            {
                by = y + py;
                bx = x + px;
                if (bx < 0 || bx >= BWID || by >= BHGT)
                    return 0;
                if (by >= 0 && grd[by][bx] != 0)
                    return 0;
            }
        }
    }
    return 1;
}

/* tile(p) - Draw one two-column block for a piece. */
int tile(p)
int p;
{
    if (p)
        setbg(pbg(p));
    else
        rst();
    outch(' ');
    outch(' ');
    return 0;
}

/* synbrd() - Redraw the fixed board contents. */
int synbrd()
{
    int r;
    int c;

    for (r = 0; r < BHGT; r++)
    {
        cur(BROW + r + 1, BCOL);
        for (c = 0; c < BWID; c++)
            tile(grd[r][c]);
    }
    rst();
    prvo = 0;
    return 0;
}

/* clrp(p,r,x,y) - Erase a previously drawn falling piece. */
int clrp(p, r, x, y)
int p;
int r;
int x;
int y;
{
    int py;
    int px;
    int by;
    int bx;

    for (py = 0; py < 4; py++)
    {
        by = y + py;
        if (by < 0 || by >= BHGT)
            continue;
        for (px = 0; px < 4; px++)
        {
            if (cell(p, r, py, px))
            {
                bx = x + px;
                if (bx >= 0 && bx < BWID)
                {
                    cur(BROW + by + 1, BCOL + bx * 2);
                    tile(grd[by][bx]);
                }
            }
        }
    }
    rst();
    return 0;
}

/* drwp(p,r,x,y) - Draw the falling piece. */
int drwp(p, r, x, y)
int p;
int r;
int x;
int y;
{
    int py;
    int px;
    int by;
    int bx;

    setbg(pbg(p));
    for (py = 0; py < 4; py++)
    {
        by = y + py;
        if (by < 0 || by >= BHGT)
            continue;
        for (px = 0; px < 4; px++)
        {
            if (cell(p, r, py, px))
            {
                bx = x + px;
                if (bx >= 0 && bx < BWID)
                {
                    cur(BROW + by + 1, BCOL + bx * 2);
                    outch(' ');
                    outch(' ');
                }
            }
        }
    }
    rst();
    return 0;
}

/* actdrw() - Refresh only the active falling piece. */
int actdrw()
{
    if (prvo)
        clrp(prvp, prvr, prvx, prvy);

    if (gstat == RUN && actp != PNUL)
    {
        drwp(actp, actr, actx, acty);
        prvp = actp;
        prvr = actr;
        prvx = actx;
        prvy = acty;
        prvo = 1;
    }
    else
    {
        prvo = 0;
    }
    cur(5, 1);
    return 0;
}

/* title() - Draw heading and controls. */
int title()
{
    cur(1, 1);
    setfg(36);
    pstr("TETRIS");
    rst();
    pstr(" for ");
    setfg(33);
    pstr("Altair 8800");
    rst();
    pstr(" V2.0");

    cur(2, 1);
    setfg(37);
    pstr("LEFT/RIGHT Move  UP Rotate  DOWN Soft Drop  SPACE Drop  ESC/Q Quit");
    rst();
    return 0;
}

/* border() - Draw the colourful board cabinet. */
int border()
{
    int r;
    int c;
    int left;

    left = BCOL - 2;
    for (r = 0; r < BHGT + 2; r++)
    {
        cur(BROW + r, left);
        for (c = 0; c < BWID + 2; c++)
        {
            setbg(cbg(r, c));
            outch(' ');
            outch(' ');
        }
    }
    rst();
    return 0;
}

/* stats() - Draw score, line and level values. */
int stats()
{
    cur(4, 1);
    setfg(35);
    pstr("STATUS");
    rst();

    cur(5, 1);
    setfg(36);
    pstr("Score: ");
    setfg(33);
    pnum(score);
    rst();
    pstr("          ");

    cur(6, 1);
    setfg(36);
    pstr("Lines: ");
    setfg(33);
    pnum(lines);
    rst();
    pstr("          ");

    cur(7, 1);
    setfg(36);
    pstr("Level: ");
    setfg(33);
    pnum(lvl);
    rst();
    pstr("          ");
    return 0;
}

/* nextdr() - Draw the next piece preview. */
int nextdr()
{
    int y;
    int x;

    cur(NROW, NCOL);
    setfg(36);
    pstr("NEXT");
    rst();

    for (y = 0; y < 4; y++)
    {
        cur(NROW + y + 1, NCOL);
        pstr("        ");
    }

    if (nxtp < PI || nxtp > PL)
        return 0;

    setbg(pbg(nxtp));
    for (y = 0; y < 4; y++)
    {
        for (x = 0; x < 4; x++)
        {
            if (cell(nxtp, 0, y, x))
            {
                cur(NROW + y + 1, NCOL + x * 2);
                outch(' ');
                outch(' ');
            }
        }
    }
    rst();
    return 0;
}

/* setspd() - Recompute gravity from level. */
int setspd()
{
    grav = 20 - lvl;
    if (grav < 5)
        grav = 5;
    return 0;
}

/* addscr(n) - Add line-clear points. */
int addscr(n)
int n;
{
    if (n == 1) score += 40 * (lvl + 1);
    if (n == 2) score += 100 * (lvl + 1);
    if (n == 3) score += 300 * (lvl + 1);
    if (n == 4) score += 1200 * (lvl + 1);
    return 0;
}

/* clrlin() - Remove full rows and compact the board. */
int clrlin()
{
    int r;
    int c;
    int s;
    int full;
    int got;

    got = 0;
    for (r = BHGT - 1; r >= 0; r--)
    {
        full = 1;
        for (c = 0; c < BWID; c++)
        {
            if (grd[r][c] == 0)
            {
                full = 0;
                break;
            }
        }

        if (full)
        {
            got++;
            for (s = r; s > 0; s--)
                for (c = 0; c < BWID; c++)
                    grd[s][c] = grd[s - 1][c];
            for (c = 0; c < BWID; c++)
                grd[0][c] = 0;
            r++;
        }
    }

    if (got)
    {
        lines += got;
        addscr(got);
        lvl = lines / 10;
        if (lvl > 15)
            lvl = 15;
        setspd();
        synbrd();
        stats();
    }
    return got;
}

/* stamp() - Merge the active piece into the board. */
int stamp()
{
    int py;
    int px;
    int by;
    int bx;

    for (py = 0; py < 4; py++)
    {
        for (px = 0; px < 4; px++)
        {
            if (cell(actp, actr, py, px))
            {
                by = acty + py;
                bx = actx + px;
                if (by >= 0 && by < BHGT && bx >= 0 && bx < BWID)
                    grd[by][bx] = actp;
            }
        }
    }
    actp = PNUL;
    return 0;
}

/* spawn() - Start a new falling piece. */
int spawn()
{
    actp = nxtp;
    actr = 0;
    actx = 3;
    acty = -1;
    tick = 0;

    nxtp = newpcs();
    nextdr();

    if (!fit(actp, actr, actx, acty))
    {
        gstat = OVER;
        return 0;
    }

    actdrw();
    stats();
    return 1;
}

/* lock() - Freeze active piece, clear rows, then spawn. */
int lock()
{
    stamp();
    actdrw();
    clrlin();
    if (gstat == RUN)
        spawn();
    return 0;
}

/* trymv(x,y,r) - Try to place the active piece. */
int trymv(x, y, r)
int x;
int y;
int r;
{
    if (!fit(actp, r, x, y))
        return 0;
    actx = x;
    acty = y;
    actr = r & 3;
    tick = 0;
    actdrw();
    return 1;
}

/* rotp() - Rotate with small horizontal wall kicks. */
int rotp()
{
    int nr;

    nr = (actr + 1) & 3;
    if (trymv(actx, acty, nr)) return 1;
    if (trymv(actx - 1, acty, nr)) return 1;
    if (trymv(actx + 1, acty, nr)) return 1;
    if (trymv(actx - 2, acty, nr)) return 1;
    if (trymv(actx + 2, acty, nr)) return 1;
    return 0;
}

/* soft() - Drop one row or lock if blocked. */
int soft()
{
    if (trymv(actx, acty + 1, actr))
    {
        score++;
        stats();
        return 1;
    }
    lock();
    return 0;
}

/* hard() - Drop to the bottom and lock. */
int hard()
{
    while (fit(actp, actr, actx, acty + 1))
    {
        acty++;
        score += 2;
    }
    tick = 0;
    actdrw();
    stats();
    lock();
    return 0;
}

/* step() - Advance gravity one tick. */
int step()
{
    tick++;
    if (tick < grav)
        return 0;
    tick = 0;

    if (fit(actp, actr, actx, acty + 1))
    {
        acty++;
        actdrw();
    }
    else
    {
        lock();
    }
    return 0;
}

/* keymap(c) - Decode SDK keys and ANSI cursor sequences. */
int keymap(c)
int c;
{
    if (escst == 2)
    {
        escst = 0;
        if (c == 'A') return KROT;
        if (c == 'B') return KSOF;
        if (c == 'C') return KRIG;
        if (c == 'D') return KLEF;
        return KNON;
    }

    if (escst == 1)
    {
        escst = 0;
        if (c == '[')
        {
            escst = 2;
            return KNON;
        }
        return KQUI;
    }

    if (c == ESC)
    {
        escst = 1;
        escct = 0;
        return KNON;
    }
    if (c == 'q' || c == 'Q') return KQUI;
    if (c == KLT) return KLEF;
    if (c == KRT) return KRIG;
    if (c == KUP) return KROT;
    if (c == KDN) return KSOF;
    if (c == KSP) return KHAR;
    return KNON;
}

/* dokey(k) - Apply one decoded command. */
int dokey(k)
int k;
{
    if (k == KQUI)
    {
        gstat = QUIT;
        return 0;
    }
    if (gstat != RUN)
        return 0;
    if (k == KLEF) trymv(actx - 1, acty, actr);
    if (k == KRIG) trymv(actx + 1, acty, actr);
    if (k == KROT) rotp();
    if (k == KSOF) soft();
    if (k == KHAR) hard();
    return 0;
}

/* esctik() - Let a lone ESC become quit after a tiny grace period. */
int esctik()
{
    if (escst == 1)
    {
        escct++;
        if (escct > 2)
        {
            escst = 0;
            gstat = QUIT;
        }
    }
    return 0;
}

/* overdr() - Draw final message. */
int overdr()
{
    cur(15, 5);
    if (gstat == QUIT)
    {
        setfg(36);
        pstr("GAME QUIT ");
    }
    else
    {
        setfg(35);
        pstr("GAME OVER!");
    }
    rst();

    cur(16, 5);
    setfg(33);
    pstr("Final Score: ");
    rst();
    pnum(score);

    cur(17, 5);
    setfg(33);
    pstr("Lines Cleared: ");
    rst();
    pnum(lines);

    cur(18, 5);
    setfg(33);
    pstr("Level Reached: ");
    rst();
    pnum(lvl);
    return 0;
}

/* ginit() - Reset game state. */
int ginit()
{
    int r;
    int c;

    for (r = 0; r < BHGT; r++)
        for (c = 0; c < BWID; c++)
            grd[r][c] = 0;

    bagn = 0;
    actp = PNUL;
    nxtp = newpcs();
    gstat = RUN;
    score = 0;
    lines = 0;
    lvl = 0;
    tick = 0;
    setspd();
    prvo = 0;
    escst = 0;
    escct = 0;
    return 0;
}

/* main() - Program entry point. */
int main()
{
    int ch;
    int k;

    inishp();
    ginit();

    cls();
    hide();
    title();
    border();
    synbrd();
    stats();
    nextdr();
    spawn();

    while (gstat == RUN)
    {
        tset(TMRMS);
        step();

        while (tact() && gstat == RUN)
        {
            ch = keyrd();
            if (ch)
            {
                k = keymap(ch);
                if (k)
                    dokey(k);
            }
        }
        esctik();
    }

    actdrw();
    stats();
    overdr();

    cur(24, 1);
    show();
    pstr("Thanks for playing Tetris!\r\n");
    return 0;
}
