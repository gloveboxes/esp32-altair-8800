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
int bdos();
int bios();

/* Terminal output functions */
int x_numpr(); /* Print signed integer in decimal */

/* Cursor and screen control */
int x_curmv(row, col); /* Move cursor to row,col */
int x_clrsc();         /* Clear screen and reset attributes */
int x_hidcr();         /* Hide cursor */
int x_shwcr();         /* Show cursor */
int x_ereol();         /* Erase from cursor to end of line */
int x_csav();          /* Save cursor position */
int x_crst();          /* Restore cursor position */
int x_cup(n);          /* Move cursor up n rows */
int x_cdn(n);          /* Move cursor down n rows */
int x_clt(n);          /* Move cursor left n cols */
int x_crt(n);          /* Move cursor right n cols */

/* Keyboard input functions */
int x_conin(); /* Read one char from console (blocking) */
int x_cout(code);   /* Write one char to console */
int x_keyrd(); /* Read key code (non-blocking, 0 if none) */
int x_keyck(); /* Check if key is waiting (non-zero=yes) */
int x_keygt(); /* Get waiting key or 0 if none */

/* Key code test functions */
int x_isesc(code);   /* Test if code is ESC */
int x_iscc(code);    /* Test if code is Ctrl-C */
int x_isup(code);    /* Test if code is Up arrow */
int x_isdn(code);    /* Test if code is Down arrow */
int x_islt(code);    /* Test if code is Left arrow */
int x_isrt(code);    /* Test if code is Right arrow */
int x_isspc(code);   /* Test if code is Space */

/* Color and attribute functions */
int x_setc();   /* Emit ESC[Nm for any SGR code (fg, bg, attr) */
int x_rstc();   /* Reset all attributes (ESC[0m) */

/* Terminal key codes for Altair environment */
#define XK_ESC 27
#define XK_CC 3
#define XK_UP 5
#define XK_DN 24
#define XK_LT 19
#define XK_RT 4
#define XK_SPC 32

/* Standard ANSI color codes for xterm.js
 *
 * Use x_setc(code) to emit any SGR code (foreground, background, or attribute).
 * Use x_rstc() to reset all attributes.
 *
 * Normal palette (TFT VT100 renderer):
 *   0: black     fg 30  bg 40    8: bright black  fg 90  bg 100
 *   1: red       fg 31  bg 41    9: bright red    fg 91  bg 101
 *   2: green     fg 32  bg 42   10: bright green  fg 92  bg 102
 *   3: yellow    fg 33  bg 43   11: bright yellow fg 93  bg 103
 *   4: blue      fg 34  bg 44   12: bright blue   fg 94  bg 104
 *   5: magenta   fg 35  bg 45   13: bright magenta fg 95 bg 105
 *   6: cyan      fg 36  bg 46   14: bright cyan   fg 96  bg 106
 *   7: white     fg 37  bg 47   15: bright white  fg 97  bg 107
 *
 * Useful attributes:
 *   ESC[0m  - Reset all (fg 7, bg 0)
 *   ESC[1m  - Bold/bright foreground
 *   ESC[7m  - Reverse video
 *   ESC[22m - Normal intensity
 *   ESC[39m - Reset foreground only
 *   ESC[49m - Reset background only
 *   Combine: ESC[1;33m = bright yellow, ESC[37;44m = white on blue
 *
 * xterm indexed color on TFT:
 *   Foreground: ESC[38;5;Nm   Background: ESC[48;5;Nm
 *   Only N=0..15 is honored by the TFT VT100 renderer. Prefer the explicit
 *   16-color SGR codes above for consistent rendering across TFT and xterm.js.
 *
 * For colored blocks: print spaces with background color (e.g. bg 41-47,
 * 100-107), two spaces per cell for squarer pixels. Always reset before
 * normal text and before exiting.
 */
#define XC_BLK 30  /* Black */
#define XC_RED 31  /* Red */
#define XC_GRN 32  /* Green */
#define XC_YEL 33  /* Yellow */
#define XC_BLU 34  /* Blue */
#define XC_MAG 35  /* Magenta */
#define XC_CYN 36  /* Cyan */
#define XC_WHT 37  /* White */
#define XC_BYEL 93 /* Bright Yellow */
#define XC_RST 0   /* Reset all attributes */
