/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

#pragma once

#include "intel8080.h"
#include "cpu_state.h"
#include "websocket_console.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Altair command types
typedef enum
{
    NOP = 0,
    EXAMINE = 1,
    EXAMINE_NEXT = 2,
    DEPOSIT = 3,
    DEPOSIT_NEXT = 4,
    SINGLE_STEP = 5,
    DISASSEMBLE = 6,
    TRACE = 7,
    RESET = 8,
    STOP_CMD = 9,
    LOAD_ALTAIR_BASIC = 10,
    RUN_CMD = 11
} ALTAIR_COMMAND;

extern intel8080_t cpu;
extern uint8_t memory[64 * 1024];
extern ALTAIR_COMMAND cmd_switches;

void disassemble(intel8080_t* cpu);
void process_control_panel_commands(void);
void process_virtual_input(const char* command, size_t len);
void publish_cpu_state(char* command, uint16_t address_bus, uint8_t data_bus);
void trace(intel8080_t* cpu);
void altair_reset(void);
