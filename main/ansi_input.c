#include "ansi_input.h"

#define CTRL_KEY(ch) ((ch) & 0x1f)

uint8_t ansi_input_process(uint8_t ch, uint32_t now_ms)
{
    enum
    {
        KEY_STATE_NORMAL = 0,
        KEY_STATE_ESC,
        KEY_STATE_ESC_BRACKET,
        KEY_STATE_ESC_BRACKET_NUM
    };

    static uint8_t key_state = KEY_STATE_NORMAL;
    static uint8_t pending_key = 0;
    static uint32_t esc_start = 0;

    switch (key_state)
    {
        case KEY_STATE_NORMAL:
            if (ch == 0x00)
            {
                return 0x00;
            }
            if (ch == 0x1b)
            {
                key_state = KEY_STATE_ESC;
                esc_start = now_ms;
                return 0x00;
            }
            if (ch == 0x7f || ch == 0x08)
            {
                return (uint8_t)CTRL_KEY('H');
            }
            return ch;

        case KEY_STATE_ESC:
            if (ch == 0x00)
            {
                if ((uint32_t)(now_ms - esc_start) >= ANSI_INPUT_ESC_GRACE_MS)
                {
                    key_state = KEY_STATE_NORMAL;
                    return 0x1b;
                }
                return 0x00;
            }
            if (ch == '[')
            {
                key_state = KEY_STATE_ESC_BRACKET;
                return 0x00;
            }
            key_state = KEY_STATE_NORMAL;
            return ch;

        case KEY_STATE_ESC_BRACKET:
            if (ch == 0x00)
            {
                return 0x00;
            }
            switch (ch)
            {
                case 'A':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('E');
                case 'B':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('X');
                case 'C':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('D');
                case 'D':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('S');
                case '2':
                    pending_key = (uint8_t)CTRL_KEY('O');
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                case '3':
                    pending_key = (uint8_t)CTRL_KEY('G');
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                case '5':
                    pending_key = (uint8_t)CTRL_KEY('R');
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                case '6':
                    pending_key = (uint8_t)CTRL_KEY('V');
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                default:
                    key_state = KEY_STATE_NORMAL;
                    return 0x00;
            }

        case KEY_STATE_ESC_BRACKET_NUM:
            if (ch == 0x00)
            {
                return 0x00;
            }
            key_state = KEY_STATE_NORMAL;
            if (ch == '~')
            {
                uint8_t result = pending_key;
                pending_key = 0;
                return result;
            }
            pending_key = 0;
            return 0x00;
    }

    key_state = KEY_STATE_NORMAL;
    return 0x00;
}
