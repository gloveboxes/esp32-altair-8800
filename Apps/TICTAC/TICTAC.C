/*
 * 5x5 Tic-Tac-Toe for Altair 8800 - VT100/xterm.js compatible
 * BDS C 1.6 on CP/M
 *
 * 5x5 board, get 4 in a row to win
 * Player (X) vs Computer (O) with heuristic AI
 * Tetris-style colorful VT100 cabinet display
 *
 * Controls:
 *   Arrow keys: Move cursor
 *   SPACE:      Place piece
 *   Q/ESC:      Quit
 */

#include "dxterm.h"
#include "dxtimer.h"
#include "dxsys.h"

#define BSIZ 5
#define WLEN 4
#define BROW 5
#define BCOL 18
#define CWID 6
#define CHGT 3
#define NROW 8
#define NCOL 64

#define EMP 0
#define HUM 1
#define COMP 2

#define PLAY 0
#define DONE 1
#define QUIT 2

#define K_NON 0
#define K_UP 1
#define K_DN 2
#define K_LT 3
#define K_RT 4
#define K_PLC 5
#define K_QUI 6

int brd[BSIZ][BSIZ];
int crow, ccol;
int gstat;
int turn;
int hwin, cwin, drws;
int result;
int escst, escct;

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

/* pbg(p) - Block background color for a piece. */
int pbg(p)
int p;
{
    if (p == HUM) return 41;
    if (p == COMP) return 44;
    return 100;
}

/* cbg(r,c) - Checker color for cabinet border. */
int cbg(r, c)
int r;
int c;
{
    if (((r / 2) + (c / 2)) & 1)
        return 100;
    return 107;
}

/* drwcel(r,c,hi) - Draw one board cell. hi=highlight. */
int drwcel(r, c, hi)
int r;
int c;
int hi;
{
    int sr, sc, p, bg, row;

    p = brd[r][c];
    sr = BROW + 1 + r * (CHGT + 1);
    sc = BCOL + 2 + c * (CWID + 1);

    if (hi && p == EMP)
        bg = 43;
    else
        bg = pbg(p);

    for (row = 0; row < CHGT; row++)
    {
        x_curmv(sr + row, sc);
        x_setc(bg);

        if (hi && p != EMP && row == 0)
        {
            x_setc(97);
            pstr("[    ]");
        }
        else if (hi && p != EMP && row == 2)
        {
            x_setc(97);
            pstr("[    ]");
        }
        else if (row == 1 && p == HUM)
        {
            if (hi)
            {
                x_setc(97);
                pstr("[");
                pstr(" XX ");
                pstr("]");
            }
            else
            {
                x_setc(97);
                pstr("  XX  ");
            }
        }
        else if (row == 1 && p == COMP)
        {
            if (hi)
            {
                x_setc(97);
                pstr("[");
                pstr(" OO ");
                pstr("]");
            }
            else
            {
                x_setc(97);
                pstr("  OO  ");
            }
        }
        else if (row == 1 && hi)
        {
            x_setc(30);
            pstr("  ..  ");
        }
        else
        {
            pstr("      ");
        }
        x_rstc();
    }
    return 0;
}

/* drwgrd() - Draw grid separator lines. */
int drwgrd()
{
    int r, c, gr, gc;

    /* Horizontal grid lines */
    for (r = 1; r < BSIZ; r++)
    {
        gr = BROW + 1 + r * (CHGT + 1) - 1;
        x_curmv(gr, BCOL + 2);
        x_setc(90);
        for (c = 0; c < BSIZ * (CWID + 1) - 1; c++)
            x_cout('-');
        x_rstc();
    }

    /* Vertical grid lines */
    for (c = 1; c < BSIZ; c++)
    {
        gc = BCOL + 2 + c * (CWID + 1) - 1;
        for (r = 0; r < BSIZ; r++)
        {
            gr = BROW + 1 + r * (CHGT + 1);
            x_curmv(gr, gc);
            x_setc(90);
            x_cout('|');
            x_curmv(gr + 1, gc);
            x_cout('|');
            x_curmv(gr + 2, gc);
            x_cout('|');
            x_rstc();
        }
    }

    /* Cross points */
    for (r = 1; r < BSIZ; r++)
    {
        gr = BROW + 1 + r * (CHGT + 1) - 1;
        for (c = 1; c < BSIZ; c++)
        {
            gc = BCOL + 2 + c * (CWID + 1) - 1;
            x_curmv(gr, gc);
            x_setc(90);
            x_cout('+');
            x_rstc();
        }
    }
    return 0;
}

/* drwbrd() - Draw the entire board. */
int drwbrd()
{
    int r, c;

    drwgrd();
    for (r = 0; r < BSIZ; r++)
        for (c = 0; c < BSIZ; c++)
            drwcel(r, c, 0);
    return 0;
}

/* border() - Draw the checker cabinet border. */
int border()
{
    int r, c, bw, bh;

    bw = BSIZ * (CWID + 1) + 3;
    bh = BSIZ * (CHGT + 1) + 1;
    for (r = 0; r < bh; r++)
    {
        x_curmv(BROW + r, BCOL);
        x_setc(cbg(r, 0));
        x_cout(' ');
        x_cout(' ');
        x_rstc();

        x_curmv(BROW + r, BCOL + bw - 2);
        x_setc(cbg(r, bw - 1));
        x_cout(' ');
        x_cout(' ');
        x_rstc();
    }

    /* Top and bottom rows */
    for (c = 0; c < bw; c++)
    {
        x_curmv(BROW, BCOL + c);
        x_setc(cbg(0, c));
        x_cout(' ');
        x_rstc();

        x_curmv(BROW + bh - 1, BCOL + c);
        x_setc(cbg(bh - 1, c));
        x_cout(' ');
        x_rstc();
    }
    x_rstc();
    return 0;
}

/* title() - Draw heading and controls. */
int title()
{
    x_curmv(1, 1);
    x_setc(36);
    pstr("TICTAC");
    x_rstc();
    pstr(" for ");
    x_setc(33);
    pstr("Altair 8800");
    x_rstc();
    pstr(" V1.0");

    x_curmv(2, 1);
    x_setc(37);
    pstr("Arrows Move  SPACE Place  Q/ESC Quit");
    x_rstc();

    x_curmv(3, 1);
    x_setc(90);
    pstr("5x5 board - get 4 in a row to win!");
    x_rstc();
    return 0;
}

/* stats() - Draw score area. */
int stats()
{
    x_curmv(6, 1);
    x_setc(35);
    pstr("SCORE");
    x_rstc();

    x_curmv(8, 1);
    x_setc(36);
    pstr("You(X): ");
    x_setc(33);
    x_numpr(hwin);
    x_rstc();
    pstr("   ");

    x_curmv(10, 1);
    x_setc(36);
    pstr("CPU(O): ");
    x_setc(33);
    x_numpr(cwin);
    x_rstc();
    pstr("   ");

    x_curmv(12, 1);
    x_setc(36);
    pstr("Draws:  ");
    x_setc(33);
    x_numpr(drws);
    x_rstc();
    pstr("   ");
    return 0;
}

/* turndr() - Draw whose turn it is. */
int turndr()
{
    x_curmv(NROW, NCOL);
    x_setc(36);
    pstr("TURN");
    x_rstc();

    x_curmv(NROW + 2, NCOL);
    if (turn == HUM)
    {
        x_setc(41);
        x_setc(97);
        pstr("  XX  ");
    }
    else
    {
        x_setc(44);
        x_setc(97);
        pstr("  OO  ");
    }
    x_rstc();

    x_curmv(NROW + 4, NCOL);
    if (turn == HUM)
    {
        x_setc(31);
        pstr("Your turn");
        pstr("      ");
    }
    else
    {
        x_setc(34);
        pstr("CPU thinking");
        pstr("   ");
    }
    x_rstc();
    return 0;
}

/* evline(r,c,dr,dc) - Evaluate line of WLEN for AI. */
int evline(r, c, dr, dc)
int r;
int c;
int dr;
int dc;
{
    int i, pr, pc, h, m;

    h = 0;
    m = 0;
    for (i = 0; i < WLEN; i++)
    {
        pr = r + i * dr;
        pc = c + i * dc;
        if (pr < 0 || pr >= BSIZ)
            return 0;
        if (pc < 0 || pc >= BSIZ)
            return 0;
        if (brd[pr][pc] == HUM) h++;
        if (brd[pr][pc] == COMP) m++;
    }
    if (h > 0 && m > 0)
        return 0;
    if (m == 3) return 10000;
    if (h == 3) return 5000;
    if (m == 2) return 100;
    if (h == 2) return 50;
    if (m == 1) return 10;
    if (h == 1) return 5;
    return 1;
}

/* aiscor(r,c) - Score an empty cell for AI. */
int aiscor(r, c)
int r;
int c;
{
    int sc, off, s;

    sc = 0;

    /* Horizontal lines through (r,c) */
    for (off = 0; off < WLEN; off++)
    {
        s = evline(r, c - off, 0, 1);
        sc = sc + s;
    }

    /* Vertical lines */
    for (off = 0; off < WLEN; off++)
    {
        s = evline(r - off, c, 1, 0);
        sc = sc + s;
    }

    /* Diagonal \ lines */
    for (off = 0; off < WLEN; off++)
    {
        s = evline(r - off, c - off, 1, 1);
        sc = sc + s;
    }

    /* Diagonal / lines */
    for (off = 0; off < WLEN; off++)
    {
        s = evline(r - off, c + off, 1, -1);
        sc = sc + s;
    }

    /* Center and inner ring bonus */
    if (r == 2 && c == 2) sc = sc + 15;
    if (r >= 1 && r <= 3 && c >= 1 && c <= 3)
        sc = sc + 5;

    return sc;
}

/* aimove() - Computer selects and places a move. */
int aimove()
{
    int r, c, sc, best, br, bc;
    int ties, rn;

    best = -1;
    br = 0;
    bc = 0;
    ties = 0;

    for (r = 0; r < BSIZ; r++)
    {
        for (c = 0; c < BSIZ; c++)
        {
            if (brd[r][c] != EMP)
                continue;
            sc = aiscor(r, c);
            if (sc > best)
            {
                best = sc;
                br = r;
                bc = c;
                ties = 1;
            }
            else if (sc == best)
            {
                ties++;
                rn = x_rand() % ties;
                if (rn == 0)
                {
                    br = r;
                    bc = c;
                }
            }
        }
    }

    brd[br][bc] = COMP;
    drwcel(br, bc, 0);
    return 0;
}

/* chkwin(who) - Check if who has 4 in a row. */
int chkwin(who)
int who;
{
    int r, c, i, ok;

    /* Horizontal */
    for (r = 0; r < BSIZ; r++)
    {
        for (c = 0; c <= BSIZ - WLEN; c++)
        {
            ok = 1;
            for (i = 0; i < WLEN; i++)
            {
                if (brd[r][c + i] != who)
                    ok = 0;
            }
            if (ok) return 1;
        }
    }

    /* Vertical */
    for (c = 0; c < BSIZ; c++)
    {
        for (r = 0; r <= BSIZ - WLEN; r++)
        {
            ok = 1;
            for (i = 0; i < WLEN; i++)
            {
                if (brd[r + i][c] != who)
                    ok = 0;
            }
            if (ok) return 1;
        }
    }

    /* Diagonal \ */
    for (r = 0; r <= BSIZ - WLEN; r++)
    {
        for (c = 0; c <= BSIZ - WLEN; c++)
        {
            ok = 1;
            for (i = 0; i < WLEN; i++)
            {
                if (brd[r + i][c + i] != who)
                    ok = 0;
            }
            if (ok) return 1;
        }
    }

    /* Diagonal / */
    for (r = 0; r <= BSIZ - WLEN; r++)
    {
        for (c = WLEN - 1; c < BSIZ; c++)
        {
            ok = 1;
            for (i = 0; i < WLEN; i++)
            {
                if (brd[r + i][c - i] != who)
                    ok = 0;
            }
            if (ok) return 1;
        }
    }

    return 0;
}

/* chkful() - Check if board is full (draw). */
int chkful()
{
    int r, c;

    for (r = 0; r < BSIZ; r++)
        for (c = 0; c < BSIZ; c++)
            if (brd[r][c] == EMP)
                return 0;
    return 1;
}

/* keymap(ch) - Decode key with ANSI sequence support. */
int keymap(ch)
int ch;
{
    if (escst == 2)
    {
        escst = 0;
        if (ch == 'A') return K_UP;
        if (ch == 'B') return K_DN;
        if (ch == 'C') return K_RT;
        if (ch == 'D') return K_LT;
        return K_NON;
    }

    if (escst == 1)
    {
        escst = 0;
        if (ch == '[')
        {
            escst = 2;
            return K_NON;
        }
        return K_QUI;
    }

    if (ch == 27)
    {
        escst = 1;
        escct = 0;
        return K_NON;
    }

    if (ch == 'q' || ch == 'Q') return K_QUI;
    if (x_isup(ch)) return K_UP;
    if (x_isdn(ch)) return K_DN;
    if (x_islt(ch)) return K_LT;
    if (x_isrt(ch)) return K_RT;
    if (x_isspc(ch)) return K_PLC;
    return K_NON;
}

/* esctik() - Lone ESC becomes quit after grace period. */
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

/* dokey(k) - Handle one decoded key action. */
int dokey(k)
int k;
{
    if (k == K_QUI)
    {
        gstat = QUIT;
        return 0;
    }

    if (k == K_UP && crow > 0)
    {
        drwcel(crow, ccol, 0);
        crow--;
        drwcel(crow, ccol, 1);
    }
    else if (k == K_DN && crow < BSIZ - 1)
    {
        drwcel(crow, ccol, 0);
        crow++;
        drwcel(crow, ccol, 1);
    }
    else if (k == K_LT && ccol > 0)
    {
        drwcel(crow, ccol, 0);
        ccol--;
        drwcel(crow, ccol, 1);
    }
    else if (k == K_RT && ccol < BSIZ - 1)
    {
        drwcel(crow, ccol, 0);
        ccol++;
        drwcel(crow, ccol, 1);
    }
    else if (k == K_PLC && brd[crow][ccol] == EMP)
    {
        brd[crow][ccol] = HUM;
        drwcel(crow, ccol, 0);
        if (chkwin(HUM))
        {
            result = 1;
            gstat = DONE;
        }
        else if (chkful())
        {
            result = 3;
            gstat = DONE;
        }
        else
        {
            turn = COMP;
            turndr();
        }
    }
    return 0;
}

/* hmturn() - Run one tick of the human turn. */
int hmturn()
{
    int ch, k;

    x_tset(2, 25);
    while (x_tact(2) && gstat == PLAY)
    {
        ch = x_keyrd();
        if (ch == 0) continue;
        k = keymap(ch);
        if (k != K_NON)
            dokey(k);
    }
    esctik();
    return 0;
}

/* cpturn() - Run the CPU turn. */
int cpturn()
{
    x_delay(1, 400);
    aimove();

    if (chkwin(COMP))
    {
        result = 2;
        gstat = DONE;
    }
    else if (chkful())
    {
        result = 3;
        gstat = DONE;
    }
    else
    {
        turn = HUM;
        turndr();
        drwcel(crow, ccol, 1);
    }
    return 0;
}

/* overdr() - Draw game result message. */
int overdr()
{
    x_curmv(16, 1);
    if (result == 1)
    {
        x_setc(32);
        pstr("YOU WIN!    ");
        hwin++;
    }
    else if (result == 2)
    {
        x_setc(31);
        pstr("CPU WINS!   ");
        cwin++;
    }
    else
    {
        x_setc(33);
        pstr("DRAW!       ");
        drws++;
    }
    x_rstc();

    stats();

    x_curmv(18, 1);
    x_setc(37);
    pstr("Play again? (Y/N)");
    x_rstc();
    return 0;
}

/* replay() - Ask play again, return 1 to continue. */
int replay()
{
    int ch;

    overdr();
    while (1)
    {
        ch = x_keyrd();
        if (ch == 'y' || ch == 'Y')
            return 1;
        if (ch == 'n' || ch == 'N')
            return 0;
        if (ch == 27)
            return 0;
    }
}

/* ginit() - Reset board and state for new game. */
int ginit()
{
    int r, c;

    for (r = 0; r < BSIZ; r++)
        for (c = 0; c < BSIZ; c++)
            brd[r][c] = EMP;

    crow = 2;
    ccol = 2;
    gstat = PLAY;
    turn = HUM;
    result = 0;
    escst = 0;
    escct = 0;
    return 0;
}

/* main() - Program entry point. */
int main()
{
    int run;

    hwin = 0;
    cwin = 0;
    drws = 0;
    run = 1;

    while (run)
    {
        ginit();
        x_clrsc();
        x_hidcr();
        title();
        border();
        drwbrd();
        stats();
        turndr();
        drwcel(crow, ccol, 1);

        while (gstat == PLAY)
        {
            if (turn == HUM)
                hmturn();
            else
                cpturn();
        }

        if (gstat == QUIT)
            break;

        run = replay();
    }

    x_curmv(28, 1);
    x_shwcr();
    x_rstc();
    pstr("Thanks for playing!\r\n");
    return 0;
}
