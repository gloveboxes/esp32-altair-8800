#ifndef CPU_STATE_H
#define CPU_STATE_H

#include <stdint.h>
#include "intel8080.h"

typedef enum
{
    CPU_RUNNING = 1,
    CPU_STOPPED = 2,
    CPU_LOW_POWER = 3
} CPU_OPERATING_MODE;

// Global CPU instance
extern intel8080_t cpu;

// Bus switches state
extern uint16_t bus_switches;

// Set the CPU operating mode
void cpu_state_set_mode(CPU_OPERATING_MODE mode);

// Toggle the CPU operating mode between RUNNING and STOPPED
CPU_OPERATING_MODE cpu_state_toggle_mode(void);

// Process a single character for CPU monitor commands in STOPPED mode
void process_control_panel_commands_char(uint8_t ch);

// Inline wrapper for high-performance reads
static inline CPU_OPERATING_MODE cpu_state_get_mode(void)
{
    extern volatile CPU_OPERATING_MODE g_cpu_mode;
    return g_cpu_mode;
}

#endif // CPU_STATE_H
