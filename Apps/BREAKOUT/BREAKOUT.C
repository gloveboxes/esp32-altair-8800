/*
 * BREAKOUT.C - Breakout for Altair 8800 / CP/M / BDS C.
 *
 * VT100/xterm.js display, BDS C 1.6 syntax.
 * All local symbols are seven characters or fewer.
 *
 * Keys:
 *   Left/Right arrows  Move paddle
 *   Space             Launch ball
 *   ESC or Ctrl-C      Quit
 */

#define ESC 27
#define CTLC 3
#define SPC 32
#define KRT 4
#define KLT 19

#define TID 1
#define TMS 20

#define SCRW 80
#define SCRH 30
#define MINR 3
#define MAXR 30
#define MINC 3
#define MAXC 78

#define BRS 8
#define BCS 13
#define BRW 4
#define BRG 1
#define BRO 7
#define BCO 8

#define PADR 29
#define PADW 8
#define PADL 5

#define WAIT 0
#define PLAY 1
#define LOSE 2
#define DONE 3

int bdos();
int bios();
int x_tset();
int x_texp();
unsigned x_rand();

int br[BRS][BCS];
int bx;
int by;
int dx;
int dy;
int pad;
int score;
int lives;
int left;
int state;
int tick;
int spd;
int quit;

/* chout(c) - Write one console character. */
int chout(c)
int c;
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

/* clrs() - Clear the terminal and reset attributes. */
int clrs()
{
    chout(ESC);
    cput("[0m");
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

/* hid() - Hide cursor. */
int hid()
{
    chout(ESC);
    cput("[?25l");
    return 0;
}

/* shw() - Show cursor. */
int shw()
{
    chout(ESC);
    cput("[?25h");
    return 0;
}

/* rst() - Reset text attributes. */
int rst()
{
    chout(ESC);
    cput("[0m");
    return 0;
}

/* col(c) - Set ANSI foreground color. */
int col(c)
int c;
{
    chout(ESC);
    cput("[");
    nump(c);
    cput("m");
    return 0;
}

/* bg(c) - Set ANSI background color. */
int bg(c)
int c;
{
    chout(ESC);
    cput("[");
    nump(c);
    cput("m");
    return 0;
}

/* cbg(r,c) - Checker color for the board border. */
int cbg(r, c)
int r;
int c;
{
    if (((r / 2) + (c / 2)) & 1)
        return 100;
    return 107;
}

/* keygt() - Read one key if ready. */
int keygt()
{
    if (!(bdos(11) & 0xFF))
        return 0;
    return bdos(6, 0xFF) & 0xFF;
}

/* drstat() - Draw score and lives. */
int drstat()
{
    col(37);
    curmv(1, 2);
    cput("SCORE:");
    nump(score);
    eol();
    curmv(1, 50);
    cput("LIFE:");
    nump(lives);
    rst();
    return 0;
}

/* brdr() - Draw the checked board border. */
int brdr()
{
    int r;
    int c;

    for (c = 0; c < SCRW; c += 2)
    {
        bg(cbg(0, c / 2));
        curmv(2, c + 1);
        chout(' ');
        chout(' ');
        bg(cbg(SCRH - 1, c / 2));
        curmv(SCRH, c + 1);
        chout(' ');
        chout(' ');
    }

    for (r = 3; r < SCRH; r++)
    {
        bg(cbg(r - 2, 0));
        curmv(r, 1);
        chout(' ');
        chout(' ');
        bg(cbg(r - 2, (SCRW / 2) - 1));
        curmv(r, SCRW - 1);
        chout(' ');
        chout(' ');
    }
    rst();
    return 0;
}

/* brclr(r) - Select brick color for row. */
int brclr(r)
int r;
{
    if (r == 0)
        bg(105);
    else if (r == 1)
        bg(101);
    else if (r == 2)
        bg(43);
    else if (r == 3)
        bg(103);
    else if (r == 4)
        bg(102);
    else if (r == 5)
        bg(104);
    else if (r == 6)
        bg(45);
    else
        bg(106);
    return 0;
}

/* drbrk(r,c) - Draw one brick cell. */
int drbrk(r, c)
int r;
int c;
{
    int x;
    int y;
    int i;

    y = BRO + r;
    x = BCO + c * (BRW + BRG);
    curmv(y, x);
    if (br[r][c])
    {
        brclr(r);
        for (i = 0; i < BRW; i++)
            chout(' ');
        rst();
    }
    else
    {
        for (i = 0; i < BRW; i++)
            chout(' ');
    }
    return 0;
}

/* fixpt(r,c) - Restore the screen cell under a moved ball. */
int fixpt(r, c)
int r;
int c;
{
    int brc;
    int brr;
    int x1;

    brr = r - BRO;
    x1 = c - BCO;
    if (brr >= 0 && brr < BRS && x1 >= 0)
    {
        brc = x1 / (BRW + BRG);
        if (brc >= 0 && brc < BCS && (x1 % (BRW + BRG)) < BRW)
        {
            drbrk(brr, brc);
            return 0;
        }
    }

    if (r == PADR && c >= pad && c < pad + PADW)
    {
        drpad();
        return 0;
    }

    rst();
    curmv(r, c);
    chout(' ');
    return 0;
}

/* drall() - Draw the brick wall. */
int drall()
{
    int r;
    int c;

    for (r = 0; r < BRS; r++)
    {
        for (c = 0; c < BCS; c++)
            drbrk(r, c);
    }
    return 0;
}

/* erball() - Erase ball at row/column. */
int erball(r, c)
int r;
int c;
{
    fixpt(r, c);
    return 0;
}

/* drball() - Draw the ball. */
int drball()
{
    col(97);
    curmv(by, bx);
    chout('O');
    rst();
    return 0;
}

/* mvdraw(r,c) - Restore old ball and draw new ball. */
int mvdraw(r, c)
int r;
int c;
{
    fixpt(r, c);
    drball();
    return 0;
}

/* erpad() - Erase paddle. */
int erpad()
{
    int i;

    curmv(PADR, pad);
    for (i = 0; i < PADW; i++)
        chout(' ');
    return 0;
}

/* drpad() - Draw paddle. */
int drpad()
{
    int i;

    bg(45);
    curmv(PADR, pad);
    for (i = 0; i < PADW; i++)
        chout(' ');
    rst();
    return 0;
}

/* msg(s) - Draw centered game message. */
int msg(s)
char *s;
{
    int i;

    rst();
    curmv(23, MINC);
    for (i = MINC; i <= MAXC; i++)
        chout(' ');
    col(37);
    curmv(23, 20);
    cput(s);
    rst();
    return 0;
}

/* clrmsg() - Clear message row. */
int clrmsg()
{
    int i;

    rst();
    curmv(23, MINC);
    for (i = MINC; i <= MAXC; i++)
        chout(' ');
    return 0;
}

/* setup() - Reset bricks and play state. */
int setup()
{
    int r;
    int c;

    score = 0;
    lives = 4;
    left = 0;
    spd = 4;
    quit = 0;
    for (r = 0; r < BRS; r++)
    {
        for (c = 0; c < BCS; c++)
        {
            br[r][c] = 1;
            left++;
        }
    }
    return 0;
}

/* serve() - Put ball on paddle and wait for launch. */
int serve()
{
    pad = (SCRW - PADW) / 2;
    bx = pad + PADW / 2;
    by = PADR - 1;
    dx = 1;
    if (x_rand() & 1)
        dx = -1;
    dy = -1;
    tick = 0;
    state = WAIT;
    drpad();
    drball();
    msg("SPACE TO LAUNCH");
    return 0;
}

/* padhit() - Bounce from paddle if contacted. */
int padhit()
{
    int rel;

    if (dy <= 0)
        return 0;
    if (by != PADR - 1)
        return 0;
    if (bx < pad || bx >= pad + PADW)
        return 0;

    rel = bx - pad;
    dy = -1;
    if (rel < 2)
        dx = -1;
    else if (rel > PADW - 3)
        dx = 1;
    score += 1;
    drstat();
    return 1;
}

/* brhit() - Test and clear brick at ball position. */
int brhit(pr, pc)
int pr;
int pc;
{
    int r;
    int c;
    int x1;
    int y1;

    y1 = by - BRO;
    if (y1 < 0 || y1 >= BRS)
        return 0;
    x1 = bx - BCO;
    if (x1 < 0)
        return 0;
    c = x1 / (BRW + BRG);
    if (c < 0 || c >= BCS)
        return 0;
    if ((x1 % (BRW + BRG)) >= BRW)
        return 0;

    r = y1;
    if (!br[r][c])
        return 0;

    br[r][c] = 0;
    left--;
    score += (BRS - r) * 10;
    if (left < 12)
        spd = 2;
    else if (left < 45)
        spd = 3;
    drbrk(r, c);
    drstat();
    if (pr == by)
        dx = -dx;
    else if (pc == bx)
        dy = -dy;
    else
        dy = -dy;
    if (left == 0)
        state = DONE;
    return 1;
}

/* lost() - Handle a missed ball. */
int lost()
{
    lives--;
    drstat();
    if (lives <= 0)
    {
        state = LOSE;
        return 0;
    }
    erpad();
    serve();
    return 0;
}

/* mvball() - Advance ball one tick. */
int mvball()
{
    int ox;
    int oy;

    if (state != PLAY)
        return 0;

    ox = bx;
    oy = by;
    bx += dx;
    by += dy;

    if (bx <= MINC)
    {
        bx = MINC;
        dx = 1;
    }
    else if (bx >= MAXC)
    {
        bx = MAXC;
        dx = -1;
    }

    if (by <= MINR)
    {
        by = MINR;
        dy = 1;
    }

    padhit();
    if (by >= MAXR)
    {
        erball(oy, ox);
        lost();
        return 0;
    }

    brhit(oy, ox);
    mvdraw(oy, ox);
    return 0;
}

/* movpad(d) - Move paddle and attached ball. */
int movpad(d)
int d;
{
    int old;
    int neu;

    old = pad;
    neu = pad + d;
    if (neu < MINC)
        neu = MINC;
    if (neu > MAXC - PADW + 1)
        neu = MAXC - PADW + 1;
    if (neu == old)
        return 0;

    pad = old;
    erpad();
    pad = neu;
    drpad();

    if (state == WAIT)
    {
        erball(by, bx);
        bx = pad + PADW / 2;
        drball();
    }
    return 0;
}

/* input() - Process one pending key. */
int input()
{
    int k;

    k = keygt();
    if (!k)
        return 0;
    if (k == ESC || k == CTLC)
    {
        quit = 1;
        return 0;
    }
    if (k == KLT)
        movpad(-PADL);
    else if (k == KRT)
        movpad(PADL);
    else if (k == SPC && state == WAIT)
    {
        clrmsg();
        state = PLAY;
    }
    return 0;
}

/* frame() - Draw the first screen. */
int frame()
{
    clrs();
    hid();
    drstat();
    brdr();
    drall();
    return 0;
}

/* main() - Game entry point. */
int main()
{
    setup();
    frame();
    serve();

    x_tset(TID, TMS);
    while (!quit && state != LOSE && state != DONE)
    {
        input();
        if (x_texp(TID))
        {
            tick++;
            if (tick >= spd)
            {
                tick = 0;
                mvball();
            }
            x_tset(TID, TMS);
        }
    }

    clrmsg();
    if (state == DONE)
        msg("YOU CLEARED THE WALL");
    else if (state == LOSE)
        msg("GAME OVER");

    curmv(25, 20);
    col(37);
    cput("FINAL SCORE: ");
    nump(score);
    rst();
    curmv(SCRH, 1);
    shw();
    cput("\r\n");
    return 0;
}
