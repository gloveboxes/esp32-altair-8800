#include "altair_splash.h"

#if CONFIG_ALTAIR_DISPLAY_AXS15231B

#include <stdint.h>
#include "vt100_terminal.h"

// ASCII splash inspired by the original MITS Altair 8800 front panel.
// Waveshare 3.5" AXS15231B colour VT100 display only.
//
// Chassis is rendered using blue background blocks (ANSI bg 44) — like the
// breakout demo's chequer-bordered playfield — instead of '#' characters.
// Layout: 30 rows x 80 cols.
//   rows  1- 2 : solid blue top bar
//   rows  3-28 : 2-col blue left edge + 76-col content + 2-col blue right edge
//   rows 29-30 : solid blue bottom bar
// All content lines below are padded to exactly 76 visible columns. ANSI SGR
// escapes do not occupy cells in the VT100 buffer.
//
// Colour key: bg 44 = blue (case), fg 37 = grey (panel labels),
//             fg 91 = bright red (LEDs), fg 93 = yellow (toggles),
//             1;97 = bold white (badge), 0 = reset.
void altair_splash_show(void)
{
    static const char *const top_bar =
        "\x1b[44m                                                                                \x1b[0m";
    static const char *const edge_l = "\x1b[44m  \x1b[0m";
    static const char *const edge_r = "\x1b[44m  \x1b[0m\r\n";

    /* 76-col content rows. Each row's visible width is exactly 76. */
    static const char *const content[] = {

        /* blank */
        "                                                                            ",
        /* Title: 'A L T A I R   8 8 0 0       E S P 3 2 - S 3' (42 chars, 17 left + 17 right). */
        "                 \x1b[1;97mA L T A I R   8 8 0 0       E S P 3 2 - S 3\x1b[0m                ",
        /* blank */
        "                                                                            ",
        /* blank */
        "                                                                            ",
        /* STATUS labels: 10 LEDs in 7-col slots.
         * Stars sit at cols 6,13,20,27,34,41,48,55,62,69 of the 76-char row. */
        "\x1b[37m     INTE   PROT   MEMR   INP     MI    OUT    HLTA  STACK    WO    INT     \x1b[0m",
        "\x1b[91m      *      *      *      *      *      *      *      *      *      *      \x1b[0m",
        /* blank */
        "                                                                            ",
        /* blank */
        "                                                                            ",
        /* Address bus shifted right by 3 cols so WAIT/HLDA sit under INTE/PROT
         * region and the address LEDs progress evenly to the right edge. */
        "\x1b[37m  WAIT HLDA  A15 A14 A13 A12 A11 A10 A9  A8  A7  A6  A5  A4  A3  A2  A1  A0 \x1b[0m",
        "\x1b[91m    *    *    *   *   *   *   *   *   *   *   *   *   *   *   *   *   *   * \x1b[0m",
        /* blank */
        "                                                                            ",
        /* blank */
        "\x1b[37m                                                                            \x1b[0m",
        /* Data bus: D7..D0 centred. */
        "\x1b[37m                      D7  D6  D5  D4  D3  D2  D1  D0                        \x1b[0m",
        "\x1b[91m                       *   *   *   *   *   *   *   *                        \x1b[0m",
        /* blank */
        "                                                                            ",
        /* blank */
        "                                                                            ",
        /* Sense switches (16 yellow toggles aligned with addr LEDs above). */
        "\x1b[93m        \\   \\   \\   \\   \\   \\   \\   \\   \\   \\   \\   \\   \\   \\   \\   \\       \x1b[0m",
        /* blank */
        "                                                                            ",
        /* blank */
        "                                                                            ",
        /* Switch row labels. */
        "           \x1b[37mSTOP  STEP  EXAMINE  DEPOSIT  RESET  PROTECT    AUX  AUX\x1b[0m         ",
        "           \x1b[37mRUN         NEXT     NEXT       CLR  UNPROTECT             \x1b[0m      ",
        /* Yellow paddle toggles below each switch label. */
        "            \x1b[93m\\     \\       \\        \\       \\         \\      \\    \\   \x1b[0m       ",
        /* blank */
        "                                                                            ",
        /* blank */
        "                                                                            ",
        /* "Booting..." centered (10 chars, 33 left + 33 right). */
        "                                 \x1b[97mBooting...\x1b[0m                                 ",
        /* blank spacer below Booting */
        "                                                                            ",
    };
    const int n_content = sizeof(content) / sizeof(content[0]);

    /* Hide the cursor so its block doesn't sit on the splash. */
    static const char *hide_cursor = "\x1b[?25l";
    for (const char *p = hide_cursor; *p; ++p)
        vt100_terminal_putchar((uint8_t)*p);

    /* Emit top blue bar (2 rows). */
    for (int i = 0; i < 2; ++i)
    {
        for (const char *p = top_bar; *p; ++p)
        {
            vt100_terminal_putchar((uint8_t)*p);
        }
        vt100_terminal_putchar('\r');
        vt100_terminal_putchar('\n');
    }

    /* Emit content rows wrapped in blue side edges. */
    for (int row = 0; row < n_content; ++row)
    {
        for (const char *p = edge_l; *p; ++p)
            vt100_terminal_putchar((uint8_t)*p);
        for (const char *p = content[row]; *p; ++p)
            vt100_terminal_putchar((uint8_t)*p);
        for (const char *p = edge_r; *p; ++p)
            vt100_terminal_putchar((uint8_t)*p);
    }

    /* Emit bottom blue bar — 2 rows. Newline after the first; no trailing
     * newline after the last so the terminal does not scroll a blank line +
     * cursor onto the bottom edge. */
    for (const char *p = top_bar; *p; ++p)
        vt100_terminal_putchar((uint8_t)*p);
    vt100_terminal_putchar('\r');
    vt100_terminal_putchar('\n');
    for (const char *p = top_bar; *p; ++p)
    {
        vt100_terminal_putchar((uint8_t)*p);
    }

    /* Reset SGR state before any subsequent writes. */
    const char *reset = "\x1b[0m";
    while (*reset)
        vt100_terminal_putchar((uint8_t)*reset++);
}

#endif /* CONFIG_ALTAIR_DISPLAY_AXS15231B */
