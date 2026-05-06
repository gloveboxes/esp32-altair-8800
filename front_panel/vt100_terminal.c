/**
 * @file vt100_terminal.c
 * @brief On-device VT100 terminal for Waveshare AXS15231B displays.
 *
 * The AXS15231B path follows the Pico VT100 reference layout: an 80x30
 * terminal using 6x10 cells, with the bottom 20 px reserved for IP and compact
 * Altair status LEDs.
 */

#include "vt100_terminal.h"
#include "panel_display.h"

#include "sdkconfig.h"

#if CONFIG_ALTAIR_DISPLAY_AXS15231B
#include "axs15231b_lcd.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_timer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- Layout constants -------------------------------------------------- */

#define CELL_W       6
#define CELL_H      10
#define GLYPH_XOFF   0
#define GLYPH_YOFF   1
#define GLYPH_W      5
#define GLYPH_H      7

#define LEFT_MARGIN  0
#define HAS_STATUS_BAR 1

#define TERM_H      (VT100_ROWS * CELL_H)
#define STATUS_H    20
#define STATUS_Y    TERM_H
#define PRESENT_ROWS_PER_BAND 10

/* ---- Embedded 5x7 bitmap font ----------------------------------------- *
 *                                                                           *
 * 95 entries covering printable ASCII 0x20–0x7E.                           *
 * Each entry: 5 bytes, one per column, bit 0 = top row, bit 6 = bottom row.*
 * ------------------------------------------------------------------------ */
static const uint8_t s_font[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 0x20  ' '  */
    {0x00,0x00,0x5F,0x00,0x00}, /* 0x21  '!'  */
    {0x00,0x07,0x00,0x07,0x00}, /* 0x22  '"'  */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 0x23  '#'  */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 0x24  '$'  */
    {0x23,0x13,0x08,0x64,0x62}, /* 0x25  '%'  */
    {0x36,0x49,0x55,0x22,0x50}, /* 0x26  '&'  */
    {0x00,0x05,0x03,0x00,0x00}, /* 0x27  '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 0x28  '('  */
    {0x00,0x41,0x22,0x1C,0x00}, /* 0x29  ')'  */
    {0x14,0x08,0x3E,0x08,0x14}, /* 0x2A  '*'  */
    {0x08,0x08,0x3E,0x08,0x08}, /* 0x2B  '+'  */
    {0x00,0x50,0x30,0x00,0x00}, /* 0x2C  ','  */
    {0x08,0x08,0x08,0x08,0x08}, /* 0x2D  '-'  */
    {0x00,0x60,0x60,0x00,0x00}, /* 0x2E  '.'  */
    {0x20,0x10,0x08,0x04,0x02}, /* 0x2F  '/'  */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0x30  '0'  */
    {0x00,0x42,0x7F,0x40,0x00}, /* 0x31  '1'  */
    {0x42,0x61,0x51,0x49,0x46}, /* 0x32  '2'  */
    {0x21,0x41,0x45,0x4B,0x31}, /* 0x33  '3'  */
    {0x18,0x14,0x12,0x7F,0x10}, /* 0x34  '4'  */
    {0x27,0x45,0x45,0x45,0x39}, /* 0x35  '5'  */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 0x36  '6'  */
    {0x01,0x71,0x09,0x05,0x03}, /* 0x37  '7'  */
    {0x36,0x49,0x49,0x49,0x36}, /* 0x38  '8'  */
    {0x06,0x49,0x49,0x29,0x1E}, /* 0x39  '9'  */
    {0x00,0x36,0x36,0x00,0x00}, /* 0x3A  ':'  */
    {0x00,0x56,0x36,0x00,0x00}, /* 0x3B  ';'  */
    {0x08,0x14,0x22,0x41,0x00}, /* 0x3C  '<'  */
    {0x14,0x14,0x14,0x14,0x14}, /* 0x3D  '='  */
    {0x00,0x41,0x22,0x14,0x08}, /* 0x3E  '>'  */
    {0x02,0x01,0x51,0x09,0x06}, /* 0x3F  '?'  */
    {0x32,0x49,0x79,0x41,0x3E}, /* 0x40  '@'  */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 0x41  'A'  */
    {0x7F,0x49,0x49,0x49,0x36}, /* 0x42  'B'  */
    {0x3E,0x41,0x41,0x41,0x22}, /* 0x43  'C'  */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 0x44  'D'  */
    {0x7F,0x49,0x49,0x49,0x41}, /* 0x45  'E'  */
    {0x7F,0x09,0x09,0x09,0x01}, /* 0x46  'F'  */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 0x47  'G'  */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 0x48  'H'  */
    {0x00,0x41,0x7F,0x41,0x00}, /* 0x49  'I'  */
    {0x20,0x40,0x41,0x3F,0x01}, /* 0x4A  'J'  */
    {0x7F,0x08,0x14,0x22,0x41}, /* 0x4B  'K'  */
    {0x7F,0x40,0x40,0x40,0x40}, /* 0x4C  'L'  */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 0x4D  'M'  */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 0x4E  'N'  */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 0x4F  'O'  */
    {0x7F,0x09,0x09,0x09,0x06}, /* 0x50  'P'  */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 0x51  'Q'  */
    {0x7F,0x09,0x19,0x29,0x46}, /* 0x52  'R'  */
    {0x46,0x49,0x49,0x49,0x31}, /* 0x53  'S'  */
    {0x01,0x01,0x7F,0x01,0x01}, /* 0x54  'T'  */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 0x55  'U'  */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 0x56  'V'  */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 0x57  'W'  */
    {0x63,0x14,0x08,0x14,0x63}, /* 0x58  'X'  */
    {0x07,0x08,0x70,0x08,0x07}, /* 0x59  'Y'  */
    {0x61,0x51,0x49,0x45,0x43}, /* 0x5A  'Z'  */
    {0x00,0x7F,0x41,0x41,0x00}, /* 0x5B  '['  */
    {0x02,0x04,0x08,0x10,0x20}, /* 0x5C  '\'  */
    {0x00,0x41,0x41,0x7F,0x00}, /* 0x5D  ']'  */
    {0x04,0x02,0x01,0x02,0x04}, /* 0x5E  '^'  */
    {0x40,0x40,0x40,0x40,0x40}, /* 0x5F  '_'  */
    {0x00,0x01,0x02,0x04,0x00}, /* 0x60  '`'  */
    {0x20,0x54,0x54,0x54,0x78}, /* 0x61  'a'  */
    {0x7F,0x48,0x44,0x44,0x38}, /* 0x62  'b'  */
    {0x38,0x44,0x44,0x44,0x20}, /* 0x63  'c'  */
    {0x38,0x44,0x44,0x48,0x7F}, /* 0x64  'd'  */
    {0x38,0x54,0x54,0x54,0x18}, /* 0x65  'e'  */
    {0x08,0x7E,0x09,0x01,0x02}, /* 0x66  'f'  */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 0x67  'g'  */
    {0x7F,0x08,0x04,0x04,0x78}, /* 0x68  'h'  */
    {0x00,0x44,0x7D,0x40,0x00}, /* 0x69  'i'  */
    {0x20,0x40,0x44,0x3D,0x00}, /* 0x6A  'j'  */
    {0x7F,0x10,0x28,0x44,0x00}, /* 0x6B  'k'  */
    {0x00,0x41,0x7F,0x40,0x00}, /* 0x6C  'l'  */
    {0x7C,0x04,0x18,0x04,0x78}, /* 0x6D  'm'  */
    {0x7C,0x08,0x04,0x04,0x78}, /* 0x6E  'n'  */
    {0x38,0x44,0x44,0x44,0x38}, /* 0x6F  'o'  */
    {0x7C,0x14,0x14,0x14,0x08}, /* 0x70  'p'  */
    {0x08,0x14,0x14,0x18,0x7C}, /* 0x71  'q'  */
    {0x7C,0x08,0x04,0x04,0x08}, /* 0x72  'r'  */
    {0x48,0x54,0x54,0x54,0x20}, /* 0x73  's'  */
    {0x04,0x3F,0x44,0x40,0x20}, /* 0x74  't'  */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 0x75  'u'  */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 0x76  'v'  */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 0x77  'w'  */
    {0x44,0x28,0x10,0x28,0x44}, /* 0x78  'x'  */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 0x79  'y'  */
    {0x44,0x64,0x54,0x4C,0x44}, /* 0x7A  'z'  */
    {0x00,0x08,0x36,0x41,0x00}, /* 0x7B  '{'  */
    {0x00,0x00,0x7F,0x00,0x00}, /* 0x7C  '|'  */
    {0x00,0x41,0x36,0x08,0x00}, /* 0x7D  '}'  */
    {0x10,0x08,0x08,0x10,0x08}, /* 0x7E  '~'  */
};

/* Filled 5x7 oval used for bright-white 'O' game balls (e.g. breakout). The
 * bit layout matches s_font: 5 columns, low 7 bits per column = pixel rows.
 * Outer columns lose the top and bottom row to round the corners. */
static const uint8_t s_ball_glyph[5] = {
    0x3E, /* 0011110 — col 0 */
    0x7F, /* 0111111 — col 1 */
    0x7F, /* 0111111 — col 2 */
    0x7F, /* 0111111 — col 3 */
    0x3E, /* 0011110 — col 4 */
};

/* ---- Colour and state -------------------------------------------------- */

#define VT_COLOR_COUNT 16

static const panel_color_t s_palette[VT_COLOR_COUNT] = {
    0x0000, 0xA800, 0x0540, 0xAA80, 0x0015, 0xA815, 0x0555, 0xAD55,
    0x528A, 0xFAAA, 0x57EA, 0xFFEA, 0x555F, 0xFAAF, 0x57FF, 0xFFFF,
};

typedef struct {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
} terminal_cell_t;

static terminal_cell_t  s_buffer[VT100_ROWS][VT100_COLS];
static terminal_cell_t  s_snapshot[VT100_ROWS][VT100_COLS];
static char             s_snapshot_ip[64];
static uint32_t         s_dirty_rows;
static int              s_col;
static int              s_row;
static int              s_saved_col;
static int              s_saved_row;
static uint8_t          s_cur_fg;
static uint8_t          s_cur_bg;
static bool             s_bold;
static bool             s_cursor_visible;
static bool             s_initialized;
static SemaphoreHandle_t s_mutex;
static bool             s_status_draw_partial;

static uint16_t s_status_address;
static uint8_t  s_status_data;
static uint16_t s_status_bits;
static char     s_status_ip[64];
static bool     s_status_dirty;

typedef enum {
    VT_STATE_NORMAL = 0,
    VT_STATE_ESC,
    VT_STATE_CSI,
    VT_STATE_ESC_OTHER,
} vt_state_t;

#define CSI_MAX_PARAMS 8

static vt_state_t s_vt_state;
static int        s_csi_params[CSI_MAX_PARAMS];
static int        s_csi_nparams;
static bool       s_csi_priv;

static void mark_all_dirty(void)
{
    s_dirty_rows = (1UL << VT100_ROWS) - 1UL;
}

static void mark_row_dirty(int row)
{
    if ((unsigned)row < VT100_ROWS) {
        s_dirty_rows |= (1UL << row);
    }
}

static int clamp(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int param_or_default(int index, int def)
{
    return (index < s_csi_nparams && s_csi_params[index] > 0) ? s_csi_params[index] : def;
}

static void reset_parser(void)
{
    s_vt_state = VT_STATE_NORMAL;
    s_csi_nparams = 0;
    s_csi_priv = false;
    memset(s_csi_params, 0, sizeof(s_csi_params));
}

static void set_cell_blank(int row, int col, uint8_t fg, uint8_t bg)
{
    s_buffer[row][col].ch = ' ';
    s_buffer[row][col].fg = fg;
    s_buffer[row][col].bg = bg;
}

static void clear_row_range(int row, int start_col, int end_col)
{
    start_col = clamp(start_col, 0, VT100_COLS);
    end_col = clamp(end_col, 0, VT100_COLS);
    for (int col = start_col; col < end_col; col++) {
        set_cell_blank(row, col, s_cur_fg, s_cur_bg);
    }
    mark_row_dirty(row);
}

static void clear_row_default(int row)
{
    for (int col = 0; col < VT100_COLS; col++) {
        set_cell_blank(row, col, 7, HAS_STATUS_BAR ? 0 : 15);
    }
    mark_row_dirty(row);
}

static void reset_terminal_state(bool preserve_status)
{
    char saved_ip[sizeof(s_status_ip)];
    uint16_t saved_address = s_status_address;
    uint8_t saved_data = s_status_data;
    uint16_t saved_status = s_status_bits;

    if (preserve_status) {
        memcpy(saved_ip, s_status_ip, sizeof(saved_ip));
    }

    s_col = 0;
    s_row = 0;
    s_saved_col = 0;
    s_saved_row = 0;
    s_cur_fg = 7;
    s_cur_bg = HAS_STATUS_BAR ? 0 : 15;
    s_bold = false;
    s_cursor_visible = true;
    s_dirty_rows = 0;
    reset_parser();

    s_status_ip[0] = '\0';
    s_status_address = 0;
    s_status_data = 0;
    s_status_bits = 0;
    if (preserve_status) {
        memcpy(s_status_ip, saved_ip, sizeof(s_status_ip));
        s_status_address = saved_address;
        s_status_data = saved_data;
        s_status_bits = saved_status;
    }
    s_status_dirty = HAS_STATUS_BAR;

    for (int row = 0; row < VT100_ROWS; row++) {
        clear_row_default(row);
    }
}

static void scroll_up(void)
{
    memmove(s_buffer[0], s_buffer[1], sizeof(s_buffer[0]) * (VT100_ROWS - 1));
    for (int col = 0; col < VT100_COLS; col++) {
        set_cell_blank(VT100_ROWS - 1, col, s_cur_fg, s_cur_bg);
    }
    mark_all_dirty();
}

static void newline(void)
{
    mark_row_dirty(s_row);
    s_row++;
    if (s_row >= VT100_ROWS) {
        s_row = VT100_ROWS - 1;
        scroll_up();
    }
    mark_row_dirty(s_row);
}

static void sgr(void)
{
    if (s_csi_nparams == 0) {
        s_cur_fg = 7;
        s_cur_bg = HAS_STATUS_BAR ? 0 : 15;
        s_bold = false;
        return;
    }

    for (int i = 0; i < s_csi_nparams; i++) {
        int value = s_csi_params[i];
        if (value == 0) {
            s_cur_fg = 7;
            s_cur_bg = HAS_STATUS_BAR ? 0 : 15;
            s_bold = false;
        } else if (value == 1) {
            s_bold = true;
            if (s_cur_fg < 8) s_cur_fg += 8;
        } else if (value == 22) {
            s_bold = false;
            if (s_cur_fg >= 8) s_cur_fg -= 8;
        } else if (value == 7) {
            uint8_t swap = s_cur_fg;
            s_cur_fg = s_cur_bg;
            s_cur_bg = swap;
        } else if (value >= 30 && value <= 37) {
            s_cur_fg = (uint8_t)(value - 30 + (s_bold ? 8 : 0));
        } else if (value >= 40 && value <= 47) {
            s_cur_bg = (uint8_t)(value - 40);
        } else if (value == 39) {
            s_cur_fg = (uint8_t)(7 + (s_bold ? 8 : 0));
        } else if (value == 49) {
            s_cur_bg = HAS_STATUS_BAR ? 0 : 15;
        } else if (value >= 90 && value <= 97) {
            s_cur_fg = (uint8_t)(value - 90 + 8);
        } else if (value >= 100 && value <= 107) {
            s_cur_bg = (uint8_t)(value - 100 + 8);
        } else if ((value == 38 || value == 48) && i + 2 < s_csi_nparams && s_csi_params[i + 1] == 5) {
            int index = s_csi_params[i + 2];
            if ((unsigned)index < VT_COLOR_COUNT) {
                if (value == 38) s_cur_fg = (uint8_t)index;
                else s_cur_bg = (uint8_t)index;
            }
            i += 2;
        }
    }
}

static void handle_csi(uint8_t final)
{
    int count;

    switch (final) {
        case 'A':
            mark_row_dirty(s_row);
            s_row = clamp(s_row - param_or_default(0, 1), 0, VT100_ROWS - 1);
            mark_row_dirty(s_row);
            break;
        case 'B':
            mark_row_dirty(s_row);
            s_row = clamp(s_row + param_or_default(0, 1), 0, VT100_ROWS - 1);
            mark_row_dirty(s_row);
            break;
        case 'C':
            mark_row_dirty(s_row);
            s_col = clamp(s_col + param_or_default(0, 1), 0, VT100_COLS - 1);
            mark_row_dirty(s_row);
            break;
        case 'D':
            mark_row_dirty(s_row);
            s_col = clamp(s_col - param_or_default(0, 1), 0, VT100_COLS - 1);
            mark_row_dirty(s_row);
            break;
        case 'G':
            mark_row_dirty(s_row);
            s_col = clamp(param_or_default(0, 1) - 1, 0, VT100_COLS - 1);
            mark_row_dirty(s_row);
            break;
        case 'H':
        case 'f':
            mark_row_dirty(s_row);
            s_row = clamp(param_or_default(0, 1) - 1, 0, VT100_ROWS - 1);
            s_col = clamp(param_or_default(1, 1) - 1, 0, VT100_COLS - 1);
            mark_row_dirty(s_row);
            break;
        case 'J': {
            int mode = param_or_default(0, 0);
            if (mode == 2 || mode == 3) {
                for (int row = 0; row < VT100_ROWS; row++) clear_row_range(row, 0, VT100_COLS);
                s_col = 0;
                s_row = 0;
            } else if (mode == 1) {
                for (int row = 0; row < s_row; row++) clear_row_range(row, 0, VT100_COLS);
                clear_row_range(s_row, 0, s_col + 1);
            } else {
                clear_row_range(s_row, s_col, VT100_COLS);
                for (int row = s_row + 1; row < VT100_ROWS; row++) clear_row_range(row, 0, VT100_COLS);
            }
            break;
        }
        case 'K': {
            int mode = param_or_default(0, 0);
            if (mode == 2) clear_row_range(s_row, 0, VT100_COLS);
            else if (mode == 1) clear_row_range(s_row, 0, s_col + 1);
            else clear_row_range(s_row, s_col, VT100_COLS);
            break;
        }
        case 'S':
            count = param_or_default(0, 1);
            for (int i = 0; i < count; i++) scroll_up();
            break;
        case 'T':
            count = param_or_default(0, 1);
            for (int i = 0; i < count; i++) {
                memmove(s_buffer[1], s_buffer[0], sizeof(s_buffer[0]) * (VT100_ROWS - 1));
                for (int col = 0; col < VT100_COLS; col++) set_cell_blank(0, col, s_cur_fg, s_cur_bg);
            }
            mark_all_dirty();
            break;
        case 'L':
            count = clamp(param_or_default(0, 1), 1, VT100_ROWS - s_row);
            memmove(s_buffer[s_row + count], s_buffer[s_row], sizeof(s_buffer[0]) * (VT100_ROWS - s_row - count));
            for (int row = s_row; row < s_row + count; row++) {
                for (int col = 0; col < VT100_COLS; col++) set_cell_blank(row, col, s_cur_fg, s_cur_bg);
            }
            mark_all_dirty();
            break;
        case 'M':
            count = clamp(param_or_default(0, 1), 1, VT100_ROWS - s_row);
            memmove(s_buffer[s_row], s_buffer[s_row + count], sizeof(s_buffer[0]) * (VT100_ROWS - s_row - count));
            for (int row = VT100_ROWS - count; row < VT100_ROWS; row++) {
                for (int col = 0; col < VT100_COLS; col++) set_cell_blank(row, col, s_cur_fg, s_cur_bg);
            }
            mark_all_dirty();
            break;
        case '@':
            count = clamp(param_or_default(0, 1), 1, VT100_COLS - s_col);
            for (int col = VT100_COLS - 1; col >= s_col + count; col--) s_buffer[s_row][col] = s_buffer[s_row][col - count];
            for (int col = s_col; col < s_col + count; col++) set_cell_blank(s_row, col, s_cur_fg, s_cur_bg);
            mark_row_dirty(s_row);
            break;
        case 'P':
            count = clamp(param_or_default(0, 1), 1, VT100_COLS - s_col);
            for (int col = s_col; col < VT100_COLS - count; col++) s_buffer[s_row][col] = s_buffer[s_row][col + count];
            for (int col = VT100_COLS - count; col < VT100_COLS; col++) set_cell_blank(s_row, col, s_cur_fg, s_cur_bg);
            mark_row_dirty(s_row);
            break;
        case 'm':
            sgr();
            break;
        case 's':
            s_saved_col = s_col;
            s_saved_row = s_row;
            break;
        case 'u':
            mark_row_dirty(s_row);
            s_col = s_saved_col;
            s_row = s_saved_row;
            mark_row_dirty(s_row);
            break;
        case 'h':
            if (s_csi_priv && param_or_default(0, 0) == 25) s_cursor_visible = true;
            break;
        case 'l':
            if (s_csi_priv && param_or_default(0, 0) == 25) s_cursor_visible = false;
            break;
        default:
            break;
    }
}

static void emit_char(uint8_t ch)
{
    if (s_col >= VT100_COLS) {
        s_col = 0;
        newline();
    }

    s_buffer[s_row][s_col].ch = ch;
    s_buffer[s_row][s_col].fg = s_cur_fg;
    s_buffer[s_row][s_col].bg = s_cur_bg;
    mark_row_dirty(s_row);
    s_col++;
}

static void draw_cell(int col, int row, terminal_cell_t cell, bool inverse)
{
    panel_color_t fg = s_palette[inverse ? cell.bg : cell.fg];
    panel_color_t bg = s_palette[inverse ? cell.fg : cell.bg];
    if (!HAS_STATUS_BAR) {
        fg = inverse ? PANEL_COLOR_WHITE : PANEL_COLOR_BLACK;
        bg = inverse ? PANEL_COLOR_BLACK : PANEL_COLOR_WHITE;
    }

    /* Detect "ball" cell: bright-white 'O' on black, used by breakout-style
     * games. Substitute a filled oval glyph in place of the font letter. The
     * detection uses the raw cell attrs (not the inverse copy), so that an
     * inverted cursor over a ball still draws the ball glyph. */
    bool is_ball = (cell.ch == 'O' && cell.fg == 15 && cell.bg == 0);

    int x = LEFT_MARGIN + col * CELL_W;
    int y = row * CELL_H;

#if CONFIG_ALTAIR_DISPLAY_AXS15231B
    /* Fast path: one column-major pass directly into the framebuffer. */
    bool printable = (cell.ch >= 0x21 && cell.ch <= 0x7E);
    const uint8_t *glyph_cols = is_ball     ? s_ball_glyph
                                : printable ? s_font[cell.ch - 0x20]
                                            : NULL;
    axs15231b_lcd_blit_glyph_cell(x, y, fg, bg, glyph_cols,
                                  CELL_W, CELL_H, GLYPH_W, GLYPH_H,
                                  GLYPH_XOFF, GLYPH_YOFF);
    return;
#endif

    panel_display_fill_rect(x, y, CELL_W, CELL_H, bg);

    if (!is_ball && (cell.ch < 0x21 || cell.ch > 0x7E)) {
        return;
    }

    const uint8_t *cols = is_ball ? s_ball_glyph : s_font[cell.ch - 0x20];
    for (int fc = 0; fc < GLYPH_W; fc++) {
        uint8_t bits = cols[fc];
        for (int fr = 0; fr < GLYPH_H; fr++) {
            if (bits & (1U << fr)) {
                panel_display_draw_pixel(x + GLYPH_XOFF + fc, y + GLYPH_YOFF + fr, fg);
            }
        }
    }
}

static void draw_status_text(const char *text, int x, int y, panel_color_t fg, panel_color_t bg)
{
    while (*text && x < panel_display_width()) {
        char ch = *text++;
        if (ch < 0x21 || ch > 0x7E) {
            x += CELL_W;
            continue;
        }
        int glyph = ch - 0x20;
        if (s_status_draw_partial) {
            panel_display_status_region_fill_rect(x, y - STATUS_Y, CELL_W, CELL_H, bg);
        } else {
            panel_display_fill_rect(x, y, CELL_W, CELL_H, bg);
        }
        for (int fc = 0; fc < GLYPH_W; fc++) {
            uint8_t bits = s_font[glyph][fc];
            for (int fr = 0; fr < GLYPH_H; fr++) {
                if (bits & (1U << fr)) {
                    if (s_status_draw_partial) {
                        panel_display_status_region_draw_pixel(x + fc, y + fr - STATUS_Y, fg);
                    } else {
                        panel_display_draw_pixel(x + fc, y + fr, fg);
                    }
                }
            }
        }
        x += CELL_W;
    }
}

static void draw_status_leds(uint32_t bits, int count, int x, int y, panel_color_t on, panel_color_t off)
{
    for (int bit = count - 1; bit >= 0; bit--) {
        if (s_status_draw_partial) {
            panel_display_status_region_fill_rect(x, y - STATUS_Y, 4, 4, (bits & (1UL << bit)) ? on : off);
        } else {
            panel_display_fill_rect(x, y, 4, 4, (bits & (1UL << bit)) ? on : off);
        }
        x += 6;
    }
}

static void draw_status_bar(uint16_t address, uint8_t data, uint16_t status, const char *ip_text)
{
    if (!HAS_STATUS_BAR) return;

    const panel_color_t bg = 0x192A;
    const panel_color_t sep = 0x5B56;
    const panel_color_t text = 0xB65B;
    const panel_color_t title = PANEL_COLOR_WHITE;
    const panel_color_t sub = 0x8D13;
    const panel_color_t led_on = PANEL_COLOR_RED;
    const panel_color_t led_off = 0x3000;

    s_status_draw_partial = panel_display_status_region_supported();
    if (s_status_draw_partial) {
        panel_display_status_region_clear(bg);
        panel_display_status_region_fill_rect(0, 0, panel_display_width(), 1, sep);
    } else {
        panel_display_fill_rect(0, STATUS_Y, panel_display_width(), STATUS_H, bg);
        panel_display_fill_rect(0, STATUS_Y, panel_display_width(), 1, sep);
    }

    if (ip_text[0] != '\0') {
        draw_status_text(ip_text, 4, STATUS_Y + 5, text, bg);
    }

    draw_status_text("ALTAIR 8800", (panel_display_width() - 11 * CELL_W) / 2, STATUS_Y + 1, title, bg);
    draw_status_text("COMPUTER", (panel_display_width() - 8 * CELL_W) / 2, STATUS_Y + 10, sub, bg);

    int data_x = panel_display_width() - 4 - (8 * 6 - 2);
    int status_x = data_x - 8 - (10 * 6 - 2);
    int address_x = panel_display_width() - 4 - (16 * 6 - 2);
    draw_status_leds(status, 10, status_x, STATUS_Y + 3, led_on, led_off);
    draw_status_leds(data, 8, data_x, STATUS_Y + 3, led_on, led_off);
    draw_status_leds(address, 16, address_x, STATUS_Y + 13, led_on, led_off);
    s_status_draw_partial = false;
}

static uint32_t row_mask_range(int start_row, int row_count)
{
    uint32_t mask = 0;
    for (int row = start_row; row < start_row + row_count && row < VT100_ROWS; row++) {
        mask |= (1UL << row);
    }
    return mask;
}

static void present_dirty_text_bands(uint32_t dirty_rows)
{
    if (!panel_display_present_band_supported()) {
        panel_display_present();
        return;
    }

    if (dirty_rows) {
        dirty_rows = row_mask_range(0, VT100_ROWS);
    }

    for (int row = 0; row < VT100_ROWS; row += PRESENT_ROWS_PER_BAND) {
        int rows_in_band = VT100_ROWS - row;
        if (rows_in_band > PRESENT_ROWS_PER_BAND) rows_in_band = PRESENT_ROWS_PER_BAND;
        uint32_t mask = row_mask_range(row, rows_in_band);
        if (dirty_rows & mask) {
            panel_display_present_band(row * CELL_H, rows_in_band * CELL_H);
        }
    }
}

static void process_byte_locked(uint8_t c)
{
    mark_row_dirty(s_row);

    switch (s_vt_state) {
        case VT_STATE_NORMAL:
            if (c == 0x1B) {
                s_vt_state = VT_STATE_ESC;
            } else if (c == '\r') {
                s_col = 0;
            } else if (c == '\n') {
                newline();
            } else if (c == '\b') {
                if (s_col > 0) s_col--;
            } else if (c == '\t') {
                int next = (s_col + 8) & ~7;
                s_col = clamp(next, 0, VT100_COLS - 1);
            } else if (c >= 0x20 && c <= 0x7E) {
                emit_char(c);
            }
            break;

        case VT_STATE_ESC:
            if (c == '[') {
                s_vt_state = VT_STATE_CSI;
                s_csi_nparams = 0;
                s_csi_priv = false;
                memset(s_csi_params, 0, sizeof(s_csi_params));
            } else if (c == '(' || c == ')' || c == '#') {
                s_vt_state = VT_STATE_ESC_OTHER;
            } else if (c == 'D') {
                newline();
                s_vt_state = VT_STATE_NORMAL;
            } else if (c == 'M') {
                if (s_row > 0) s_row--;
                s_vt_state = VT_STATE_NORMAL;
            } else if (c == 'c') {
                reset_terminal_state(true);
            } else if (c == '7') {
                s_saved_col = s_col;
                s_saved_row = s_row;
                s_vt_state = VT_STATE_NORMAL;
            } else if (c == '8') {
                s_col = s_saved_col;
                s_row = s_saved_row;
                s_vt_state = VT_STATE_NORMAL;
            } else {
                s_vt_state = VT_STATE_NORMAL;
            }
            break;

        case VT_STATE_ESC_OTHER:
            s_vt_state = VT_STATE_NORMAL;
            break;

        case VT_STATE_CSI:
            if (c == '?') {
                s_csi_priv = true;
            } else if (c >= '0' && c <= '9') {
                if (s_csi_nparams == 0) s_csi_nparams = 1;
                s_csi_params[s_csi_nparams - 1] = s_csi_params[s_csi_nparams - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (s_csi_nparams == 0) s_csi_nparams = 1;
                if (s_csi_nparams < CSI_MAX_PARAMS) {
                    s_csi_nparams++;
                    s_csi_params[s_csi_nparams - 1] = 0;
                }
            } else if (c >= 0x40 && c <= 0x7E) {
                handle_csi(c);
                s_csi_priv = false;
                s_vt_state = VT_STATE_NORMAL;
            } else {
                s_csi_priv = false;
                s_vt_state = VT_STATE_NORMAL;
            }
            break;
    }

    mark_row_dirty(s_row);
}

void vt100_terminal_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (!s_mutex) {
        return;
    }

    reset_terminal_state(false);
    s_initialized = true;

    panel_display_fill_screen(HAS_STATUS_BAR ? PANEL_COLOR_BLACK : PANEL_COLOR_WHITE);
    if (HAS_STATUS_BAR && !panel_display_status_region_supported()) {
        draw_status_bar(s_status_address, s_status_data, s_status_bits, s_status_ip);
        s_status_dirty = false;
    }
    panel_display_present();
    if (HAS_STATUS_BAR && panel_display_status_region_supported()) {
        draw_status_bar(s_status_address, s_status_data, s_status_bits, s_status_ip);
        panel_display_status_region_present();
        s_status_dirty = false;
    }
}

void vt100_terminal_putchar(uint8_t c)
{
    if (!s_initialized || !s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    process_byte_locked(c);
    xSemaphoreGive(s_mutex);
}

void vt100_terminal_flush(void)
{
    if (!s_initialized || !s_mutex) return;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        printf("[vt100] flush: mutex take TIMEOUT (held by puchar/set_ip/update_status)\n");
        return;
    }
    uint32_t dirty_rows = s_dirty_rows;
    s_dirty_rows = 0;
    bool status_dirty = s_status_dirty;
    s_status_dirty = false;
    int cursor_col = s_col;
    int cursor_row = s_row;
    bool cursor_visible = s_cursor_visible;
    uint16_t status_address = s_status_address;
    uint8_t status_data = s_status_data;
    uint16_t status_bits = s_status_bits;
    if (dirty_rows) memcpy(s_snapshot, s_buffer, sizeof(s_snapshot));
    memcpy(s_snapshot_ip, s_status_ip, sizeof(s_snapshot_ip));
    xSemaphoreGive(s_mutex);

    if (!dirty_rows && !status_dirty) return;

    extern volatile int g_panel_checkpoint;
    extern volatile uint32_t g_panel_checkpoint_count;
    g_panel_checkpoint = 5; g_panel_checkpoint_count++;  // start render
    int64_t render_start_us = esp_timer_get_time();
    int dirty_count = __builtin_popcount(dirty_rows);

    if (dirty_rows) for (int row = 0; row < VT100_ROWS; row++) {
        if (!(dirty_rows & (1UL << row))) continue;
#if !CONFIG_ALTAIR_DISPLAY_AXS15231B
        /* On AXS the fast draw_cell path paints the cell bg itself, so this
         * full-width row clear would just write every pixel twice. */
        panel_display_fill_rect(0, row * CELL_H, panel_display_width(), CELL_H,
                                HAS_STATUS_BAR ? PANEL_COLOR_BLACK : PANEL_COLOR_WHITE);
#endif
        for (int col = 0; col < VT100_COLS; col++) {
            draw_cell(col, row, s_snapshot[row][col], false);
        }
    }

    if (dirty_rows && cursor_visible && (unsigned)cursor_row < VT100_ROWS && (unsigned)cursor_col < VT100_COLS) {
        terminal_cell_t cursor_cell = s_snapshot[cursor_row][cursor_col];
        draw_cell(cursor_col, cursor_row, cursor_cell, true);
    }

    if (status_dirty && !panel_display_status_region_supported()) {
        draw_status_bar(status_address, status_data, status_bits, s_snapshot_ip);
    }

    int64_t render_dur_us = esp_timer_get_time() - render_start_us;
    (void)render_dur_us;
    (void)dirty_count;

    g_panel_checkpoint = 10; g_panel_checkpoint_count++;  // before present
    static int64_t s_last_present_us = 0;
    int64_t before_us = esp_timer_get_time();
    if (dirty_rows) {
        present_dirty_text_bands(dirty_rows);
        if (HAS_STATUS_BAR && panel_display_status_region_supported()) {
            draw_status_bar(status_address, status_data, status_bits, s_snapshot_ip);
            panel_display_status_region_present();
        }
    } else if (HAS_STATUS_BAR && status_dirty && panel_display_status_region_supported()) {
        draw_status_bar(status_address, status_data, status_bits, s_snapshot_ip);
        panel_display_status_region_present();
    } else if (status_dirty) {
        panel_display_present();
    }
    int64_t after_us = esp_timer_get_time();
    g_panel_checkpoint = 19; g_panel_checkpoint_count++;  // after present
    if (after_us - before_us > 200000) {
        printf("[vt100] present took %lld us (long!)\n", (long long)(after_us - before_us));
    }
    s_last_present_us = after_us;
    (void)s_last_present_us;
}

void vt100_terminal_update_status(uint16_t address, uint8_t data, uint16_t status)
{
    if (!HAS_STATUS_BAR || !s_initialized || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (address != s_status_address || data != s_status_data || status != s_status_bits) {
        s_status_address = address;
        s_status_data = data;
        s_status_bits = status;
        s_status_dirty = true;
    }
    xSemaphoreGive(s_mutex);
}

void vt100_terminal_set_ip(const char *ip_addr, const char *hostname)
{
    (void)hostname;
    if (!HAS_STATUS_BAR || !s_initialized || !s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ip_addr != NULL && ip_addr[0] != '\0') {
        snprintf(s_status_ip, sizeof(s_status_ip), "%s", ip_addr);
    } else {
        s_status_ip[0] = '\0';
    }
    s_status_dirty = true;
    xSemaphoreGive(s_mutex);
}
