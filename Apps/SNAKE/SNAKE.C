/*
 * Snake Game for Altair 8800 - VT100/xterm.js compatible
 * BDS C 1.6 on CP/M
 *
 * Classic Snake game using arrow keys for movement.
 * Collect food (*) to grow longer and increase score.
 * Don't hit walls or yourself!
 *
 * Controls:
 * - Arrow keys: Change direction
 * - ESC: Quit game
 *
 * Based on breakout.c pattern for BDS C compatibility
 */

#include "dxterm.h"
#include "dxsys.h"



/* Timer configuration */
#define T2H 28 /* Timer 2 high byte port */
#define T2L 29 /* Timer 2 low byte port */

/* Board layout */
#define BWID 30
#define BHGT 20
#define BROW 4
#define BCOL 17

/* Snake Settings */
#define MAXSNK 200
#define INILEN 3

/* Game States */
#define PLAY 0
#define OVER 8
#define QUIT 9

/* Directions */
#define D_UP 1
#define D_DN 2
#define D_LT 3
#define D_RT 4

/* Snake body coordinates */
int sn_row[MAXSNK];
int sn_col[MAXSNK];
int sn_len;
int sn_dir;
int nxt_dir;

/* Food position */
int fd_row, fd_col;
int fd_ex;

/* Game state */
int gm_st;
int score;
int sp_lvl;
int escst;
int escct;

/* Simple counter-based timing */
int mv_cnt;

int inp();
int outp();


/* --- Game Display --- */

int cset(c)
int c;
{
    printf("\033[%dm", c);
    return 0;
}

int setbg(c)
int c;
{
    printf("\033[%dm", c);
    return 0;
}

int rst()
{
    printf("\033[0m");
    return 0;
}

int tset(ms)
unsigned ms;
{
    char hi, lo;

    hi = ms >> 8;
    outp(T2H, hi);
    lo = ms & 0xFF;
    outp(T2L, lo);
    return 0;
}

int texp()
{
    return (inp(T2L) == 0);
}

int scr_r(r)
int r;
{
    return BROW + r + 1;
}

int scr_c(c)
int c;
{
    return BCOL + c * 2;
}

int cbg(r, c)
int r;
int c;
{
    if (((r / 2) + (c / 2)) & 1)
        return 46;
    return 45;
}

int tile(bg)
int bg;
{
    if (bg)
        setbg(bg);
    else
        rst();
    putchar(' ');
    putchar(' ');
    return 0;
}

int dr_wal()
{
    int r;
    int c;
    int left;

    left = BCOL - 2;
    for (r = 0; r < BHGT + 2; r++)
    {
        x_curmv(BROW + r, left);
        for (c = 0; c < BWID + 2; c++)
        {
            setbg(cbg(r, c));
            putchar(' ');
            putchar(' ');
        }
    }
    rst();
    return 0;
}

int dr_fld()
{
    int r;
    int c;

    for (r = 0; r < BHGT; r++)
    {
        x_curmv(scr_r(r), scr_c(0));
        for (c = 0; c < BWID; c++)
            tile(40);
    }
    rst();
    return 0;
}

int dr_ins()
{
    x_curmv(1, 1);
    cset(36);
    puts("SNAKE");
    rst();
    puts(" for ");
    cset(33);
    puts("Altair 8800");
    rst();
    puts(" V2.0");

    x_curmv(2, 1);
    cset(37);
    puts("ARROWS Move  ESC/Q Quit  Eat red blocks, avoid the cabinet and yourself");
    rst();
    return 0;
}

int dr_seg(r, c, is_head)
int r;
int c;
int is_head;
{
    x_curmv(scr_r(r), scr_c(c));
    if (is_head)
        tile(46);
    else
        tile(42);
    return 0;
}

int er_pos(r, c)
int r;
int c;
{
    x_curmv(scr_r(r), scr_c(c));
    tile(40);
    return 0;
}

int dr_fod(r, c)
int r;
int c;
{
    x_curmv(scr_r(r), scr_c(c));
    tile(41);
    return 0;
}

int upd_st()
{
    x_curmv(5, 1);
    cset(35);
    puts("STATUS");
    rst();

    x_curmv(6, 1);
    cset(36);
    puts("Score: ");
    cset(33);
    x_numpr(score);
    rst();
    puts("  ");

    x_curmv(7, 1);
    cset(36);
    puts("Length: ");
    cset(33);
    x_numpr(sn_len);
    rst();
    puts("  ");

    x_curmv(8, 1);
    cset(36);
    puts("Speed: ");
    cset(33);
    x_numpr(sp_lvl);
    rst();
    puts("  ");
    return 0;
}

/* --- Random Number Generation --- */

unsigned rnd()
{
    return x_rand();
}

/* --- Food Management --- */

int is_occ(row, col)
int row;
int col;
{
    int i;

    /* Check if position conflicts with snake body */
    for (i = 0; i < sn_len; i++)
    {
        if (sn_row[i] == row && sn_col[i] == col)
        {
            return 1;
        }
    }
    return 0;
}

int hit_sn(row, col, lim)
int row;
int col;
int lim;
{
    int i;

    for (i = 0; i < lim; i++)
    {
        if (sn_row[i] == row && sn_col[i] == col)
            return 1;
    }
    return 0;
}

int pl_fod()
{
    int att;
    int fdr, fdc;
    int r, c;

    att = 0;
    while (att < 200)
    { /* Try random cells first */
        fdr = rnd() % BHGT;
        fdc = rnd() % BWID;

        if (!is_occ(fdr, fdc))
        {
            fd_row = fdr;
            fd_col = fdc;
            fd_ex = 1;
            dr_fod(fd_row, fd_col);
            return 1;
        }
        att++;
    }

    for (r = 0; r < BHGT; r++)
    {
        for (c = 0; c < BWID; c++)
        {
            if (!is_occ(r, c))
            {
                fd_row = r;
                fd_col = c;
                fd_ex = 1;
                dr_fod(fd_row, fd_col);
                return 1;
            }
        }
    }

    return 0; /* Failed to place food */
}

/* --- Input Handling --- */

int opp(a, b)
int a;
int b;
{
    if (a == D_UP && b == D_DN) return 1;
    if (a == D_DN && b == D_UP) return 1;
    if (a == D_LT && b == D_RT) return 1;
    if (a == D_RT && b == D_LT) return 1;
    return 0;
}

int keymap(k)
int k;
{
    if (escst == 2)
    {
        escst = 0;
        if (k == 'A') return D_UP;
        if (k == 'B') return D_DN;
        if (k == 'C') return D_RT;
        if (k == 'D') return D_LT;
        return 0;
    }

    if (escst == 1)
    {
        escst = 0;
        if (k == '[')
        {
            escst = 2;
            return 0;
        }
        return QUIT;
    }

    if (k == 27)
    {
        escst = 1;
        escct = 0;
        return 0;
    }

    if (k == 'q' || k == 'Q')
        return QUIT;
    if (x_isup(k)) return D_UP;
    if (x_isdn(k)) return D_DN;
    if (x_islt(k)) return D_LT;
    if (x_isrt(k)) return D_RT;
    return 0;
}

int inp_hnd()
{
    int key;
    int cmd;

    key = x_keyrd(); /* Read raw key code no waiting */
    if (!key)
        return 0;

    cmd = keymap(key);

    if (cmd == QUIT)
    {
        gm_st = QUIT;
        return 1;
    }

    if (cmd >= D_UP && cmd <= D_RT && !opp(cmd, sn_dir) && !opp(cmd, nxt_dir))
        nxt_dir = cmd;

    return 0;
}

int esctik()
{
    if (escst == 1)
    {
        escct++;
        if (escct > 2)
        {
            escst = 0;
            gm_st = QUIT;
        }
    }
    return 0;
}

/* --- Game Logic --- */

int new_gm()
{
    int i;

    gm_st  = PLAY;
    score  = 0;
    sp_lvl = 1;
    fd_ex  = 0;
    sn_len  = INILEN;
    sn_dir  = D_RT;
    nxt_dir = D_RT;
    escst   = 0;
    escct   = 0;
    mv_cnt  = 0;

    /* Place snake in center, moving right */
    for (i = 0; i < sn_len; i++)
    {
        sn_row[i] = BHGT / 2;
        sn_col[i] = (BWID / 2) - i;
    }

    /* Draw initial snake */
    for (i = 0; i < sn_len; i++)
    {
        dr_seg(sn_row[i], sn_col[i], (i == 0));
    }

    return 0;
}

int step()
{
    int nr, nc;
    int i;
    int ate;
    int lim;
    int grow;
    int nlen;
    int tailr;
    int tailc;
    int oldr;
    int oldc;

    sn_dir = nxt_dir;

    nr = sn_row[0];
    nc = sn_col[0];

    if (sn_dir == D_UP) nr--;
    if (sn_dir == D_DN) nr++;
    if (sn_dir == D_LT) nc--;
    if (sn_dir == D_RT) nc++;

    if (nr < 0 || nr >= BHGT || nc < 0 || nc >= BWID)
    {
        gm_st = OVER;
        return 0;
    }

    ate = 0;
    if (fd_ex && nr == fd_row && nc == fd_col)
    {
        ate = 1;
        fd_ex = 0;
    }

    /* Moving into the old tail is legal unless eating. */
    lim = sn_len;
    if (!ate)
        lim = sn_len - 1;
    if (hit_sn(nr, nc, lim))
    {
        gm_st = OVER;
        return 0;
    }

    grow = 0;
    if (ate)
    {
        score += 10;

        if (score % 50 == 0 && sp_lvl < 10)
            sp_lvl++;

        if (sn_len < MAXSNK)
            grow = 1;
    }

    oldr = sn_row[0];
    oldc = sn_col[0];
    tailr = sn_row[sn_len - 1];
    tailc = sn_col[sn_len - 1];

    nlen = sn_len;
    if (grow)
        nlen = sn_len + 1;

    for (i = nlen - 1; i > 0; i--)
    {
        sn_row[i] = sn_row[i - 1];
        sn_col[i] = sn_col[i - 1];
    }

    sn_len = nlen;
    sn_row[0] = nr;
    sn_col[0] = nc;

    if (!grow)
        er_pos(tailr, tailc);
    dr_seg(oldr, oldc, 0);
    dr_seg(nr, nc, 1);

    if (ate && !fd_ex)
    {
        if (!pl_fod())
            gm_st = OVER;
    }

    return 1;
}



/* --- Game Over Display --- */

int sh_ovr()
{
    x_curmv(15, 34);
    cset(35);
    puts("GAME OVER!");
    rst();
    x_curmv(16, 31);
    cset(33);
    puts("Final Score: ");
    rst();
    x_numpr(score);
    x_curmv(17, 31);
    cset(33);
    puts("Final Length: ");
    rst();
    x_numpr(sn_len);
    x_curmv(18, 31);
    puts("Press ESC to quit");
    return 0;
}

/* --- Main Program --- */

int main()
{
    int key;
    int mv_del;

    /* Set up display */
    x_clrsc();
    x_hidcr();
    dr_ins();
    dr_wal();
    dr_fld();

    /* Initialize game */
    new_gm();

    /* Place first food */
    pl_fod();

    /* Initial status display */
    upd_st();

    mv_cnt = 0;
    tset(20); /* Set timer for main loop - 20ms like game.c */

    /* Main game loop */
    while (gm_st == PLAY)
    {
        /* Handle input every cycle */
        inp_hnd();
        
        if (gm_st != PLAY)
            break;

        /* Use counter-based movement timing */
        if (texp())
        {
            mv_cnt++;
            esctik();
            if (gm_st != PLAY)
                break;
            
            /* Move snake every N cycles based on speed level */
            {
                mv_del = 10 - sp_lvl; /* Starts at 10, gets faster */
                if (mv_del < 4) mv_del = 4; /* Minimum delay */
                
                if (mv_cnt >= mv_del)
                {
                    step();
                    mv_cnt = 0;

                    /* Update status display */
                    upd_st();
                }
            }

            tset(20); /* Reset 20ms timer like game.c */
        }
    }

    /* Game over or quit */
    if (gm_st == OVER)
    {
        sh_ovr();

        /* Wait for quit key */
        while (1)
        {
            key = x_keyrd(); /* Read raw key code no waiting */
            if (key && x_isesc(key))
                break;
            if (key == 'q' || key == 'Q')
                break;
        }
    }

    /* Cleanup */
    x_curmv(26, 1);
    x_shwcr();
    puts("Thanks for playing Snake!\r\n");

    return 0;
}
