#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#else
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif

#include "host_platform.h"

#include <stdio.h>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>

#define HOST_PENDING_INPUT_SIZE 8

static DWORD saved_input_mode;
static DWORD saved_output_mode;
static bool input_console = false;
static bool output_console = false;
static bool terminal_configured = false;
static unsigned char pending_input[HOST_PENDING_INPUT_SIZE];
static size_t pending_input_len = 0;
static size_t pending_input_pos = 0;

static void queue_bytes(const unsigned char *bytes, size_t len)
{
    size_t i;

    if (len > sizeof(pending_input))
    {
        len = sizeof(pending_input);
    }

    for (i = 0; i < len; i++)
    {
        pending_input[i] = bytes[i];
    }
    pending_input_len = len;
    pending_input_pos = 0;
}

static bool queue_windows_key(int key)
{
    static const unsigned char up[] = {0x1b, '[', 'A'};
    static const unsigned char down[] = {0x1b, '[', 'B'};
    static const unsigned char right[] = {0x1b, '[', 'C'};
    static const unsigned char left[] = {0x1b, '[', 'D'};
    static const unsigned char insert[] = {0x1b, '[', '2', '~'};
    static const unsigned char del[] = {0x1b, '[', '3', '~'};
    static const unsigned char page_up[] = {0x1b, '[', '5', '~'};
    static const unsigned char page_down[] = {0x1b, '[', '6', '~'};

    switch (key)
    {
        case 72:
            queue_bytes(up, sizeof(up));
            return true;
        case 80:
            queue_bytes(down, sizeof(down));
            return true;
        case 77:
            queue_bytes(right, sizeof(right));
            return true;
        case 75:
            queue_bytes(left, sizeof(left));
            return true;
        case 82:
            queue_bytes(insert, sizeof(insert));
            return true;
        case 83:
            queue_bytes(del, sizeof(del));
            return true;
        case 73:
            queue_bytes(page_up, sizeof(page_up));
            return true;
        case 81:
            queue_bytes(page_down, sizeof(page_down));
            return true;
        default:
            return false;
    }
}

void host_prefer_efficiency_core(void)
{
}

uint32_t host_monotonic_ms(void)
{
    static LARGE_INTEGER frequency;
    static LARGE_INTEGER start;
    LARGE_INTEGER now;

    if (frequency.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
    }

    QueryPerformanceCounter(&now);
    return (uint32_t)(((now.QuadPart - start.QuadPart) * 1000ULL) / frequency.QuadPart);
}

bool host_terminal_configure(void)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);

    input_console = GetConsoleMode(input, &saved_input_mode) != 0;
    output_console = GetConsoleMode(output, &saved_output_mode) != 0;

    if (input_console)
    {
        DWORD mode = saved_input_mode;
        mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        mode |= ENABLE_EXTENDED_FLAGS;
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        if (!SetConsoleMode(input, mode))
        {
            fprintf(stderr, "SetConsoleMode(stdin) failed (%lu)\n", GetLastError());
            return false;
        }
    }

    if (output_console)
    {
        DWORD mode = saved_output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(output, mode);
    }

    terminal_configured = true;
    return true;
}

void host_terminal_restore(void)
{
    if (!terminal_configured)
    {
        return;
    }

    if (input_console)
    {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_input_mode);
    }
    if (output_console)
    {
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved_output_mode);
    }
    terminal_configured = false;
}

int host_terminal_read_byte(void)
{
    int ch;

    if (pending_input_pos < pending_input_len)
    {
        return pending_input[pending_input_pos++];
    }
    pending_input_len = 0;
    pending_input_pos = 0;

    if (!input_console || !_kbhit())
    {
        return -1;
    }

    ch = _getch();
    if (ch == 0 || ch == 0xe0)
    {
        ch = _getch();
        if (queue_windows_key(ch))
        {
            return pending_input[pending_input_pos++];
        }
        return -1;
    }

    return ch & 0xff;
}

bool host_terminal_write_byte(uint8_t ch)
{
    return _write(_fileno(stdout), &ch, 1) == 1;
}

#else
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

static struct termios saved_termios;
static int saved_stdin_flags = -1;
static bool terminal_configured = false;

void host_prefer_efficiency_core(void)
{
#ifdef __APPLE__
    int rc;

    rc = pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
    if (rc != 0)
    {
        fprintf(stderr, "altair-local: failed to set utility QoS (%d)\n", rc);
    }
#endif
}

uint32_t host_monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0;
    }

    return (uint32_t)((now.tv_sec * 1000u) + (now.tv_nsec / 1000000u));
}

bool host_terminal_configure(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO))
    {
        return true;
    }

    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0)
    {
        perror("tcgetattr");
        return false;
    }

    raw = saved_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    {
        perror("tcsetattr");
        return false;
    }

    saved_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (saved_stdin_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags | O_NONBLOCK) != 0)
    {
        perror("fcntl");
        host_terminal_restore();
        return false;
    }

    terminal_configured = true;
    return true;
}

void host_terminal_restore(void)
{
    if (!terminal_configured)
    {
        return;
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    if (saved_stdin_flags >= 0)
    {
        fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags);
    }
    terminal_configured = false;
}

int host_terminal_read_byte(void)
{
    unsigned char ch;
    ssize_t n;

    n = read(STDIN_FILENO, &ch, 1);
    if (n <= 0)
    {
        return -1;
    }

    return ch;
}

bool host_terminal_write_byte(uint8_t ch)
{
    return write(STDOUT_FILENO, &ch, 1) >= 0 || errno == EAGAIN;
}

#endif
