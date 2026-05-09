/* ============================================================
 * LLM RULES FOR GENERATING BDS C CODE (Altair 8800 / CP/M)
 * ============================================================
 *
 * 1. Syntax:
 *    - Use K&R (BDS C) style: return_type name(args) on next line
 *    - No ANSI prototypes, no "void", no modern keywords
 *    - All function definitions and calls must follow BDS C rules
 *
 * 2. Symbols (VERY IMPORTANT):
 *    - All symbol names (functions, variables, labels, statics, globals)
 *      must be unique in their first 7 characters
 *    - Prefer short, descriptive names, e.g. "x_delay", "x_tset"
 *    - Avoid underscores beyond the leading "x_" unless necessary
 *    - Do not exceed 7 characters for clarity and linker safety
 *
 * 3. Types:
 *    - Use int or unsigned (16-bit) for parameters and locals
 *    - Use long.c for longs
 *    - Explicitly declare return type (no implicit int)
 *
 *
 * 6. Style:
 *    - Add a short comment block before each function
 *    - Keep indentation simple (max 4 spaces)
 *    - No C99/C89 features (stick to 1980-era BDS C)
 *
 * 7. The app runs on CP/M single tasking OS, only one app runs at a time
 * ============================================================
 */

/* Console and BDOS entry points */

#include "dxterm.h"

/* x_numpr(n) - Print signed integer in decimal form. */
int x_numpr(n)
int n;
{
    char buf[6];
    int i;

    if (n == 0)
    {
        putchar('0');
        return 0;
    }

    if (n < 0)
    {
        putchar('-');
        n = -n;
    }

    i = 0;
    while (n > 0 && i < 6)
    {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }

    while (i--)
        putchar(buf[i]);

    return 0;
}

/* x_curmv(row,col) - Move cursor to 1-based row/column. */
int x_curmv(row, col)
int row;
int col;
{
    printf("\033[%d;%dH", row, col);
    return 0;
}

/* x_clrsc() - Clear screen and reset attributes. */
int x_clrsc()
{
    printf("\033[2J\033[0m");
    x_curmv(1, 1);
    return 0;
}

/* x_hidcr() - Hide the terminal cursor. */
int x_hidcr()
{
    printf("\033[?25l");
    return 0;
}

/* x_shwcr() - Show the terminal cursor. */
int x_shwcr()
{
    printf("\033[?25h");
    return 0;
}

int x_conin() /* Read one char from console (blocking) */
{
    return (bdos(1) & 0xFF);
}

int x_cout(code) /* Write one char to console */
int code;
{
    return bdos(2, code);
}

/* x_keyrd() - Read raw key code without waiting. */
int x_keyrd()
{
    return (bdos(6, 0xFF) & 0xFF);
}

/* x_keyck() - Return non-zero if a key is waiting. */
int x_keyck()
{
    return (bdos(11) & 0xFF);
}

/* x_keygt() - Fetch next key if available, else return 0. */
int x_keygt()
{
    if (!x_keyck())
        return 0;
    return x_keyrd();
}

/* x_isesc(code) - Return non-zero if code is ESC. */
int x_isesc(code)
int code;
{
    return (code == XK_ESC);
}

/* x_iscc(code) - Return non-zero if code is Ctrl-C. */
int x_iscc(code)
int code;
{
    return (code == XK_CC);
}

/* x_isup(code) - Return non-zero if code is Up arrow. */
int x_isup(code)
int code;
{
    return (code == XK_UP);
}

/* x_isdn(code) - Return non-zero if code is Down arrow. */
int x_isdn(code)
int code;
{
    return (code == XK_DN);
}

/* x_islt(code) - Return non-zero if code is Left arrow. */
int x_islt(code)
int code;
{
    return (code == XK_LT);
}

/* x_isrt(code) - Return non-zero if code is Right arrow. */
int x_isrt(code)
int code;
{
    return (code == XK_RT);
}

/* x_isspc(code) - Return non-zero if code is Space. */
int x_isspc(code)
int code;
{
    return (code == XK_SPC);
}

/* x_setc(code) - Set foreground color using ANSI code. */
int x_setc(code)
int code;
{
    printf("\033[%dm", code);
    return 0;
}

/* x_rstc() - Reset all color and text attributes. */
int x_rstc()
{
    printf("\033[0m");
    return 0;
}

/* x_ereol() - Erase from cursor to end of line. */
int x_ereol()
{
    printf("\033[K");
    return 0;
}

/* x_csav() - Save cursor position. */
int x_csav()
{
    printf("\033[s");
    return 0;
}

/* x_crst() - Restore cursor position. */
int x_crst()
{
    printf("\033[u");
    return 0;
}

/* x_cup(n) - Move cursor up n rows. */
int x_cup(n)
int n;
{
    printf("\033[%dA", n);
    return 0;
}

/* x_cdn(n) - Move cursor down n rows. */
int x_cdn(n)
int n;
{
    printf("\033[%dB", n);
    return 0;
}

/* x_crt(n) - Move cursor right n columns. */
int x_crt(n)
int n;
{
    printf("\033[%dC", n);
    return 0;
}

/* x_clt(n) - Move cursor left n columns. */
int x_clt(n)
int n;
{
    printf("\033[%dD", n);
    return 0;
}
