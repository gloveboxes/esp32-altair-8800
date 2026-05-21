#include "intel8080.h"
#include "op_codes.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ARDUINO
	#include <Arduino.h>
	#include <SPI.h>
#endif
#ifdef WIN32
	#include <windows.h>
#endif

#include "memory.h"

// Performance optimization macros
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CHECK_CARRY(a, b) ((a + b) > 0xff)
#define CHECK_HALF_CARRY(a, b) (((a & 0xf) + (b & 0xf)) > 0xf)

// define CPU stats LEDs
#define STATUS_MEMORY_READ		0x80
#define STATUS_PORT_INPUT		0x40
#define STATUS_OP_CODE_FETCH	0x20
#define STATUS_PORT_OUTPUT		0x10
#define STATUS_HALT				0x08
#define STATUS_STACK			0x04
#define STATUS_WRITE_OUTPUT		0x02	// inverted!
#define STATUS_INTERRUPT		0x01

// Fast parity lookup table
static const uint8_t parity_table[256] = {
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

static inline uint8_t get_parity_fast(uint8_t val)
{
	return parity_table[val];
}

uint8_t get_parity(uint8_t val)
{
	return get_parity_fast(val);
}

// Forward declarations for jump table
static uint8_t i8080_nop(intel8080_t *cpu);
static uint8_t i8080_lxi(intel8080_t *cpu);
static uint8_t i8080_stax(intel8080_t *cpu);
static uint8_t i8080_inx(intel8080_t *cpu);
static uint8_t i8080_inr(intel8080_t *cpu);
static uint8_t i8080_dcr(intel8080_t *cpu);
static uint8_t i8080_mvi(intel8080_t *cpu);
static uint8_t i8080_rlc(intel8080_t *cpu);
static uint8_t i8080_dad(intel8080_t *cpu);
static uint8_t i8080_ldax(intel8080_t *cpu);
static uint8_t i8080_dcx(intel8080_t *cpu);
static uint8_t i8080_rrc(intel8080_t *cpu);
static uint8_t i8080_ral(intel8080_t *cpu);
static uint8_t i8080_rar(intel8080_t *cpu);
static uint8_t i8080_shld(intel8080_t *cpu);
static uint8_t i8080_daa(intel8080_t *cpu);
static uint8_t i8080_lhld(intel8080_t *cpu);
static uint8_t i8080_cma(intel8080_t *cpu);
static uint8_t i8080_lda(intel8080_t *cpu);
static uint8_t i8080_sta(intel8080_t *cpu);
static uint8_t i8080_stc(intel8080_t *cpu);
static uint8_t i8080_cmc(intel8080_t *cpu);
static uint8_t i8080_mov(intel8080_t *cpu);
static uint8_t i8080_add(intel8080_t *cpu);
static uint8_t i8080_adc(intel8080_t *cpu);
static uint8_t i8080_sub(intel8080_t *cpu);
static uint8_t i8080_sbb(intel8080_t *cpu);
static uint8_t i8080_ana(intel8080_t *cpu);
static uint8_t i8080_xra(intel8080_t *cpu);
static uint8_t i8080_ora(intel8080_t *cpu);
static uint8_t i8080_cmp(intel8080_t *cpu);
static uint8_t i8080_rccc(intel8080_t *cpu);
static uint8_t i8080_pop(intel8080_t *cpu);
static uint8_t i8080_jccc(intel8080_t *cpu);
static uint8_t i8080_jmp(intel8080_t *cpu);
static uint8_t i8080_cccc(intel8080_t *cpu);
static uint8_t i8080_push(intel8080_t *cpu);
static uint8_t i8080_adi(intel8080_t *cpu);
static uint8_t i8080_rst(intel8080_t *cpu);
static uint8_t i8080_ret(intel8080_t *cpu);
static uint8_t i8080_call(intel8080_t *cpu);
static uint8_t i8080_aci(intel8080_t *cpu);
static uint8_t i8080_out(intel8080_t *cpu);
static uint8_t i8080_sui(intel8080_t *cpu);
static uint8_t i8080_in(intel8080_t *cpu);
static uint8_t i8080_sbi(intel8080_t *cpu);
static uint8_t i8080_xthl(intel8080_t *cpu);
static uint8_t i8080_ani(intel8080_t *cpu);
static uint8_t i8080_pchl(intel8080_t *cpu);
static uint8_t i8080_xchg(intel8080_t *cpu);
static uint8_t i8080_xri(intel8080_t *cpu);
static uint8_t i8080_di(intel8080_t *cpu);
static uint8_t i8080_ori(intel8080_t *cpu);
static uint8_t i8080_sphl(intel8080_t *cpu);
static uint8_t i8080_ei(intel8080_t *cpu);
static uint8_t i8080_cpi(intel8080_t *cpu);
static uint8_t i8080_hlt(intel8080_t *cpu);

// Jump table for fast opcode dispatch
static uint8_t (*const opcode_handlers[256])(intel8080_t *cpu) = {
	[0x00] = i8080_nop,    [0x01] = i8080_lxi,    [0x02] = i8080_stax,   [0x03] = i8080_inx,
	[0x04] = i8080_inr,    [0x05] = i8080_dcr,    [0x06] = i8080_mvi,    [0x07] = i8080_rlc,
	[0x08] = NULL,         [0x09] = i8080_dad,    [0x0a] = i8080_ldax,   [0x0b] = i8080_dcx,
	[0x0c] = i8080_inr,    [0x0d] = i8080_dcr,    [0x0e] = i8080_mvi,    [0x0f] = i8080_rrc,
	[0x10] = NULL,         [0x11] = i8080_lxi,    [0x12] = i8080_stax,   [0x13] = i8080_inx,
	[0x14] = i8080_inr,    [0x15] = i8080_dcr,    [0x16] = i8080_mvi,    [0x17] = i8080_ral,
	[0x18] = NULL,         [0x19] = i8080_dad,    [0x1a] = i8080_ldax,   [0x1b] = i8080_dcx,
	[0x1c] = i8080_inr,    [0x1d] = i8080_dcr,    [0x1e] = i8080_mvi,    [0x1f] = i8080_rar,
	[0x20] = NULL,         [0x21] = i8080_lxi,    [0x22] = i8080_shld,   [0x23] = i8080_inx,
	[0x24] = i8080_inr,    [0x25] = i8080_dcr,    [0x26] = i8080_mvi,    [0x27] = i8080_daa,
	[0x28] = NULL,         [0x29] = i8080_dad,    [0x2a] = i8080_lhld,   [0x2b] = i8080_dcx,
	[0x2c] = i8080_inr,    [0x2d] = i8080_dcr,    [0x2e] = i8080_mvi,    [0x2f] = i8080_cma,
	[0x30] = NULL,         [0x31] = i8080_lxi,    [0x32] = i8080_sta,    [0x33] = i8080_inx,
	[0x34] = i8080_inr,    [0x35] = i8080_dcr,    [0x36] = i8080_mvi,    [0x37] = i8080_stc,
	[0x38] = NULL,         [0x39] = i8080_dad,    [0x3a] = i8080_lda,    [0x3b] = i8080_dcx,
	[0x3c] = i8080_inr,    [0x3d] = i8080_dcr,    [0x3e] = i8080_mvi,    [0x3f] = i8080_cmc,
	[0x40] = i8080_mov,    [0x41] = i8080_mov,    [0x42] = i8080_mov,    [0x43] = i8080_mov,
	[0x44] = i8080_mov,    [0x45] = i8080_mov,    [0x46] = i8080_mov,    [0x47] = i8080_mov,
	[0x48] = i8080_mov,    [0x49] = i8080_mov,    [0x4a] = i8080_mov,    [0x4b] = i8080_mov,
	[0x4c] = i8080_mov,    [0x4d] = i8080_mov,    [0x4e] = i8080_mov,    [0x4f] = i8080_mov,
	[0x50] = i8080_mov,    [0x51] = i8080_mov,    [0x52] = i8080_mov,    [0x53] = i8080_mov,
	[0x54] = i8080_mov,    [0x55] = i8080_mov,    [0x56] = i8080_mov,    [0x57] = i8080_mov,
	[0x58] = i8080_mov,    [0x59] = i8080_mov,    [0x5a] = i8080_mov,    [0x5b] = i8080_mov,
	[0x5c] = i8080_mov,    [0x5d] = i8080_mov,    [0x5e] = i8080_mov,    [0x5f] = i8080_mov,
	[0x60] = i8080_mov,    [0x61] = i8080_mov,    [0x62] = i8080_mov,    [0x63] = i8080_mov,
	[0x64] = i8080_mov,    [0x65] = i8080_mov,    [0x66] = i8080_mov,    [0x67] = i8080_mov,
	[0x68] = i8080_mov,    [0x69] = i8080_mov,    [0x6a] = i8080_mov,    [0x6b] = i8080_mov,
	[0x6c] = i8080_mov,    [0x6d] = i8080_mov,    [0x6e] = i8080_mov,    [0x6f] = i8080_mov,
	[0x70] = i8080_mov,    [0x71] = i8080_mov,    [0x72] = i8080_mov,    [0x73] = i8080_mov,
	[0x74] = i8080_mov,    [0x75] = i8080_mov,    [0x76] = i8080_hlt,    [0x77] = i8080_mov,
	[0x78] = i8080_mov,    [0x79] = i8080_mov,    [0x7a] = i8080_mov,    [0x7b] = i8080_mov,
	[0x7c] = i8080_mov,    [0x7d] = i8080_mov,    [0x7e] = i8080_mov,    [0x7f] = i8080_mov,
	[0x80] = i8080_add,    [0x81] = i8080_add,    [0x82] = i8080_add,    [0x83] = i8080_add,
	[0x84] = i8080_add,    [0x85] = i8080_add,    [0x86] = i8080_add,    [0x87] = i8080_add,
	[0x88] = i8080_adc,    [0x89] = i8080_adc,    [0x8a] = i8080_adc,    [0x8b] = i8080_adc,
	[0x8c] = i8080_adc,    [0x8d] = i8080_adc,    [0x8e] = i8080_adc,    [0x8f] = i8080_adc,
	[0x90] = i8080_sub,    [0x91] = i8080_sub,    [0x92] = i8080_sub,    [0x93] = i8080_sub,
	[0x94] = i8080_sub,    [0x95] = i8080_sub,    [0x96] = i8080_sub,    [0x97] = i8080_sub,
	[0x98] = i8080_sbb,    [0x99] = i8080_sbb,    [0x9a] = i8080_sbb,    [0x9b] = i8080_sbb,
	[0x9c] = i8080_sbb,    [0x9d] = i8080_sbb,    [0x9e] = i8080_sbb,    [0x9f] = i8080_sbb,
	[0xa0] = i8080_ana,    [0xa1] = i8080_ana,    [0xa2] = i8080_ana,    [0xa3] = i8080_ana,
	[0xa4] = i8080_ana,    [0xa5] = i8080_ana,    [0xa6] = i8080_ana,    [0xa7] = i8080_ana,
	[0xa8] = i8080_xra,    [0xa9] = i8080_xra,    [0xaa] = i8080_xra,    [0xab] = i8080_xra,
	[0xac] = i8080_xra,    [0xad] = i8080_xra,    [0xae] = i8080_xra,    [0xaf] = i8080_xra,
	[0xb0] = i8080_ora,    [0xb1] = i8080_ora,    [0xb2] = i8080_ora,    [0xb3] = i8080_ora,
	[0xb4] = i8080_ora,    [0xb5] = i8080_ora,    [0xb6] = i8080_ora,    [0xb7] = i8080_ora,
	[0xb8] = i8080_cmp,    [0xb9] = i8080_cmp,    [0xba] = i8080_cmp,    [0xbb] = i8080_cmp,
	[0xbc] = i8080_cmp,    [0xbd] = i8080_cmp,    [0xbe] = i8080_cmp,    [0xbf] = i8080_cmp,
	[0xc0] = i8080_rccc,   [0xc1] = i8080_pop,    [0xc2] = i8080_jccc,   [0xc3] = i8080_jmp,
	[0xc4] = i8080_cccc,   [0xc5] = i8080_push,   [0xc6] = i8080_adi,    [0xc7] = i8080_rst,
	[0xc8] = i8080_rccc,   [0xc9] = i8080_ret,    [0xca] = i8080_jccc,   [0xcb] = NULL,
	[0xcc] = i8080_cccc,   [0xcd] = i8080_call,   [0xce] = i8080_aci,    [0xcf] = i8080_rst,
	[0xd0] = i8080_rccc,   [0xd1] = i8080_pop,    [0xd2] = i8080_jccc,   [0xd3] = i8080_out,
	[0xd4] = i8080_cccc,   [0xd5] = i8080_push,   [0xd6] = i8080_sui,    [0xd7] = i8080_rst,
	[0xd8] = i8080_rccc,   [0xd9] = NULL,         [0xda] = i8080_jccc,   [0xdb] = i8080_in,
	[0xdc] = i8080_cccc,   [0xdd] = NULL,         [0xde] = i8080_sbi,    [0xdf] = i8080_rst,
	[0xe0] = i8080_rccc,   [0xe1] = i8080_pop,    [0xe2] = i8080_jccc,   [0xe3] = i8080_xthl,
	[0xe4] = i8080_cccc,   [0xe5] = i8080_push,   [0xe6] = i8080_ani,    [0xe7] = i8080_rst,
	[0xe8] = i8080_rccc,   [0xe9] = i8080_pchl,   [0xea] = i8080_jccc,   [0xeb] = i8080_xchg,
	[0xec] = i8080_cccc,   [0xed] = NULL,         [0xee] = i8080_xri,    [0xef] = i8080_rst,
	[0xf0] = i8080_rccc,   [0xf1] = i8080_pop,    [0xf2] = i8080_jccc,   [0xf3] = i8080_di,
	[0xf4] = i8080_cccc,   [0xf5] = i8080_push,   [0xf6] = i8080_ori,    [0xf7] = i8080_rst,
	[0xf8] = i8080_rccc,   [0xf9] = i8080_sphl,   [0xfa] = i8080_jccc,   [0xfb] = i8080_ei,
	[0xfc] = i8080_cccc,   [0xfd] = NULL,         [0xfe] = i8080_cpi,    [0xff] = i8080_rst
};

void i8080_reset(intel8080_t *cpu, port_in in, port_out out, read_sense_switches sense,
			 disk_controller_t *disk_controller, io_port_in_fn io_in, io_port_out_fn io_out)
{
	memset(cpu, 0, sizeof(intel8080_t));
	cpu->term_in = in;
	cpu->term_out = out;
	cpu->io_port_in_handler = io_in;
	cpu->io_port_out_handler = io_out;
	cpu->disk_controller = *disk_controller;
	cpu->registers.flags = 0x2;
	cpu->sense = sense;
	cpu->cpuStatus = 0x00;
	cpu->halted = false;
	cpu->iff = false;
}

static inline void i8080_update_flag_bit(intel8080_t *cpu, uint8_t flag, int condition)
{
	if (LIKELY(condition)) {
		cpu->registers.flags |= flag;
	} else {
		cpu->registers.flags &= ~flag;
	}
}

static inline void i8080_set_flag(intel8080_t *cpu, uint8_t flag)
{
	cpu->registers.flags |= flag;
}

static inline void i8080_clear_flag(intel8080_t *cpu, uint8_t flag)
{
	cpu->registers.flags &= ~flag;
}

static inline void i8080_mwrite(intel8080_t *cpu)
{
	cpu->cpuStatus &= ~(STATUS_MEMORY_READ);
	cpu->cpuStatus |= STATUS_WRITE_OUTPUT;
	write8(cpu->address_bus, cpu->data_bus);
}

static inline void i8080_mread(intel8080_t *cpu)
{
	cpu->cpuStatus |= STATUS_MEMORY_READ;
	cpu->data_bus = read8(cpu->address_bus);
}

/* Read a little-endian 16-bit value via the bus, leaving address_bus/data_bus
   reflecting the second (high-byte) access. */
static inline uint16_t i8080_mread16(intel8080_t *cpu, uint16_t addr)
{
	cpu->cpuStatus |= STATUS_MEMORY_READ;
	cpu->address_bus = addr;
	uint8_t lo = read8(addr);
	cpu->address_bus = addr + 1;
	uint8_t hi = read8(addr + 1);
	cpu->data_bus = hi;
	return ((uint16_t)hi << 8) | lo;
}

/* Write a little-endian 16-bit value via the bus, leaving address_bus/data_bus
   reflecting the second (high-byte) access. */
static inline void i8080_mwrite16(intel8080_t *cpu, uint16_t addr, uint16_t val)
{
	cpu->cpuStatus &= ~STATUS_MEMORY_READ;
	cpu->cpuStatus |= STATUS_WRITE_OUTPUT;
	cpu->address_bus = addr;
	cpu->data_bus = (uint8_t)(val & 0xff);
	write8(addr, cpu->data_bus);
	cpu->address_bus = addr + 1;
	cpu->data_bus = (uint8_t)((val >> 8) & 0xff);
	write8(addr + 1, cpu->data_bus);
}

static inline void i8080_pairwrite(intel8080_t *cpu, uint8_t pair, uint16_t val)
{
	/* Register-pair writes are internal CPU operations and do not
	   touch the memory bus, so STATUS_MEMORY_READ is left alone. */
	switch(pair)
	{
	case PAIR_BC:
		cpu->registers.bc = val;
		break;
	case PAIR_DE:
		cpu->registers.de = val;
		break;
	case PAIR_HL:
		cpu->registers.hl = val;
		break;
	case PAIR_SP:
		cpu->registers.sp = val;
		break;
	}
}

static inline uint16_t i8080_pairread(intel8080_t *cpu, uint8_t pair)
{
	/* Register-pair reads are internal and do not assert MEMR. */
	switch(pair)
	{
	case PAIR_BC:
		return cpu->registers.bc;
	case PAIR_DE:
		return cpu->registers.de;
	case PAIR_HL:
		return cpu->registers.hl;
	case PAIR_SP:
		return cpu->registers.sp;
	default:
		return 0;
	}
}

static inline void i8080_regwrite(intel8080_t *cpu, uint8_t reg, uint8_t val)
{
	switch(reg)
	{
	case REGISTER_A:
		cpu->registers.a = val;
		break;
	case REGISTER_B:
		cpu->registers.b = val;
		break;
	case REGISTER_C:
		cpu->registers.c = val;
		break;
	case REGISTER_D:
		cpu->registers.d = val;
		break;
	case REGISTER_E:
		cpu->registers.e = val;
		break;
	case REGISTER_H:
		cpu->registers.h = val;
		break;
	case REGISTER_L:
		cpu->registers.l = val;
		break;
	case MEMORY_ACCESS:
		cpu->address_bus = cpu->registers.hl;
		cpu->data_bus = val;
		i8080_mwrite(cpu);
		break;
	}
}

static inline uint8_t i8080_regread(intel8080_t *cpu, uint8_t reg)
{
	switch(reg)
	{
	case REGISTER_A:
		return cpu->registers.a;
	case REGISTER_B:
		return cpu->registers.b;
	case REGISTER_C:
		return cpu->registers.c;
	case REGISTER_D:
		return cpu->registers.d;
	case REGISTER_E:
		return cpu->registers.e;
	case REGISTER_H:
		return cpu->registers.h;
	case REGISTER_L:
		return cpu->registers.l;
	case MEMORY_ACCESS:
		cpu->address_bus = cpu->registers.hl;
		i8080_mread(cpu);
		return cpu->data_bus;
	default:
		return 0;
	}
}

uint8_t i8080_check_condition(intel8080_t *cpu, uint8_t condition)
{
	switch(condition)
	{
	case CONDITION_NZ:
		return !(cpu->registers.flags & FLAGS_ZERO);
	case CONDITION_Z:
		return (cpu->registers.flags & FLAGS_ZERO);
	case CONDITION_NC:
		return !(cpu->registers.flags & FLAGS_CARRY);
	case CONDITION_C:
		return (cpu->registers.flags & FLAGS_CARRY);
	case CONDITION_PO:
		return !(cpu->registers.flags & FLAGS_PARITY);
	case CONDITION_PE:
		return (cpu->registers.flags & FLAGS_PARITY);
	case CONDITION_P:
		return !(cpu->registers.flags & FLAGS_SIGN);
	case CONDITION_M:
		return (cpu->registers.flags & FLAGS_SIGN);
	}
	return 0;
}

void i8080_resume(intel8080_t *cpu)
{
	/* Centralized halt-clear used by i8080_examine and front-panel RUN /
	   mode-toggle paths. Also clears STATUS_HALT so the HLTA LED is
	   correct even while the CPU mode is still STOPPED (no cycles yet). */
	cpu->halted = false;
	cpu->cpuStatus &= ~STATUS_HALT;
}

void i8080_examine(intel8080_t *cpu, uint16_t address)
{
	// Jump to the supplied address. Loading a new PC from the front panel
	// also resumes the CPU if it was previously halted by HLT.
	i8080_resume(cpu);
	cpu->registers.pc = cpu->address_bus = address;
	cpu->data_bus = read8(cpu->address_bus);
}

void i8080_examine_next(intel8080_t *cpu)
{
	cpu->address_bus++;
	cpu->data_bus = read8(cpu->address_bus);
}

void i8080_deposit(intel8080_t *cpu, uint8_t data)
{
	cpu->data_bus = data;
	i8080_mwrite(cpu);
}

void i8080_deposit_next(intel8080_t *cpu, uint8_t data)
{
	i8080_examine_next(cpu);
	cpu->data_bus = data;
	i8080_mwrite(cpu);
}

void i8080_update_flags(intel8080_t *cpu, uint8_t reg, uint8_t mask)
{
	/* Note: FLAGS_H is intentionally not handled here. The half-carry must
	   be derived from the operands and is set explicitly by each
	   instruction handler before invoking this helper. */
	uint8_t val = i8080_regread(cpu, reg);

	if (mask & FLAGS_PARITY) {
		i8080_update_flag_bit(cpu, FLAGS_PARITY, get_parity_fast(val));
	}
	if (mask & FLAGS_ZERO) {
		i8080_update_flag_bit(cpu, FLAGS_ZERO, val == 0);
	}
	if (mask & FLAGS_SIGN) {
		i8080_update_flag_bit(cpu, FLAGS_SIGN, val & 0x80);
	}
}

/* Generic 8-bit subtract with explicit carry-in. The 8080 implements SUB/SBB
   via two's-complement addition: result = A + ~val + (1 - cy_in). The
   auxiliary carry and carry-out are taken from the low-nibble and full-byte
   carries of that addition. cy_in is 0 for SUB/SUI/CMP/CPI and equal to the
   current CY flag for SBB/SBI. */
void i8080_gensub(intel8080_t *cpu, uint8_t val, uint8_t cy_in)
{
	uint8_t a = cpu->registers.a;
	uint8_t neg = (uint8_t)~val;
	uint8_t add = (uint8_t)(1 - cy_in);
	uint16_t lo = (uint16_t)(a & 0xf) + (uint16_t)(neg & 0xf) + add;
	uint16_t full = (uint16_t)a + neg + add;

	i8080_update_flag_bit(cpu, FLAGS_H, lo > 0xf);
	i8080_update_flag_bit(cpu, FLAGS_CARRY, !(full > 0xff));

	cpu->registers.a = (uint8_t)full;
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);
}

void i8080_compare(intel8080_t *cpu, uint8_t val)
{
	uint8_t tmp_a = cpu->registers.a;
	i8080_gensub(cpu, val, 0);
	cpu->registers.a = tmp_a;
}

static uint8_t i8080_mov(intel8080_t *cpu)
{
	uint8_t dest = DESTINATION(cpu->current_op_code);
	uint8_t source = SOURCE(cpu->current_op_code);
	uint8_t val;
	uint8_t cycles;

	if(dest == MEMORY_ACCESS || source == MEMORY_ACCESS)
		cycles = CYCLES_MOV_MEM;
	else
		cycles = CYCLES_MOV_REG;

	val = i8080_regread(cpu, source);
	i8080_regwrite(cpu, dest, val);
	cpu->registers.pc++;

	return cycles;
}

static uint8_t i8080_mvi(intel8080_t *cpu)
{
	uint8_t dest = DESTINATION(cpu->current_op_code);
	uint8_t cycles;

	if(dest == MEMORY_ACCESS)
		cycles = CYCLES_MVI_MEM;
	else
		cycles = CYCLES_MVI_REG;

	i8080_regwrite(cpu, dest, read8(cpu->registers.pc+1));

	cpu->registers.pc+=2;

	return cycles;
}

static uint8_t i8080_lxi(intel8080_t *cpu)
{
	uint8_t pair = RP(cpu->current_op_code);

	i8080_pairwrite(cpu, pair, read16(cpu->registers.pc+1));
	cpu->registers.pc+=3;

	return CYCLES_LXI;
}

uint8_t i8080_lda(intel8080_t *cpu)
{
	cpu->address_bus = read16(cpu->registers.pc+1);
	i8080_mread(cpu);
	cpu->registers.a = cpu->data_bus;

	cpu->registers.pc+=3;
	return CYCLES_LDA;
}

uint8_t i8080_sta(intel8080_t *cpu)
{
	cpu->address_bus = read16(cpu->registers.pc+1);
	cpu->data_bus = cpu->registers.a;
	i8080_mwrite(cpu);

	cpu->registers.pc+=3;
	return CYCLES_STA;
}

uint8_t i8080_lhld(intel8080_t *cpu)
{
	uint16_t addr = i8080_mread16(cpu, cpu->registers.pc+1);
	cpu->registers.hl = i8080_mread16(cpu, addr);
	cpu->registers.pc+=3;
	return CYCLES_LHLD;
}

uint8_t i8080_shld(intel8080_t *cpu)
{
	uint16_t addr = i8080_mread16(cpu, cpu->registers.pc+1);
	i8080_mwrite16(cpu, addr, cpu->registers.hl);
	cpu->registers.pc+=3;
	return CYCLES_SHLD;
}

// TODO: only BC and DE allowed for indirect
uint8_t i8080_ldax(intel8080_t *cpu)
{
	uint8_t pair = RP(cpu->current_op_code);

	cpu->address_bus = i8080_pairread(cpu, pair);
	i8080_mread(cpu);
	cpu->registers.a = cpu->data_bus;
	cpu->registers.pc++;
	return CYCLES_LDAX;
}

// TODO: only BC and DE allowed for indirect
uint8_t i8080_stax(intel8080_t *cpu)
{
	uint8_t pair = RP(cpu->current_op_code);
	cpu->address_bus = i8080_pairread(cpu, pair);
	cpu->data_bus = cpu->registers.a;
	i8080_mwrite(cpu);
	cpu->registers.pc++;
	return CYCLES_STAX;
}

uint8_t i8080_xchg(intel8080_t *cpu)
{
	uint16_t tmp = cpu->registers.hl;
	cpu->registers.hl = cpu->registers.de;
	cpu->registers.de = tmp;
	cpu->registers.pc++;

	return CYCLES_XCHG;
}

/* Generic 8-bit add with explicit carry-in. Used by ADD/ADI (cy_in=0) and
   ADC/ACI (cy_in=current CY). Computing the half-carry from the low nibbles
   plus the explicit carry avoids losing AC when val+cy would overflow a
   uint8_t (the bug the previous implementation had). */
void i8080_genadd(intel8080_t *cpu, uint8_t val, uint8_t cy_in)
{
	uint8_t a = cpu->registers.a;
	uint16_t lo = (uint16_t)(a & 0xf) + (uint16_t)(val & 0xf) + cy_in;
	uint16_t full = (uint16_t)a + val + cy_in;

	i8080_update_flag_bit(cpu, FLAGS_H, lo > 0xf);
	i8080_update_flag_bit(cpu, FLAGS_CARRY, full > 0xff);

	cpu->registers.a = (uint8_t)full;
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);
}


uint8_t i8080_add(intel8080_t *cpu)
{
	uint8_t source = SOURCE(cpu->current_op_code);
	uint8_t val = i8080_regread(cpu, source);
	i8080_genadd(cpu, val, 0);
	cpu->registers.pc++;
	return CYCLES_ADD;
}

uint8_t i8080_adi(intel8080_t *cpu)
{
	i8080_genadd(cpu, read8(cpu->registers.pc+1), 0);
	cpu->registers.pc+=2;
	return CYCLES_ADI;
}

uint8_t i8080_adc(intel8080_t *cpu)
{
	uint8_t source = SOURCE(cpu->current_op_code);
	uint8_t val = i8080_regread(cpu, source);
	uint8_t cy = (cpu->registers.flags & FLAGS_CARRY) ? 1 : 0;
	i8080_genadd(cpu, val, cy);
	cpu->registers.pc++;
	return CYCLES_ADC;
}

uint8_t i8080_aci(intel8080_t *cpu)
{
	uint8_t val = read8(cpu->registers.pc+1);
	uint8_t cy = (cpu->registers.flags & FLAGS_CARRY) ? 1 : 0;
	i8080_genadd(cpu, val, cy);
	cpu->registers.pc+=2;
	return CYCLES_ACI;
}

uint8_t i8080_sub(intel8080_t *cpu)
{
	uint8_t source = SOURCE(cpu->current_op_code);
	uint8_t val = i8080_regread(cpu, source);
	i8080_gensub(cpu, val, 0);
	cpu->registers.pc++;
	return CYCLES_SUB;
}

uint8_t i8080_sui(intel8080_t *cpu)
{
	i8080_gensub(cpu, read8(cpu->registers.pc+1), 0);
	cpu->registers.pc+=2;
	return CYCLES_SUI;
}

uint8_t i8080_sbb(intel8080_t *cpu)
{
	uint8_t source = SOURCE(cpu->current_op_code);
	uint8_t val = i8080_regread(cpu, source);
	uint8_t cy = (cpu->registers.flags & FLAGS_CARRY) ? 1 : 0;
	i8080_gensub(cpu, val, cy);
	cpu->registers.pc++;
	return CYCLES_SBB;
}

uint8_t i8080_sbi(intel8080_t *cpu)
{
	uint8_t val = read8(cpu->registers.pc+1);
	uint8_t cy = (cpu->registers.flags & FLAGS_CARRY) ? 1 : 0;
	i8080_gensub(cpu, val, cy);
	cpu->registers.pc+=2;
	return CYCLES_SBI;
}

uint8_t i8080_inr(intel8080_t *cpu)
{
	uint8_t dest = DESTINATION(cpu->current_op_code);
	uint8_t val = i8080_regread(cpu, dest);

	i8080_update_flag_bit(cpu, FLAGS_H, CHECK_HALF_CARRY(val, 1));

	i8080_regwrite(cpu, dest, val + 1);

	i8080_update_flags(cpu, dest, FLAGS_ZERO | FLAGS_PARITY | FLAGS_SIGN);
	cpu->registers.pc++;
	return CYCLES_INR;
}

uint8_t i8080_dcr(intel8080_t *cpu)
{
	uint8_t dest = DESTINATION(cpu->current_op_code);
	uint8_t val = i8080_regread(cpu, dest);

	i8080_update_flag_bit(cpu, FLAGS_H, CHECK_HALF_CARRY(val, 0xff));

	i8080_regwrite(cpu, dest, val + 0xff);
	i8080_update_flags(cpu, dest, FLAGS_ZERO | FLAGS_PARITY | FLAGS_SIGN);
	cpu->registers.pc++;
	return CYCLES_DCR;
}

uint8_t i8080_inx(intel8080_t *cpu)
{
	uint8_t rp = RP(cpu->current_op_code);
	i8080_pairwrite(cpu, rp, i8080_pairread(cpu, rp) + 1);
	cpu->registers.pc++;
	return CYCLES_INX;
}

uint8_t i8080_dcx(intel8080_t *cpu)
{
	uint8_t rp = RP(cpu->current_op_code);
	cpu->registers.pc++;
	i8080_pairwrite(cpu, rp, i8080_pairread(cpu, rp) - 1);
	return CYCLES_DCX;
}

uint8_t i8080_dad(intel8080_t *cpu)
{
	uint8_t rp = RP(cpu->current_op_code);
	uint32_t val = i8080_pairread(cpu, rp);
	val += i8080_pairread(cpu, PAIR_HL);

	if(val > 0xffff)
		i8080_set_flag(cpu, FLAGS_CARRY);
	else
		i8080_clear_flag(cpu, FLAGS_CARRY);

	i8080_pairwrite(cpu, PAIR_HL, val & 0xffff);

	cpu->registers.pc++;

	return CYCLES_DAD;
}

uint8_t i8080_ana(intel8080_t *cpu)
{
	uint8_t source = SOURCE(cpu->current_op_code);
	uint8_t val = i8080_regread(cpu, source);

	/* 8080 sets AC to bit 3 of (A | operand) before the AND. */
	i8080_update_flag_bit(cpu, FLAGS_H, (cpu->registers.a | val) & 0x08);
	cpu->registers.a &= val;
	i8080_clear_flag(cpu, FLAGS_CARRY);
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);

	cpu->registers.pc++;
	return CYCLES_ANA;
}

uint8_t i8080_ani(intel8080_t *cpu)
{
	uint8_t val = read8(cpu->registers.pc+1);

	i8080_update_flag_bit(cpu, FLAGS_H, (cpu->registers.a | val) & 0x08);
	cpu->registers.a &= val;
	i8080_clear_flag(cpu, FLAGS_CARRY);
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);

	cpu->registers.pc+=2;
	return CYCLES_ANI;
}

uint8_t i8080_ora(intel8080_t *cpu)
{
	uint8_t source = SOURCE(cpu->current_op_code);

	cpu->registers.a |= i8080_regread(cpu, source);
	i8080_clear_flag(cpu, FLAGS_CARRY);
	i8080_clear_flag(cpu, FLAGS_H);
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);

	cpu->registers.pc++;
	return CYCLES_ORA;
}

uint8_t i8080_ori(intel8080_t *cpu)
{
	cpu->registers.a |= read8(cpu->registers.pc+1);
	i8080_clear_flag(cpu, FLAGS_CARRY);
	i8080_clear_flag(cpu, FLAGS_H);
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);

	cpu->registers.pc+=2;
	return CYCLES_ORI;
}

uint8_t i8080_xra(intel8080_t *cpu)
{
	uint8_t source = SOURCE(cpu->current_op_code);

	cpu->registers.a ^= i8080_regread(cpu, source);
	i8080_clear_flag(cpu, FLAGS_CARRY);
	i8080_clear_flag(cpu, FLAGS_H);
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);

	cpu->registers.pc++;
	return CYCLES_XRA;
}

uint8_t i8080_xri(intel8080_t *cpu)
{
	cpu->registers.a ^= read8(cpu->registers.pc+1);
	i8080_clear_flag(cpu, FLAGS_CARRY);
	i8080_clear_flag(cpu, FLAGS_H);
	i8080_update_flags(cpu, REGISTER_A, FLAGS_ZERO | FLAGS_SIGN | FLAGS_PARITY);

	cpu->registers.pc+=2;
	return CYCLES_XRI;
}

uint8_t i8080_ei(intel8080_t *cpu)
{
	cpu->registers.pc++;
	cpu->iff = true;
	return CYCLES_EI;
}

uint8_t i8080_di(intel8080_t *cpu)
{
	cpu->registers.pc++;
	cpu->iff = false;
	return CYCLES_DI;
}

uint8_t i8080_xthl(intel8080_t *cpu)
{
	cpu->cpuStatus |= STATUS_STACK;
	uint16_t temp = i8080_mread16(cpu, cpu->registers.sp);
	i8080_mwrite16(cpu, cpu->registers.sp, cpu->registers.hl);
	cpu->registers.hl = temp;
	cpu->registers.pc++;
	return CYCLES_XTHL;
}

uint8_t i8080_sphl(intel8080_t *cpu)
{
	cpu->registers.sp = cpu->registers.hl;
	cpu->registers.pc++;
	return CYCLES_SPHL;
}

uint8_t i8080_in(intel8080_t *cpu)
{
	static uint8_t character = 0;
	uint8_t port = read8(cpu->registers.pc + 1);
	cpu->cpuStatus |= STATUS_PORT_INPUT;

	switch(port)
	{
	case 0x00:
		cpu->registers.a = 0x00;
		break;
	case 0x1:
		cpu->registers.a = cpu->term_in();
		// cpu->term_out(cpu->registers.a);
		break;
	case 0x8:
		cpu->registers.a = cpu->disk_controller.disk_status();
		break;
	case 0x9:
		cpu->registers.a = cpu->disk_controller.sector();
		break;
	case 0xa:
		cpu->registers.a = cpu->disk_controller.read();
		break;
	case 0x10: // 2SIO port 1, status
		cpu->registers.a = 0x2; // bit 1 == transmit buffer empty
		if(!character)
		{
			character = cpu->term_in();
		}
		if(character)
		{
			cpu->registers.a |= 0x1;
		}
		break;
	case 0x11: // 2SIO port 1, read
		if(character)
		{
			cpu->registers.a = character;
			character = 0;
		}
		else
		{
			cpu->registers.a = cpu->term_in();
		}
		break;
	case 0xff: // Front panel switches
		cpu->registers.a = cpu->sense();
		break;
	default:
		cpu->registers.a = 0xff;
		cpu->registers.a = cpu->io_port_in_handler(port);
		//printf("IN PORT %x\n", cpu->data_bus);
		break;
	}

	cpu->registers.pc+=2;
	return CYCLES_IN;
}

uint8_t i8080_out(intel8080_t *cpu)
{
	uint8_t port = read8(cpu->registers.pc + 1);
	cpu->cpuStatus |= STATUS_PORT_OUTPUT | STATUS_WRITE_OUTPUT;
	switch(port)
	{
	case 0x1:
		cpu->term_out(cpu->registers.a);
		break;
	case 0x8:
		cpu->disk_controller.disk_select(cpu->registers.a);
		break;
	case 0x9:
		cpu->disk_controller.disk_function(cpu->registers.a);
		break;
	case 0xa:
		cpu->disk_controller.write(cpu->registers.a);
		break;
	case 0x10:  // 2SIO port 1 control
		break;
	case 0x11: // 2sio port 1 write
		cpu->term_out(cpu->registers.a);
		break;
	default:
		cpu->io_port_out_handler(port, cpu->registers.a);
		// printf("OUT PORT %x, DATA: %x\n", read8(cpu->registers.pc + 1), cpu->registers.a);
		break;
	}
	cpu->registers.pc+=2;
	return CYCLES_OUT;
}

uint8_t i8080_push(intel8080_t *cpu)
{
	cpu->cpuStatus |= STATUS_STACK;
	uint8_t pair = RP(cpu->current_op_code);
	uint16_t val;

	if(pair == PAIR_SP) {
		/* PUSH PSW: low byte is the flag register, but the real 8080
		   always pushes bit1=1, bit3=0, bit5=0. */
		uint8_t f = (cpu->registers.flags & 0xD5) | 0x02;
		val = ((uint16_t)cpu->registers.a << 8) | f;
	} else {
		val = i8080_pairread(cpu, pair);
	}

	cpu->registers.sp-=2;
	i8080_mwrite16(cpu, cpu->registers.sp, val);

	cpu->registers.pc++;
	return CYCLES_PUSH;
}

uint8_t i8080_pop(intel8080_t *cpu)
{
	cpu->cpuStatus |= STATUS_STACK;
	uint8_t pair = RP(cpu->current_op_code);
	uint16_t val = i8080_mread16(cpu, cpu->registers.sp);
	cpu->registers.sp+=2;
	if(pair == PAIR_SP) {
		cpu->registers.af = val;
		/* POP PSW: keep only the documented PSW bits and force the
		   fixed bit pattern. */
		cpu->registers.flags = (cpu->registers.flags & 0xD5) | 0x02;
	} else {
		i8080_pairwrite(cpu, pair, val);
	}

	cpu->registers.pc++;
	return CYCLES_POP;
}

uint8_t i8080_stc(intel8080_t *cpu)
{
	i8080_set_flag(cpu, FLAGS_CARRY);
	cpu->registers.pc++;
	return CYCLES_STC;
}

uint8_t i8080_cmc(intel8080_t *cpu)
{
	cpu->registers.flags ^= FLAGS_CARRY;
	cpu->registers.pc++;
	return CYCLES_CMC;
}

uint8_t i8080_rlc(intel8080_t *cpu)
{
	uint8_t high_bit = cpu->registers.a & 0x80;

	cpu->registers.a <<= 1;
	if(high_bit)
	{
		i8080_set_flag(cpu, FLAGS_CARRY);
		cpu->registers.a |= 1;
	}
	else
	{
		i8080_clear_flag(cpu, FLAGS_CARRY);
		cpu->registers.a &= ~1;
	}

	cpu->registers.pc++;
	return CYCLES_RLC;
}

uint8_t i8080_rrc(intel8080_t *cpu)
{
	uint8_t low_bit = cpu->registers.a & 1;

	cpu->registers.a >>= 1;

	if(low_bit)
	{
		i8080_set_flag(cpu, FLAGS_CARRY);
		cpu->registers.a |= 0x80;
	}
	else
	{
		cpu->registers.a &= ~0x80;
		i8080_clear_flag(cpu, FLAGS_CARRY);
	}

	cpu->registers.pc++;
	return CYCLES_RRC;
}

uint8_t i8080_ral(intel8080_t *cpu)
{
	uint8_t high_bit = cpu->registers.a & 0x80;
	cpu->registers.a <<= 1;

	if(cpu->registers.flags & FLAGS_CARRY)
		cpu->registers.a |= 1;
	else
		cpu->registers.a &= ~1;

	if(high_bit)
		i8080_set_flag(cpu, FLAGS_CARRY);
	else
		i8080_clear_flag(cpu, FLAGS_CARRY);

	cpu->registers.pc++;
	return CYCLES_RAL;
}

uint8_t i8080_rar(intel8080_t *cpu)
{
	uint8_t low_bit = cpu->registers.a & 1;

	cpu->registers.a >>= 1;

	if(cpu->registers.flags & FLAGS_CARRY)
		cpu->registers.a |= 0x80;
	else
		cpu->registers.a &= ~0x80;

	if(low_bit)
		i8080_set_flag(cpu, FLAGS_CARRY);
	else
		i8080_clear_flag(cpu, FLAGS_CARRY);

	cpu->registers.pc++;
	return CYCLES_RAR;
}

uint8_t i8080_jmp(intel8080_t *cpu)
{
	cpu->registers.pc = i8080_mread16(cpu, cpu->registers.pc+1);
	return CYCLES_JMP;
}

uint8_t i8080_jccc(intel8080_t *cpu)
{
	uint8_t condition = CONDITION(cpu->current_op_code);

	if(i8080_check_condition(cpu, condition))
	{
		i8080_jmp(cpu);
	}
	else
	{
		cpu->registers.pc+=3;
	}

	return CYCLES_JMP;
}

uint8_t i8080_ret(intel8080_t *cpu)
{
	cpu->cpuStatus |= STATUS_STACK;
	cpu->registers.pc = i8080_mread16(cpu, cpu->registers.sp);
	cpu->registers.sp+=2;
	return CYCLES_RET;
}

uint8_t i8080_rccc(intel8080_t *cpu)
{
	uint8_t condition = CONDITION(cpu->current_op_code);

	if(i8080_check_condition(cpu, condition))
	{
		i8080_ret(cpu);
		return CYCLES_RCC_TAKEN;
	}

	cpu->registers.pc++;
	return CYCLES_RCC_NOTTAKEN;
}

uint8_t i8080_rst(intel8080_t *cpu)
{
	cpu->cpuStatus |= STATUS_STACK;
	uint8_t vec = DESTINATION(cpu->current_op_code);

	cpu->registers.sp-=2;
	i8080_mwrite16(cpu, cpu->registers.sp, cpu->registers.pc + 1);

	cpu->registers.pc = vec*8;

	return CYCLES_RST;
}

uint8_t i8080_call(intel8080_t *cpu)
{
	cpu->cpuStatus |= STATUS_STACK;
	uint16_t target = i8080_mread16(cpu, cpu->registers.pc + 1);
	cpu->registers.sp-=2;
	i8080_mwrite16(cpu, cpu->registers.sp, cpu->registers.pc + 3);

	cpu->registers.pc = target;
	return CYCLES_CALL;
}

uint8_t i8080_cccc(intel8080_t *cpu)
{
	uint8_t condition = CONDITION(cpu->current_op_code);

	if(i8080_check_condition(cpu, condition))
	{
		i8080_call(cpu);
		return CYCLES_CCC_TAKEN;
	}

	cpu->registers.pc+=3;
	return CYCLES_CCC_NOTTAKEN;
}

uint8_t i8080_pchl(intel8080_t *cpu)
{
	cpu->registers.pc = cpu->registers.hl;
	return CYCLES_PCHL;
}

uint8_t i8080_nop(intel8080_t *cpu)
{
	cpu->registers.pc++;

	return CYCLES_NOP;
}

uint8_t i8080_cma(intel8080_t *cpu)
{
	cpu->registers.a = ~cpu->registers.a;
	cpu->registers.pc++;

	return CYCLES_CMA;
}

uint8_t i8080_cmp(intel8080_t *cpu)
{
	uint8_t reg = SOURCE(cpu->current_op_code);

	i8080_compare(cpu, i8080_regread(cpu, reg));

	cpu->registers.pc++;
	return CYCLES_CMP;
}

uint8_t i8080_cpi(intel8080_t *cpu)
{
	i8080_compare(cpu, read8(cpu->registers.pc+1));
	cpu->registers.pc+=2;
	return CYCLES_CPI;
}

void i8080_fetch_next_op(intel8080_t *cpu)
{
	cpu->address_bus = cpu->registers.pc;
	i8080_mread(cpu);
	cpu->cpuStatus |= STATUS_OP_CODE_FETCH;
}

uint8_t i8080_daa(intel8080_t *cpu)
{
	/* DAA per 8080 programmer's manual. The step-2 condition must be
	   evaluated against the ORIGINAL high nibble, not the post-step-1
	   value, because step 1 (adding 6 when lsb > 9) can wrap the byte
	   and lose the carry-into-msb. The canonical formulation:

	     step 1 fires if   AC || lsb > 9
	     step 2 fires if   CY || msb > 9 || (msb >= 9 && lsb > 9)

	   The (msb >= 9 && lsb > 9) term covers the case where step 1
	   carries from the low nibble into a high nibble that is exactly 9,
	   turning it into A and therefore needing step 2.

	   CY out is set if CY was set on entry OR step 2 fired. AC and the
	   ZSP flags are derived from the actual ADD (delegated to
	   i8080_genadd), which handles the half-carry from step 1's +6
	   correctly. Caught by 8080EXM <daa,cma,stc,cmc>. */
	uint8_t a   = cpu->registers.a;
	uint8_t lsb = a & 0x0f;
	uint8_t msb = a >> 4;
	uint8_t cy_in = (cpu->registers.flags & FLAGS_CARRY) ? 1 : 0;
	uint8_t h_in  = (cpu->registers.flags & FLAGS_H)     ? 1 : 0;

	uint8_t correction = 0;
	uint8_t cy_out = cy_in;

	if (h_in || lsb > 9) {
		correction += 0x06;
	}
	if (cy_in || msb > 9 || (msb >= 9 && lsb > 9)) {
		correction += 0x60;
		cy_out = 1;
	}

	i8080_genadd(cpu, correction, 0);
	i8080_update_flag_bit(cpu, FLAGS_CARRY, cy_out);

	cpu->registers.pc++;
	return CYCLES_DAA;
}

uint8_t i8080_hlt(intel8080_t *cpu)
{
	/* On the real 8080, HLT advances PC past the instruction and then
	   stops the CPU until an interrupt arrives. Without an interrupt
	   path wired up here we simply mark the CPU halted; i8080_cycle()
	   short-circuits while halted and keeps STATUS_HALT asserted so
	   the HLTA front-panel LED stays lit. */
	cpu->registers.pc++;
	cpu->halted = true;
	cpu->cpuStatus |= STATUS_HALT;
	return CYCLES_HLT;
}

void i8080_cycle(intel8080_t *cpu)
{
	if (UNLIKELY(cpu->halted)) {
		/* Keep HLTA asserted while halted. The bus values stay at the
		   instruction following HLT, matching real-8080 behavior. */
		cpu->cpuStatus = STATUS_HALT;
		return;
	}

	cpu->cpuStatus = 0;
	i8080_fetch_next_op(cpu);

	uint8_t op_code = cpu->current_op_code = cpu->data_bus;
	uint8_t (*handler)(intel8080_t *) = opcode_handlers[op_code];

	if (LIKELY(handler != NULL)) {
		handler(cpu);
	} else {
		// Handle undefined opcodes (NOP behavior)
		cpu->registers.pc++;
	}
}
