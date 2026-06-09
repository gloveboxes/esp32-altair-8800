// Z80 CPU adapter.
//
// The rest of the Altair emulator (front panel, main loop, host tests) talks to
// the CPU through the C API declared in intel8080.h: i8080_reset, i8080_cycle,
// i8080_examine/deposit, etc., reading register/bus state back out of an
// intel8080_t struct.
//
// This file implements that exact API on top of the ntvcm 8080/Z80 core in
// x80.cxx, running it in Z80 mode. The core keeps its CPU state in the global
// `reg` and shares the global `memory[]` with memory.c, so the adapter syncs
// `reg` back into the caller's intel8080_t after each step and supplies the
// I/O / halt / hook callbacks the core calls out to.
//
// This adapter (plus x80.cxx) is the project's single CPU core; the front
// panel and main loop talk only to the i8080_* API declared in intel8080.h.

extern "C"
{
#include "intel8080.h"
}

#include <cstddef>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "x80.hxx"
#include <djltrace.hxx>

// Front-panel status LED bits.
#define STATUS_MEMORY_READ   0x80
#define STATUS_PORT_INPUT    0x40
#define STATUS_OP_CODE_FETCH 0x20
#define STATUS_PORT_OUTPUT   0x10
#define STATUS_HALT          0x08
#define STATUS_STACK         0x04
#define STATUS_WRITE_OUTPUT  0x02
#define STATUS_INTERRUPT     0x01

// No-op tracer instance required by x80.cxx (instruction tracing stays off).
CDJLTrace tracer;

// The x80 core uses a single global CPU state, so the adapter likewise tracks a
// single active CPU instance. Its term/disk/sense/io callbacks are read back
// out of this struct from the I/O callbacks below.
static intel8080_t *g_cpu = NULL;

// Copy the x80 core's live registers (global `reg`) into the caller's struct so
// the front panel and host tests observe up-to-date register and flag state.
static void sync_regs_out(intel8080_t *cpu)
{
    cpu->registers.a = reg.a;
    cpu->registers.flags = reg.materializeFlags();
    cpu->registers.b = reg.b;
    cpu->registers.c = reg.c;
    cpu->registers.d = reg.d;
    cpu->registers.e = reg.e;
    cpu->registers.h = reg.h;
    cpu->registers.l = reg.l;
    cpu->registers.sp = reg.sp;
    cpu->registers.pc = reg.pc;
}

#ifdef ALTAIR_X80_HOST_TEST
// Host-test builds drive the CPU through a CP/M BDOS trap that pokes pc/sp (and
// occasionally other registers) straight into the intel8080_t between steps.
// Mirror those edits back into the core's global `reg` before each instruction.
// The production emulator never needs this (front-panel edits go through
// i8080_examine, which updates `reg` directly), so it stays out of the hot path.
static void sync_regs_in(intel8080_t *cpu)
{
    reg.a = cpu->registers.a;
    reg.b = cpu->registers.b;
    reg.c = cpu->registers.c;
    reg.d = cpu->registers.d;
    reg.e = cpu->registers.e;
    reg.h = cpu->registers.h;
    reg.l = cpu->registers.l;
    reg.sp = cpu->registers.sp;
    reg.pc = cpu->registers.pc;
    reg.f = cpu->registers.flags;
    reg.unmaterializeFlags();
}
#endif

static void update_display_bus(intel8080_t *cpu)
{
    cpu->display_address_bus = cpu->address_bus;
    cpu->display_data_bus = cpu->data_bus;
    cpu->display_cpuStatus = cpu->cpuStatus;
}

extern "C" void i8080_reset(intel8080_t *cpu, port_in in, port_out out, read_sense_switches sense,
                            disk_controller_t *disk_controller, io_port_in_fn io_in, io_port_out_fn io_out)
{
    memset(cpu, 0, sizeof(intel8080_t));
    cpu->term_in = in;
    cpu->term_out = out;
    cpu->io_port_in_handler = io_in;
    cpu->io_port_out_handler = io_out;
    cpu->disk_controller = *disk_controller;
    cpu->sense = sense;
    cpu->cpuStatus = 0x00;
    cpu->halted = false;
    cpu->iff = false;

    g_cpu = cpu;

    // Zero-initialize the core's registers, then select the instruction set.
    // Host-test builds run the 8080 instruction set so the standard 8080
    // diagnostic ROMs exercise the same core the firmware ships; the emulator
    // itself defaults to Z80. The boot loader address is loaded subsequently
    // via i8080_examine().
    reg = registers();
#if !defined( X80_FORCE_Z80 ) && !defined( X80_FORCE_8080 )
    // Mode is not fixed at compile time, so choose the instruction set now.
    // (When X80_FORCE_Z80/X80_FORCE_8080 is defined, reg.fZ80Mode is a static
    // constexpr and must not be assigned -- the constant already selects the
    // core, and every mode check folds away.)
#ifdef ALTAIR_X80_HOST_TEST
    reg.fZ80Mode = false;
#else
    reg.fZ80Mode = true;
#endif
#endif
    reg.pc = 0;

    sync_regs_out(cpu);
}

void i8080_resume(intel8080_t *cpu)
{
    cpu->halted = false;
    cpu->cpuStatus &= ~STATUS_HALT;
}

void i8080_cycle(intel8080_t *cpu)
{
    if (cpu->halted)
    {
        // Keep HLTA asserted while halted, matching real-8080 behavior.
        cpu->cpuStatus = STATUS_HALT;
        cpu->display_cpuStatus = STATUS_HALT;
        return;
    }

    g_cpu = cpu;

#ifdef ALTAIR_X80_HOST_TEST
    sync_regs_in(cpu);
#endif

    // Run a single instruction. Every opcode costs at least 4 cycles, so a
    // budget of 1 executes exactly one instruction.
    x80_emulate(1);

    sync_regs_out(cpu);

    // Present the next fetch on the bus for the front panel.
    cpu->address_bus = reg.pc;
    cpu->data_bus = memory[reg.pc];
    cpu->cpuStatus = STATUS_MEMORY_READ | STATUS_OP_CODE_FETCH;
    update_display_bus(cpu);
}

void i8080_examine(intel8080_t *cpu, uint16_t address)
{
    // Jumping to a new PC from the front panel also resumes a halted CPU.
    i8080_resume(cpu);
    reg.pc = address;
    cpu->registers.pc = address;
    cpu->address_bus = address;
    cpu->cpuStatus = STATUS_MEMORY_READ;
    cpu->data_bus = memory[address];
    update_display_bus(cpu);
}

void i8080_examine_next(intel8080_t *cpu)
{
    cpu->address_bus++;
    cpu->cpuStatus = STATUS_MEMORY_READ;
    cpu->data_bus = memory[cpu->address_bus];
    update_display_bus(cpu);
}

void i8080_deposit(intel8080_t *cpu, uint8_t data)
{
    cpu->data_bus = data;
    cpu->cpuStatus &= ~STATUS_MEMORY_READ;
    cpu->cpuStatus |= STATUS_WRITE_OUTPUT;
    memory[cpu->address_bus] = data;
    update_display_bus(cpu);
}

void i8080_deposit_next(intel8080_t *cpu, uint8_t data)
{
    i8080_examine_next(cpu);
    cpu->data_bus = data;
    cpu->cpuStatus &= ~STATUS_MEMORY_READ;
    cpu->cpuStatus |= STATUS_WRITE_OUTPUT;
    memory[cpu->address_bus] = data;
    update_display_bus(cpu);
}

// --- x80 core callbacks ---------------------------------------------------

// 0x64 (mov h,h) is repurposed as a hook opcode by the ntvcm core. On the
// Altair it is a genuine (no-op) instruction, so report it as a NOP. These
// callbacks are invoked from x80.cxx, so they keep its (C++) linkage.
uint8_t x80_invoke_hook(void)
{
    return OPCODE_NOP;
}

void x80_invoke_halt(void)
{
    if (g_cpu)
        g_cpu->halted = true;
}

// IN d8. Implements the Altair port map, leaving the input
// byte in the accumulator (reg.a).
void x80_invoke_in(uint8_t port)
{
    static uint8_t character = 0;
    intel8080_t *cpu = g_cpu;

    switch (port)
    {
    case 0x00:
        reg.a = 0x00;
        break;
    case 0x01:
        reg.a = cpu->term_in();
        break;
    case 0x08:
        reg.a = cpu->disk_controller.disk_status();
        break;
    case 0x09:
        reg.a = cpu->disk_controller.sector();
        break;
    case 0x0a:
        reg.a = cpu->disk_controller.read();
        break;
    case 0x10: // 2SIO port 1, status
        reg.a = 0x2; // transmit buffer empty
        if (!character)
            character = cpu->term_in();
        if (character)
            reg.a |= 0x1;
        break;
    case 0x11: // 2SIO port 1, read
        if (character)
        {
            reg.a = character;
            character = 0;
        }
        else
        {
            reg.a = cpu->term_in();
        }
        break;
    case 0xff: // Front panel switches
        reg.a = cpu->sense();
        break;
    default:
        reg.a = cpu->io_port_in_handler(port);
        break;
    }
}

// OUT d8. Implements the Altair port map, taking the output
// byte from the accumulator (reg.a).
void x80_invoke_out(uint8_t port)
{
    intel8080_t *cpu = g_cpu;

    switch (port)
    {
    case 0x01:
        cpu->term_out(reg.a);
        break;
    case 0x08:
        cpu->disk_controller.disk_select(reg.a);
        break;
    case 0x09:
        cpu->disk_controller.disk_function(reg.a);
        break;
    case 0x0a:
        cpu->disk_controller.write(reg.a);
        break;
    case 0x10: // 2SIO port 1 control
        break;
    case 0x11: // 2SIO port 1 write
        cpu->term_out(reg.a);
        break;
    default:
        cpu->io_port_out_handler(port, reg.a);
        break;
    }
}

void x80_hard_exit(const char *pcerror, uint8_t arg1, uint8_t arg2)
{
    fprintf(stderr, pcerror, arg1, arg2);
    exit(1);
}
