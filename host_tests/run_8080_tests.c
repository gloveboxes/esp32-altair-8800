/* Host-side CPU test harness (8080 + Z80).
 *
 * Links the emulator core (../altair8800/x80.cxx via cpu_x80_adapter.cpp) and
 * runs CP/M-style diagnostic .COM ROMs through a minimal BDOS trap. This is the
 * safety net that protects the upcoming M-cycle accuracy refactor.
 *
 * This one source is mode-agnostic: it only drives the i8080_* C API and the
 * BDOS trap. The CMake build compiles it twice -- with X80_FORCE_8080 to run
 * the 8080 ROMs (TST8080/CPUTEST/8080PRE/8080EXM) and with X80_FORCE_Z80 to run
 * the Z80 exercisers (zexdoc/zexall) -- so both instruction-set specializations
 * of the single CPU core are validated by identical CP/M plumbing.
 *
 * BDOS model (just enough for the standard 8080 and Z80 test ROMs):
 *   - Program is loaded at 0x0100, PC := 0x0100.
 *   - 0x0000 contains HLT (sentinel); a RET to 0x0000 ends the run.
 *   - 0x0005 contains RET; before executing it we trap and synthesize
 *       C=0  -> warm boot (exit)
 *       C=2  -> conout: print char in E
 *       C=9  -> print '$'-terminated string at DE
 *     then pop the return address and continue.
 *
 * The standard test ROMs (TST8080, CPUTEST, 8080PRE, 8080EXM, zexdoc, zexall)
 * only use BDOS functions 2 and 9 plus a final JMP 0 or BDOS 0 to exit, so
 * no IN/OUT, sense switches, or disk controller is required.
 */

#include "intel8080.h"
#include "memory.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* --- CPU-side stubs ----------------------------------------------------- */

static uint8_t stub_term_in(void)                          { return 0; }
static void    stub_term_out(uint8_t b)                    { (void)b; }
static uint8_t stub_sense(void)                            { return 0; }
static void    stub_disk_select(uint8_t b)                 { (void)b; }
static uint8_t stub_disk_status(void)                      { return 0xff; }
static void    stub_disk_function(uint8_t b)               { (void)b; }
static uint8_t stub_sector(void)                           { return 0; }
static void    stub_disk_write(uint8_t b)                  { (void)b; }
static uint8_t stub_disk_read(void)                        { return 0; }
static uint8_t stub_io_in(uint8_t port)                    { (void)port; return 0; }
static void    stub_io_out(uint8_t port, uint8_t b)        { (void)port; (void)b; }

static const disk_controller_t HARNESS_DISK = {
    .disk_select   = stub_disk_select,
    .disk_status   = stub_disk_status,
    .disk_function = stub_disk_function,
    .sector        = stub_sector,
    .write         = stub_disk_write,
    .read          = stub_disk_read,
};

/* --- Captured BDOS output ---------------------------------------------- */

#define OUTBUF_CAP (256 * 1024)
static char  outbuf[OUTBUF_CAP];
static size_t outlen;

static void out_putc(char c)
{
    if (outlen < OUTBUF_CAP - 1) {
        outbuf[outlen++] = c;
        outbuf[outlen]   = '\0';
    }
    fputc(c, stdout);
}

/* --- Test runner -------------------------------------------------------- */

intel8080_t cpu;

typedef enum {
    RUN_OK        = 0,   /* program exited normally (RET to 0 or BDOS 0)   */
    RUN_TIMEOUT   = 1,   /* cycle limit hit                                */
    RUN_TOO_BIG   = 2,   /* program would overflow memory                  */
    RUN_BAD_HALT  = 3,   /* HLT executed somewhere we did not expect       */
} run_status_t;

static void bdos_call(bool *exit_out)
{
    uint8_t func = cpu.registers.c;
    *exit_out = false;

    switch (func) {
    case 0: /* warm boot / system reset */
        *exit_out = true;
        return;
    case 2: /* conout: char in E */
        out_putc((char)cpu.registers.e);
        break;
    case 9: { /* print '$'-terminated string at DE */
        uint16_t addr = cpu.registers.de;
        for (int i = 0; i < 65536; i++) {
            uint8_t ch = memory[(uint16_t)(addr + i)];
            if (ch == '$') break;
            out_putc((char)ch);
        }
        break;
    }
    default:
        /* Unsupported BDOS call - ignore. */
        break;
    }

    /* Simulate RET: PC := pop(); SP += 2. */
    uint16_t sp = cpu.registers.sp;
    cpu.registers.pc = (uint16_t)(memory[sp] | (memory[(uint16_t)(sp + 1)] << 8));
    cpu.registers.sp = (uint16_t)(sp + 2);
}

static run_status_t run_program(const uint8_t *program, size_t len, uint64_t cycle_limit)
{
    memset(memory, 0, sizeof(memory));

    i8080_reset(&cpu,
                stub_term_in, stub_term_out, stub_sense,
                (disk_controller_t *)&HARNESS_DISK,
                stub_io_in, stub_io_out);

    /* CP/M low-memory traps. */
    memory[0x0000] = 0x76; /* HLT - sentinel; we detect PC==0 before fetch */
    memory[0x0005] = 0xC9; /* RET - we trap on PC==5 before fetch          */

    if (0x0100u + (uint32_t)len > 0x10000u) return RUN_TOO_BIG;
    memcpy(&memory[0x0100], program, len);

    cpu.registers.sp = 0xFF00;
    /* Push 0x0000 so a final RET drops us into the exit sentinel. */
    cpu.registers.sp = (uint16_t)(cpu.registers.sp - 2);
    memory[cpu.registers.sp]              = 0x00;
    memory[(uint16_t)(cpu.registers.sp+1)] = 0x00;
    cpu.registers.pc = 0x0100;

    outlen    = 0;
    outbuf[0] = '\0';

    for (uint64_t c = 0; c < cycle_limit; c++) {
        if (cpu.registers.pc == 0x0000) return RUN_OK;
        if (cpu.registers.pc == 0x0005) {
            bool exit_now = false;
            bdos_call(&exit_now);
            if (exit_now) return RUN_OK;
            continue;
        }
        i8080_cycle(&cpu);
        if (cpu.halted) return RUN_BAD_HALT;
    }
    return RUN_TIMEOUT;
}

/* --- Built-in self-test program ---------------------------------------- *
 *
 * Hand-assembled at ORG 100H:
 *
 *   MVI A,5          ; A=5
 *   ADI 3            ; A=8
 *   MOV B,A          ; B=8
 *   MVI A,0Ah        ; A=10
 *   SUB B            ; A=2, expect Z=0
 *   CPI 2            ; expect Z=1
 *   JNZ FAIL
 *   MVI C,9
 *   LXI D,OKMSG
 *   CALL 5           ; BDOS print string
 *   JMP 0            ; exit
 * FAIL:
 *   MVI C,9
 *   LXI D,FAILMSG
 *   CALL 5
 *   JMP 0
 * OKMSG:   DB "SELFTEST PASS$"
 * FAILMSG: DB "SELFTEST FAIL$"
 */
static const uint8_t selftest_program[] = {
    /* 0100 */ 0x3E, 0x05, 0xC6, 0x03, 0x47, 0x3E, 0x0A, 0x90,
    /* 0108 */ 0xFE, 0x02, 0xC2, 0x18, 0x01, 0x0E, 0x09, 0x11,
    /* 0110 */ 0x23, 0x01, 0xCD, 0x05, 0x00, 0xC3, 0x00, 0x00,
    /* 0118 */ 0x0E, 0x09, 0x11, 0x31, 0x01, 0xCD, 0x05, 0x00,
    /* 0120 */ 0xC3, 0x00, 0x00,
    /* 0123 */ 'S','E','L','F','T','E','S','T',' ','P','A','S','S','$',
    /* 0131 */ 'S','E','L','F','T','E','S','T',' ','F','A','I','L','$',
};

/* --- ROM driver --------------------------------------------------------- */

static bool contains_ci(const char *hay, const char *needle)
{
    return strcasestr(hay, needle) != NULL;
}

static int run_rom_file(const char *path, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 0xFE00) {
        fprintf(stderr, "bad size for %s\n", path);
        fclose(f);
        return 1;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if (!buf) { fclose(f); return 1; }
    if (fread(buf, 1, sz, f) != sz) { free(buf); fclose(f); return 1; }
    fclose(f);

    printf("\n========== %s (%zu bytes) ==========\n", name, sz);
    /* Safety ceiling only; well-behaved ROMs exit early on warm boot. The
     * Z80 exercisers (zexdoc/zexall) execute far more instructions than the
     * 8080 ROMs, so the cap is generous. */
    run_status_t rs = run_program(buf, sz, 60000000000ULL);
    free(buf);
    printf("\n--- run status: ");
    switch (rs) {
    case RUN_OK:       printf("clean exit\n"); break;
    case RUN_TIMEOUT:  printf("TIMEOUT (cycle limit reached)\n"); break;
    case RUN_TOO_BIG:  printf("TOO BIG\n"); break;
    case RUN_BAD_HALT: printf("unexpected HLT\n"); break;
    }

    bool failed = (rs != RUN_OK)
               || contains_ci(outbuf, "ERROR")
               || contains_ci(outbuf, "FAIL");
    if (failed) {
        printf("RESULT: %s FAILED\n", name);
        return 1;
    }
    printf("RESULT: %s ok\n", name);
    return 0;
}

static int run_roms_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr,
            "No ROMs directory at %s\n"
            "Drop standard 8080 test ROMs (TST8080.COM, CPUTEST.COM, "
            "8080PRE.COM, 8080EXM.COM) into that path to enable this test.\n",
            dir);
        return 77; /* CTest SKIP_RETURN_CODE */
    }

    int total = 0, fails = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        size_t l = strlen(n);
        if (l < 5) continue;
        if (strcasecmp(n + l - 4, ".COM") != 0) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, n);
        total++;
        fails += run_rom_file(path, n);
    }
    closedir(d);

    if (total == 0) {
        fprintf(stderr, "%s contains no .COM files; skipping ROM tests.\n", dir);
        return 77;
    }
    printf("\n========== ROM summary: %d/%d passed ==========\n",
           total - fails, total);
    return fails == 0 ? 0 : 1;
}

/* --- Selftest driver --------------------------------------------------- */

static int run_selftest(void)
{
    printf("========== Built-in self-test ==========\n");
    run_status_t rs = run_program(selftest_program, sizeof(selftest_program), 1000000);
    printf("\n--- run status: ");
    switch (rs) {
    case RUN_OK:       printf("clean exit\n"); break;
    case RUN_TIMEOUT:  printf("TIMEOUT\n"); break;
    case RUN_TOO_BIG:  printf("TOO BIG\n"); break;
    case RUN_BAD_HALT: printf("unexpected HLT\n"); break;
    }
    if (rs != RUN_OK) {
        printf("RESULT: selftest FAILED (no clean exit)\n");
        return 1;
    }
    if (strstr(outbuf, "SELFTEST PASS") == NULL) {
        printf("RESULT: selftest FAILED (no PASS marker; output was: \"%s\")\n", outbuf);
        return 1;
    }
    if (strstr(outbuf, "SELFTEST FAIL") != NULL) {
        printf("RESULT: selftest FAILED (FAIL marker present)\n");
        return 1;
    }
    printf("RESULT: selftest ok\n");
    return 0;
}

/* --- main -------------------------------------------------------------- */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s --selftest           run built-in 8080 self-test\n"
        "  %s --roms <directory>   run all .COM files in <directory>\n",
        argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        return run_selftest();
    }
    if (argc >= 3 && strcmp(argv[1], "--roms") == 0) {
        return run_roms_dir(argv[2]);
    }
    /* Default: selftest + roms/ alongside this binary's invocation dir. */
    if (argc == 1) {
        int r1 = run_selftest();
        int r2 = run_roms_dir("host_tests/roms");
        if (r2 == 77) r2 = 0; /* skipping is fine */
        return (r1 == 0 && r2 == 0) ? 0 : 1;
    }
    usage(argv[0]);
    return 2;
}
