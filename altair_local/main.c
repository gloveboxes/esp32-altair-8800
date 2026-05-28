#include "intel8080.h"
#include "memory.h"
#include "universal_88dcdd.h"
#include "PortDrivers/chat_io.h"
#include "PortDrivers/environment_io.h"
#include "PortDrivers/host_files_io.h"
#include "PortDrivers/time_io.h"
#include "PortDrivers/weather_io.h"
#include "ansi_input.h"
#include "host_platform.h"
#include "io_ports.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASCII_MASK_7BIT 0x7f

#ifndef LOCAL_RUNNER_REPO_ROOT
#define LOCAL_RUNNER_REPO_ROOT ".."
#endif

static intel8080_t cpu;
static volatile sig_atomic_t keep_running = 1;

static const char *drive_a_path = LOCAL_RUNNER_REPO_ROOT "/disks/cpm63k.dsk";
static const char *drive_b_path = LOCAL_RUNNER_REPO_ROOT "/disks/bdsc-v1.60.dsk";
static const char *drive_c_path = LOCAL_RUNNER_REPO_ROOT "/disks/escape-posix.dsk";
static const char *drive_d_path = LOCAL_RUNNER_REPO_ROOT "/disks/blank.dsk";
static const char *apps_root_path = LOCAL_RUNNER_REPO_ROOT "/Apps";
static const char *env_file_path = LOCAL_RUNNER_REPO_ROOT "/altair_local/altair_env.txt";

static void handle_signal(int signum)
{
    (void)signum;
    keep_running = 0;
}

static uint8_t terminal_read(void)
{
    int raw_ch;
    uint8_t ch;

    raw_ch = host_terminal_read_byte();
    if (raw_ch < 0)
    {
        return ansi_input_process(0x00, host_monotonic_ms());
    }

    ch = (uint8_t)raw_ch;
    ch &= ASCII_MASK_7BIT;
    if (ch == 0x1d)
    {
        keep_running = 0;
        return 0x00;
    }
    ch = ansi_input_process(ch, host_monotonic_ms());
    if (ch == '\n')
    {
        return '\r';
    }
    return ch;
}

static void terminal_write(uint8_t c)
{
    unsigned char ch = (unsigned char)(c & ASCII_MASK_7BIT);

    if (!host_terminal_write_byte(ch))
    {
        keep_running = 0;
    }
}

static uint8_t sense_switches(void)
{
    return 0xff;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--drive-a PATH] [--drive-b PATH] [--drive-c PATH] [--drive-d PATH] [--apps-root PATH] [--env-file PATH]\n"
            "\n"
            "Defaults reference the repository disks and Apps folders:\n"
            "  A: %s\n"
            "  B: %s\n"
            "  C: %s\n"
            "  D: %s\n"
            "  Apps: %s\n"
            "  Env:  %s\n",
            program, drive_a_path, drive_b_path, drive_c_path, drive_d_path, apps_root_path, env_file_path);
}

static bool parse_args(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--drive-a") == 0 && i + 1 < argc)
        {
            drive_a_path = argv[++i];
        }
        else if (strcmp(argv[i], "--drive-b") == 0 && i + 1 < argc)
        {
            drive_b_path = argv[++i];
        }
        else if (strcmp(argv[i], "--drive-c") == 0 && i + 1 < argc)
        {
            drive_c_path = argv[++i];
        }
        else if (strcmp(argv[i], "--drive-d") == 0 && i + 1 < argc)
        {
            drive_d_path = argv[++i];
        }
        else if (strcmp(argv[i], "--apps-root") == 0 && i + 1 < argc)
        {
            apps_root_path = argv[++i];
        }
        else if (strcmp(argv[i], "--env-file") == 0 && i + 1 < argc)
        {
            env_file_path = argv[++i];
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return false;
        }
        else
        {
            print_usage(argv[0]);
            return false;
        }
    }

    return true;
}

int main(int argc, char **argv)
{
    disk_controller_t controller;

    if (!parse_args(argc, argv))
    {
        return argc > 1 ? 1 : 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!host_terminal_configure())
    {
        return 1;
    }
    atexit(host_terminal_restore);
    host_prefer_efficiency_core();

    if (!host_disk_init(drive_a_path, drive_b_path, drive_c_path, drive_d_path))
    {
        host_terminal_restore();
        fprintf(stderr, "altair-local: failed to open disk images\n");
        fprintf(stderr, "  A: %s\n  B: %s\n  C: %s\n  D: %s\n", drive_a_path, drive_b_path, drive_c_path, drive_d_path);
        return 1;
    }

    host_files_init(apps_root_path);
    environment_io_init(env_file_path);
    chat_io_init();
    weather_io_init();
    controller = host_disk_controller();

    memset(memory, 0x00, 64 * 1024);
    loadDiskLoader(0xff00);
    time_reset();
    i8080_reset(&cpu, terminal_read, terminal_write, sense_switches, &controller, io_port_in, io_port_out);
    i8080_examine(&cpu, 0xff00);

    while (keep_running)
    {
        i8080_cycle(&cpu);
    }

    host_disk_close();
    host_terminal_restore();
    return 0;
}
