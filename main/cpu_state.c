#include "cpu_state.h"
#include "intel8080.h"
#include "altair_panel.h"
#include "i8080_disasm.h"
#include "virtual_monitor.h"
#include <ctype.h>
#include <stdio.h>

// Command buffer for CPU_STOPPED mode
#define COMMAND_BUFFER_SIZE 30
static char command_buffer[COMMAND_BUFFER_SIZE] = {0};
static size_t command_buffer_length = 0;

// Global CPU instance
intel8080_t cpu;

volatile CPU_OPERATING_MODE g_cpu_mode = CPU_STOPPED;
uint16_t bus_switches = 0x00;
ALTAIR_COMMAND cmd_switches = NOP;

void cpu_state_set_mode(CPU_OPERATING_MODE mode)
{
    g_cpu_mode = mode;

    // Update front panel display LED based on CPU state
    // Note: Panel update task will reflect this in the display

#ifdef ALTAIR_DEBUG
    // Optional: Add logging for debugging
    const char* mode_str;
    switch (mode)
    {
        case CPU_RUNNING:
            mode_str = "RUNNING";
            break;
        case CPU_STOPPED:
            mode_str = "STOPPED";
            break;
        case CPU_LOW_POWER:
            mode_str = "LOW_POWER";
            break;
        default:
            mode_str = "UNKNOWN";
            break;
    }
    printf("CPU mode set to %s\n", mode_str);
#endif
}

CPU_OPERATING_MODE cpu_state_toggle_mode(void)
{
    memset(command_buffer, 0, sizeof(command_buffer));
    command_buffer_length = 0;

    if (g_cpu_mode == CPU_RUNNING)
    {
        cpu_state_set_mode(CPU_STOPPED);
    }
    else
    {
        cpu_state_set_mode(CPU_RUNNING);
    }

#ifdef ALTAIR_DEBUG
    printf("CPU mode toggled to %s\n", (g_cpu_mode == CPU_RUNNING) ? "RUNNING" : "STOPPED");
#endif

    if (g_cpu_mode == CPU_STOPPED)
    {
        // Prompt for CPU monitor
        const char* prompt = "\r\n*** CPU STOPPED ***\r\nCPU MONITOR> ";
        publish_message(prompt, strlen(prompt));
    }

    return g_cpu_mode;
}

void process_control_panel_commands_char(uint8_t ch)
{
    if (ch == '\r')
    {
        // Return key pressed - process the accumulated command
        command_buffer[command_buffer_length] = '\0';

        // Process the command (even if empty)
        process_virtual_input(command_buffer, command_buffer_length);

        // Reset the command buffer
        command_buffer_length = 0;
        command_buffer[0] = '\0';
    }
    else if (ch == 8) // Backspace
    {
        if (command_buffer_length > 0)
        {
            command_buffer_length--;
            websocket_console_enqueue_output(ch);  // Echo backspace
            websocket_console_enqueue_output(' '); // Echo space to erase character
            websocket_console_enqueue_output(ch);  // Echo backspace again
        }
        // If command_buffer_length == 0, do nothing (don't echo)
    }
    else
    {
        // Accumulate characters into the command buffer
        if (command_buffer_length < COMMAND_BUFFER_SIZE - 1)
        {
            // Convert to uppercase and add to buffer
            command_buffer[command_buffer_length++] = (char)toupper((unsigned char)ch);

            // Echo the character back to the terminal
            websocket_console_enqueue_output(ch);
        }
    }
}