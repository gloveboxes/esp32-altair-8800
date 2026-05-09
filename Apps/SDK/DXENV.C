/* ============================================================
 * LLM RULES FOR GENERATING BDS C CODE (Altair 8800 / CP/M)
 * ============================================================
 *
 * 1. Syntax:
 *    - Use K&R (BDS C) style: return_type name(args) on next line
 *    - No ANSI prototypes, no "void", no modern keywords
 *    - All function definitions and calls must follow BDS C rules
 *.   - No cast operators
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

/* ============================================================
 * DXENV - Environment Variable Library for BDS C / CP/M
 * ============================================================
 * Provides key-value storage in A:ALTAIR.ENV
 * 
 * Record format (128 bytes = 1 CP/M sector, no padding):
 *   [0]       = status (0x00=empty, 0x01=active, 0xFF=deleted)
 *   [1-16]    = key (15 chars + null)
 *   [17-127]  = value (110 chars + null)
 *
 * Optimized for ~10 key-value pairs
 * ============================================================
 */

#include "stdio.h"

/* Constants */
#define E_FNAME "A:ALTAIR.ENV"
#define E_MAXR   16       /* Max records supported */
#define E_KEYSZ  16       /* Key size (15 chars + null) */
#define E_VALSZ  111      /* Value size (110 chars + null) */
#define E_RECSZ  128      /* Record size = 1 sector */

/* Status flags */
#define E_EMPTY  0x00
#define E_ACTV   0x01
#define E_DEL    0xFF

/* Return codes */
#define E_OK     0
#define E_EOPEN  -1
#define E_EREAD  -2
#define E_EWRIT  -3
#define E_EFULL  -4
#define E_ENOTF  -5

/* Record buffer */
char e_buf[E_RECSZ];

/* ------------------------------------------------------------
 * e_toupr - Convert char to uppercase
 * ------------------------------------------------------------ */
int e_toupr(c)
int c;
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    return c;
}

/* ------------------------------------------------------------
 * e_cmpk - Compare two keys (case-insensitive)
 * Returns 0 if equal, non-zero otherwise
 * ------------------------------------------------------------ */
int e_cmpk(k1, k2)
char *k1;
char *k2;
{
    int i;
    for (i = 0; i < E_KEYSZ - 1; i++) {
        if (e_toupr(k1[i]) != e_toupr(k2[i]))
            return 1;
        if (k1[i] == 0)
            return 0;
    }
    return 0;
}

/* ------------------------------------------------------------
 * e_cpys - Copy string with length limit
 * ------------------------------------------------------------ */
int e_cpys(dst, src, maxlen)
char *dst;
char *src;
int maxlen;
{
    int i;
    for (i = 0; i < maxlen - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
    return i;
}

/* ------------------------------------------------------------
 * e_clrb - Clear record buffer
 * ------------------------------------------------------------ */
int e_clrb()
{
    int i;
    for (i = 0; i < E_RECSZ; i++)
        e_buf[i] = 0;
    return 0;
}

/* ------------------------------------------------------------
 * e_init - Initialize env file (create if not exists)
 * Returns E_OK on success, error code on failure
 * ------------------------------------------------------------ */
int e_init()
{
    int fd;
    
    /* Try to open existing file */
    fd = open(E_FNAME, 0);
    if (fd != ERROR) {
        close(fd);
        return E_OK;
    }
    
    /* Create new file */
    fd = creat(E_FNAME);
    if (fd == ERROR)
        return E_EOPEN;
    
    close(fd);
    return E_OK;
}

/* ------------------------------------------------------------
 * e_find - Find record by key, return slot number
 * Returns slot (0-15) if found, -1 if not found
 * Sets e_buf with record data if found
 * ------------------------------------------------------------ */
int e_find(key)
char *key;
{
    int fd, slot;
    char tkey[E_KEYSZ];
    
    fd = open(E_FNAME, 0);
    if (fd == ERROR)
        return -1;
    
    for (slot = 0; slot < E_MAXR; slot++) {
        if (seek(fd, slot, 0) == ERROR)
            break;
        if (read(fd, e_buf, 1) != 1)
            break;
        
        /* Check if active record */
        if (e_buf[0] != E_ACTV)
            continue;
        
        /* Extract and compare key */
        e_cpys(tkey, &e_buf[1], E_KEYSZ);
        if (e_cmpk(tkey, key) == 0) {
            close(fd);
            return slot;
        }
    }
    
    close(fd);
    return -1;
}

/* ------------------------------------------------------------
 * e_slots - Find first empty or deleted slot
 * Returns slot number or -1 if full
 * ------------------------------------------------------------ */
int e_slots()
{
    int fd, slot;
    
    fd = open(E_FNAME, 0);
    if (fd == ERROR)
        return -1;
    
    for (slot = 0; slot < E_MAXR; slot++) {
        if (seek(fd, slot, 0) == ERROR) {
            close(fd);
            return slot;  /* Past EOF, use this slot */
        }
        if (read(fd, e_buf, 1) != 1) {
            close(fd);
            return slot;  /* Read failed, use this slot */
        }
        
        if (e_buf[0] == E_EMPTY || e_buf[0] == E_DEL) {
            close(fd);
            return slot;
        }
    }
    
    close(fd);
    return -1;  /* Full */
}

/* ------------------------------------------------------------
 * e_get - Read environment variable value
 * Returns E_OK and copies value to val, or E_ENOTF
 * ------------------------------------------------------------ */
int e_get(key, val)
char *key;
char *val;
{
    int slot;
    
    slot = e_find(key);
    if (slot < 0) {
        val[0] = 0;
        return E_ENOTF;
    }
    
    /* e_buf already loaded by e_find */
    e_cpys(val, &e_buf[17], E_VALSZ);
    return E_OK;
}

/* ------------------------------------------------------------
 * e_set - Set environment variable (create or update)
 * Returns E_OK on success, error code on failure
 * ------------------------------------------------------------ */
int e_set(key, val)
char *key;
char *val;
{
    int fd, slot;
    char lkey[E_KEYSZ];
    char lval[E_VALSZ];
    
    /* Copy key/val to local buffers first (avoid global overlap) */
    e_cpys(lkey, key, E_KEYSZ);
    e_cpys(lval, val, E_VALSZ);
    
    /* Check if key exists */
    slot = e_find(lkey);
    if (slot < 0) {
        /* New entry - find empty slot */
        slot = e_slots();
        if (slot < 0)
            return E_EFULL;
    }
    
    /* Build record */
    e_clrb();
    e_buf[0] = E_ACTV;
    e_cpys(&e_buf[1], lkey, E_KEYSZ);
    e_cpys(&e_buf[17], lval, E_VALSZ);
    
    /* Write record */
    fd = open(E_FNAME, 2);
    if (fd == ERROR) {
        fd = creat(E_FNAME);
        if (fd == ERROR)
            return E_EOPEN;
    }
    
    if (seek(fd, slot, 0) == ERROR) {
        close(fd);
        return E_EWRIT;
    }
    
    if (write(fd, e_buf, 1) != 1) {
        close(fd);
        return E_EWRIT;
    }
    
    close(fd);
    return E_OK;
}

/* ------------------------------------------------------------
 * e_del - Delete environment variable
 * Returns E_OK on success, E_ENOTF if not found
 * ------------------------------------------------------------ */
int e_del(key)
char *key;
{
    int fd, slot;
    
    slot = e_find(key);
    if (slot < 0)
        return E_ENOTF;
    
    /* Mark as deleted */
    e_buf[0] = E_DEL;
    
    fd = open(E_FNAME, 2);
    if (fd == ERROR)
        return E_EOPEN;
    
    if (seek(fd, slot, 0) == ERROR) {
        close(fd);
        return E_EWRIT;
    }
    
    if (write(fd, e_buf, 1) != 1) {
        close(fd);
        return E_EWRIT;
    }
    
    close(fd);
    return E_OK;
}

/* ------------------------------------------------------------
 * e_list - List all environment variables
 * Calls callback function for each: cb(key, val)
 * Returns count of variables listed
 * ------------------------------------------------------------ */
int e_list(cb)
int (*cb)();
{
    int fd, slot, cnt;
    char tkey[E_KEYSZ];
    char tval[E_VALSZ];
    
    fd = open(E_FNAME, 0);
    if (fd == ERROR)
        return 0;
    
    cnt = 0;
    for (slot = 0; slot < E_MAXR; slot++) {
        if (seek(fd, slot, 0) == ERROR)
            break;
        if (read(fd, e_buf, 1) != 1)
            break;
        
        if (e_buf[0] == E_ACTV) {
            e_cpys(tkey, &e_buf[1], E_KEYSZ);
            e_cpys(tval, &e_buf[17], E_VALSZ);
            if (cb)
                (*cb)(tkey, tval);
            cnt++;
        }
    }
    
    close(fd);
    return cnt;
}

/* ------------------------------------------------------------
 * e_count - Count active environment variables
 * Returns count of variables
 * ------------------------------------------------------------ */
int e_count()
{
    int fd, slot, cnt;
    
    fd = open(E_FNAME, 0);
    if (fd == ERROR)
        return 0;
    
    cnt = 0;
    for (slot = 0; slot < E_MAXR; slot++) {
        if (seek(fd, slot, 0) == ERROR)
            break;
        if (read(fd, e_buf, 1) != 1)
            break;
        
        if (e_buf[0] == E_ACTV)
            cnt++;
    }
    
    close(fd);
    return cnt;
}

/* ------------------------------------------------------------
 * e_clear - Delete all environment variables
 * Returns E_OK on success
 * ------------------------------------------------------------ */
int e_clear()
{
    int fd;
    
    /* Simply delete and recreate the file */
    unlink(E_FNAME);
    
    fd = creat(E_FNAME);
    if (fd == ERROR)
        return E_EOPEN;
    
    close(fd);
    return E_OK;
}

/* ------------------------------------------------------------
 * e_exst - Check if key exists
 * Returns 1 if exists, 0 if not
 * ------------------------------------------------------------ */
int e_exst(key)
char *key;
{
    int slot;
    slot = e_find(key);
    return (slot >= 0) ? 1 : 0;
}