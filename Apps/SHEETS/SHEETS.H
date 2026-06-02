/*
 * SHEETS.H - shared defines, externals, and global state for the
 * SHEETS spreadsheet. The program is split across SHEETS.C (UI and
 * main) and SHEETC.C (formula engine) so each translation unit fits
 * the BDS C 1.6 compiler.
 *
 * MUST be included AFTER "stdio.h" (and "string.h") in every .c file
 * of the program. BDS C externals overlay by offset (FORTRAN COMMON
 * style), so the include order has to be identical in both modules.
 * No data declarations may precede stdio.h, so this header is never
 * the first include.
 */

#define MAXCOL 26
#define MAXROW 99
#define CWID   10
#define VCOLS  7
#define VROWS  26

/* Max formula nesting / eval-stack depth (also the cycle-check
 * window). evrow[]/evcol[] below are sized to this. */
#define EDMAX  25

#define TITR   1
#define HDRR   2
#define DATR   3
#define EDR    29
#define STR    29
#define HLPR   30

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
#define CTLC   3
#define CTLG   7
#define CTLK   11
#define CTLN   14
#define CTLO   15
#define CTLP   16
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
int inp();
int outp();

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

/* ---- Grid state (shared across SHEETS.C and SHEETC.C) ---- */

char *cells[MAXROW][MAXCOL];
int crow, ccol;
int trow, tcol;
char ebuf[80];
char ename[16];
char mesg[64];
char clip[80];
int hasclip;
int cliprow;
int clipcol;
int dirty;
int rall;

/* Eval globals */
char *epos;
int eok;
int edepth;

/* Cell-evaluation stack for circular-reference detection: evrow[k]/
 * evcol[k] hold the cell being evaluated at nesting level k (k <
 * edepth). ecirc is set when a cell already on the stack is re-
 * entered, so rndcel can render #CIRC instead of a plain #ERR. */
int evrow[EDMAX];
int evcol[EDMAX];
int ecirc;

/* Constant long buffers used by the evaluator; initialized in main(). */
char lzro[4];
char lten[4];
