/* ============================================================
 * ENV - Environment Variable Manager for CP/M
 * ============================================================
 * Command parsing and storage are handled by the ESP32 emulator
 * through DXENV.C and port_drivers/environment_io.c.
 *
 * With arguments:  forwards the command line to the ESP32 via
 *                  e_exec() (subject to CP/M 2.2 upper-casing).
 *
 * Without arguments:  enters interactive mode. The variables
 *                  are loaded from the ESP32, the user picks one
 *                  by number and edits the value at a prompt
 *                  that reads characters via BDOS function 1 so
 *                  mixed case is preserved.
 * ============================================================
 */

#include "stdio.h"

#define MAXVARS  32
#define E_KEYSZ  17
#define E_VALSZ  256
#define LBUFSZ   144
#define PGSIZE   15

int e_init();
int e_exec();
int e_list();
int e_set();
int e_del();
int bdos();
int outp();
int putchar();

/* In-memory snapshot of the ESP32 environment. */
char keys[MAXVARS][E_KEYSZ];
char vals[MAXVARS][E_VALSZ];
int nvars;
int curpage;

/* e_list callback. Copies key/value into our arrays. */
int collect(k, v)
char *k;
char *v;
{
    int i;

    if (nvars >= MAXVARS) {
        return 0;
    }

    i = 0;
    while (k[i] && i < E_KEYSZ - 1) {
        keys[nvars][i] = k[i];
        i++;
    }
    keys[nvars][i] = 0;

    i = 0;
    while (v[i] && i < E_VALSZ - 1) {
        vals[nvars][i] = v[i];
        i++;
    }
    vals[nvars][i] = 0;

    nvars++;
    return 0;
}

/* Refresh the in-memory snapshot from the ESP32. */
int loadall()
{
    nvars = 0;
    e_list(collect);
    return nvars;
}

/* Total number of pages (at least 1, even when empty). */
int numpgs()
{
    int n;

    n = (nvars + PGSIZE - 1) / PGSIZE;
    if (n < 1) {
        n = 1;
    }
    return n;
}

/* Clamp curpage into the valid range. */
int fixpage()
{
    int last;

    last = numpgs() - 1;
    if (curpage < 0) {
        curpage = 0;
    }
    if (curpage > last) {
        curpage = last;
    }
    return curpage;
}

/* Print one page of variables. */
int showvar()
{
    int i;
    int j;
    int start;
    int stop;
    int pages;

    printf("\r\n");
    if (nvars == 0) {
        printf("(no environment variables set)\r\n");
        return 0;
    }

    fixpage();
    pages = numpgs();
    start = curpage * PGSIZE;
    stop = start + PGSIZE;
    if (stop > nvars) {
        stop = nvars;
    }

    printf("-- Page %d/%d  (entries %d-%d of %d) --\r\n",
           curpage + 1, pages, start + 1, stop, nvars);

    for (i = start; i < stop; i++) {
        printf("%3d. %s", i + 1, keys[i]);
        j = 0;
        while (keys[i][j]) {
            j++;
        }
        while (j < 16) {
            putchar(' ');
            j++;
        }
        printf(" = %s\r\n", vals[i]);
    }
    return 0;
}

/* Read a line via BDOS function 1 so case is preserved.
   Handles backspace and Ctrl-C. Returns length, or -1 on Ctrl-C. */
int readln(buf, max)
char *buf;
int max;
{
    int i;
    int c;

    i = 0;
    for (;;) {
        c = bdos(1, 0) & 0x7f;

        if (c == '\r' || c == '\n') {
            putchar('\n');
            buf[i] = 0;
            return i;
        }
        if (c == 8 || c == 127) {
            if (i > 0) {
                i--;
                putchar(' ');
                putchar(8);
            }
            continue;
        }
        if (c == 3) {
            putchar('\n');
            buf[0] = 0;
            return -1;
        }
        if (c == 21) {
            while (i > 0) {
                putchar(8);
                putchar(' ');
                putchar(8);
                i--;
            }
            continue;
        }
        if (c >= 32 && c < 127 && i < max - 1) {
            buf[i] = c;
            i++;
        }
    }
}

/* Parse a small positive integer. Returns -1 on error. */
int parsen(s)
char *s;
{
    int n;
    int any;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    n = 0;
    any = 0;
    while (*s >= '0' && *s <= '9') {
        n = (n * 10) + (*s - '0');
        s++;
        any = 1;
    }
    if (!any) {
        return -1;
    }
    return n;
}

/* Prompt for a new value and write it through e_set(). */
int editvar(idx)
int idx;
{
    char buf[E_VALSZ];
    int n;
    int rc;

    printf("\r\nKey   : %s\r\n", keys[idx]);
    printf("Value : %s\r\n", vals[idx]);
    printf("New   : ");

    n = readln(buf, E_VALSZ);
    if (n < 0) {
        printf("Cancelled.\r\n");
        return 0;
    }
    if (n == 0) {
        printf("(unchanged)\r\n");
        return 0;
    }

    rc = e_set(keys[idx], buf);
    if (rc != 0) {
        printf("Error: e_set failed (rc=%d)\r\n", rc);
        return 1;
    }

    n = 0;
    while (buf[n] && n < E_VALSZ - 1) {
        vals[idx][n] = buf[n];
        n++;
    }
    vals[idx][n] = 0;

    printf("Saved.\r\n");
    return 0;
}

/* Prompt for a new KEY and VALUE and create the entry. */
int addvar()
{
    char key[E_KEYSZ];
    char val[E_VALSZ];
    int n;
    int rc;

    printf("\r\nKey   : ");
    n = readln(key, E_KEYSZ);
    if (n <= 0) {
        printf("Cancelled.\r\n");
        return 0;
    }
    printf("Value : ");
    n = readln(val, E_VALSZ);
    if (n < 0) {
        printf("Cancelled.\r\n");
        return 0;
    }

    rc = e_set(key, val);
    if (rc != 0) {
        printf("Error: e_set failed (rc=%d)\r\n", rc);
        return 1;
    }
    printf("Saved.\r\n");
    loadall();
    return 0;
}

/* Delete the variable at the given 1-based index. */
int delvar(n)
int n;
{
    int rc;

    if (n < 1 || n > nvars) {
        printf("Invalid number.\r\n");
        return 1;
    }
    rc = e_del(keys[n - 1]);
    if (rc != 0) {
        printf("Error: e_del failed (rc=%d)\r\n", rc);
        return 1;
    }
    printf("Deleted %s.\r\n", keys[n - 1]);
    loadall();
    return 0;
}

/* Ask the user and, on Y, send magic byte 0xA5 to port 49 to reboot
   the ESP32. The 0xA5 guard prevents stray OUT 49 from rebooting. */
int doboot()
{
    char buf[8];
    int n;

    printf("\r\nReboot ESP32? (y/N) ");
    n = readln(buf, 8);
    if (n < 0) {
        printf("Cancelled.\r\n");
        return 0;
    }
    if (buf[0] != 'y' && buf[0] != 'Y') {
        printf("Cancelled.\r\n");
        return 0;
    }
    printf("Rebooting ESP32...\r\n");
    outp(49, 0xA5);
    return 0;
}

/* Interactive editor entry point. */
int imode()
{
    char line[LBUFSZ];
    int n;
    int num;
    char cmd;

    printf("ENV Interactive Mode\r\n");
    printf("====================\r\n");

    loadall();
    curpage = 0;

    for (;;) {
        showvar();
        printf("\r\n[number]=edit  A=add  D <n>=delete  N=next  P=prev  L=reload  R=reboot  Q=quit\r\n> ");

        n = readln(line, LBUFSZ);
        if (n < 0) {
            printf("Bye.\r\n");
            return 0;
        }
        if (n == 0) {
            continue;
        }

        cmd = line[0];
        if (cmd >= 'a' && cmd <= 'z') {
            cmd = cmd - 'a' + 'A';
        }

        if (cmd == 'Q') {
            printf("Bye.\r\n");
            return 0;
        }
        if (cmd == 'L') {
            loadall();
            fixpage();
            continue;
        }
        if (cmd == 'R') {
            doboot();
            continue;
        }
        if (cmd == 'N') {
            if (curpage + 1 < numpgs()) {
                curpage++;
            } else {
                printf("(already on last page)\r\n");
            }
            continue;
        }
        if (cmd == 'P') {
            if (curpage > 0) {
                curpage--;
            } else {
                printf("(already on first page)\r\n");
            }
            continue;
        }
        if (cmd == 'A') {
            addvar();
            continue;
        }
        if (cmd == 'D') {
            num = parsen(line + 1);
            if (num < 0) {
                printf("Usage: D <number>\r\n");
                continue;
            }
            delvar(num);
            continue;
        }

        num = parsen(line);
        if (num < 1 || num > nvars) {
            printf("Invalid number.\r\n");
            continue;
        }
        editvar(num - 1);
    }
}

main(argc, argv)
int argc;
char *argv[];
{
    int rc;

    rc = e_init();
    if (rc != 0) {
        printf("Error: Cannot init ENV storage\r\n");
        return 1;
    }

    if (argc <= 1) {
        return imode();
    }

    rc = e_exec(argc, argv);
    return rc == 0 ? 0 : 1;
}
