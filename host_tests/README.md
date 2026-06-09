# CPU host test harness (i8080 + Z80)

This directory contains a host-side test harness that links the actual
emulator core (`../altair8800/x80.cxx` via `cpu_x80_adapter.cpp`) and runs
CP/M-style `.COM` diagnostic ROMs through a minimal BDOS trap. It is the
safety net that catches regressions in the CPU core — including any future
refactor.

The one mode-agnostic harness source (`run_8080_tests.c`) is compiled twice,
once per CPU specialization:

| Executable        | Core mode            | ROM folder        |
| ----------------- | -------------------- | ----------------- |
| `run_8080_tests`  | 8080 (`X80_FORCE_8080`) | `roms/`        |
| `run_z80_tests`   | Z80  (`X80_FORCE_Z80`)  | `roms_z80/`    |

The harness builds with plain CMake on the host (no ESP-IDF required).

## Layout

```
host_tests/
├── CMakeLists.txt        Standalone host build (both executables + tests)
├── run_8080_tests.c      Harness: BDOS trap, ROM driver, built-in self-test
├── roms/                 Standard 8080 diagnostic .COM files
├── roms_z80/             Z80 exerciser .COM files (ZEXDOC, ZEXALL)
└── README.md             This file
```

## Building

From the repository root:

```sh
cmake -S host_tests -B build-host-tests
cmake --build build-host-tests
```

This produces `build-host-tests/run_8080_tests`.

## Running

### Built-in self-test (always available)

A tiny hand-assembled 8080 program is embedded in the harness. It
exercises ADD/SUB/CPI/JNZ/CALL/JMP and BDOS string output, and prints
`SELFTEST PASS` on success. This proves the harness wiring works end to
end without needing any external files.

```sh
./build-host-tests/run_8080_tests --selftest
```

Expected output:

```
========== Built-in self-test ==========
SELFTEST PASS
--- run status: clean exit
RESULT: selftest ok
```

### Standard 8080 diagnostic ROMs

Drop the four canonical `.COM` files into [`roms/`](roms/), then:

```sh
./build-host-tests/run_8080_tests --roms host_tests/roms
```

Expected output (with all four ROMs present, against a known-good core):

```
========== 8080EXM.COM (4608 bytes) ==========
8080 instruction exerciser
dad <b,d,h,sp>................  PASS! crc is:14474ba6
aluop nn......................  PASS! crc is:9e922f9e
aluop <b,c,d,e,h,l,m,a>.......  PASS! crc is:cf762c86
<daa,cma,stc,cmc>.............  PASS! crc is:bb3f030c
<inr,dcr> a...................  PASS! crc is:adb6460e
<inr,dcr> b...................  PASS! crc is:83ed1345
<inx,dcx> b...................  PASS! crc is:f79287cd
<inr,dcr> c...................  PASS! crc is:e5f6721b
<inr,dcr> d...................  PASS! crc is:15b5579a
<inx,dcx> d...................  PASS! crc is:7f4e2501
<inr,dcr> e...................  PASS! crc is:cf2ab396
<inr,dcr> h...................  PASS! crc is:12b2952c
<inx,dcx> h...................  PASS! crc is:9f2b23c0
<inr,dcr> l...................  PASS! crc is:ff57d356
<inr,dcr> m...................  PASS! crc is:92e963bd
<inx,dcx> sp..................  PASS! crc is:d5702fab
lhld nnnn.....................  PASS! crc is:a9c3d5cb
shld nnnn.....................  PASS! crc is:e8864f26
lxi <b,d,h,sp>,nnnn...........  PASS! crc is:fcf46e12
ldax <b,d>....................  PASS! crc is:2b821d5f
mvi <b,c,d,e,h,l,m,a>,nn......  PASS! crc is:eaa72044
mov <bcdehla>,<bcdehla>.......  PASS! crc is:10b58cee
sta nnnn / lda nnnn...........  PASS! crc is:ed57af72
<rlc,rrc,ral,rar>.............  PASS! crc is:e0d89235
stax <b,d>....................  PASS! crc is:2b0471e9
Tests complete
--- run status: clean exit
RESULT: 8080EXM.COM ok

========== 8080PRE.COM (1024 bytes) ==========
8080 Preliminary tests complete
--- run status: clean exit
RESULT: 8080PRE.COM ok

========== TST8080.COM (1536 bytes) ==========
MICROCOSM ASSOCIATES 8080/8085 CPU DIAGNOSTIC
 VERSION 1.0  (C) 1980

 CPU IS OPERATIONAL
--- run status: clean exit
RESULT: TST8080.COM ok

========== CPUTEST.COM (19200 bytes) ==========

DIAGNOSTICS II V1.2 - CPU TEST
COPYRIGHT (C) 1981 - SUPERSOFT ASSOCIATES

ABCDEFGHIJKLMNOPQRSTUVWXYZ
CPU IS 8080/8085
BEGIN TIMING TEST
END TIMING TEST
CPU TESTS OK

--- run status: clean exit
RESULT: CPUTEST.COM ok

========== ROM summary: 4/4 passed ==========
```

The order of ROMs in the output depends on directory traversal order
and is not significant. The 8080EXM CRCs above are the canonical
post-state CRCs for a correct 8080; any deviation indicates a bug in
the core. Exit code is non-zero if any ROM fails.

### Via CTest

All tests are registered with CTest:

```sh
ctest --test-dir build-host-tests --output-on-failure
```

| Test           | What it runs                                  |
| -------------- | --------------------------------------------- |
| `json_util`    | MCP JSON helper unit tests                    |
| `selftest`     | Built-in 8080 self-test (`run_8080_tests`)    |
| `roms`         | 8080 diagnostic ROMs in `roms/`               |
| `selftest_z80` | Built-in self-test on the Z80 build           |
| `roms_z80`     | Z80 exerciser ROMs in `roms_z80/`             |

The `roms` and `roms_z80` tests report `Skipped` (CTest exit code 77)
when their ROM folder is empty or missing, so a fresh checkout without
the ROMs still has a green CI run.

`--output-on-failure` only echoes a test's captured output when it
**fails**, so a passing run shows just the one-line summary per test.
To see the full per-instruction breakout (every `OK` / `PASS! crc is:…`
line) on a passing run, use verbose mode:

```sh
ctest --test-dir build-host-tests -V              # all tests, full output
ctest --test-dir build-host-tests -V -R roms_z80  # just the Z80 exercisers
```

Or run the harness binaries directly (same detailed output):

```sh
./build-host-tests/run_8080_tests --roms host_tests/roms
./build-host-tests/run_z80_tests  --roms host_tests/roms_z80
```

## The 8080 ROMs

| File          | Author / origin                          | Notes                                            |
| ------------- | ---------------------------------------- | ------------------------------------------------ |
| `TST8080.COM` | Kelly Smith, Microcosm Associates (1980) | Quick sanity check; prints `CPU IS OPERATIONAL`. |
| `CPUTEST.COM` | SuperSoft Diagnostics II, v1.2           | Broader instruction coverage.                    |
| `8080PRE.COM` | Frank Cringle                            | Preliminary exerciser; fast.                     |
| `8080EXM.COM` | Frank Cringle                            | **Full exerciser.** CRC-checks every documented instruction against the post-state of a real chip. Run takes 30–120 s on a modern Mac. |

These are widely-redistributed binaries. Reliable sources:

- [`superzazu/8080`](https://github.com/superzazu/8080/tree/master/cpu_tests) — has all four ready to download.
- [`udo-munk/z80pack`](https://github.com/udo-munk/z80pack) — under the CP/M-2 disk libraries.
- The Altair Clone downloads page: <https://altairclone.com/downloads/cpu_tests/>.

Place them directly in [`roms/`](roms/) (case-insensitive `.COM` extension).

## The Z80 ROMs

Frank Cringle's Z80 instruction exercisers (`zexdoc`/`zexall`) CRC-check
every Z80 instruction against the post-state of a real Z80, including
the `ix`/`iy` index registers, block moves, bit ops, and the extended
opcodes that the 8080 lacks. They run through the same BDOS trap as the
8080 ROMs and print `OK` for each instruction group on success.

| File         | Origin        | Notes                                                          |
| ------------ | ------------- | -------------------------------------------------------------- |
| `ZEXDOC.COM` | Frank Cringle | Checks only the **documented** flag bits. ~2 min on a modern Mac. |
| `ZEXALL.COM` | Frank Cringle | Also checks the **undocumented** flag bits (bits 3 & 5). Slower. |

Place them directly in [`roms_z80/`](roms_z80/) (case-insensitive
`.COM` extension). A copy of both ships in the repo under
`references/ntvcm/z80test/`.

## How the harness works

The standard 8080 test ROMs are CP/M `.COM` files. The harness provides
just enough CP/M to run them:

1. Program is loaded at `0x0100` and `PC` is set to `0x0100`.
2. `0x0000` contains `HLT` as an exit sentinel — when the program does a
   final `JMP 0` or returns to `0x0000`, the harness stops.
3. `0x0005` contains `RET`. Before fetching from `0x0005` the harness
   traps and synthesizes the BDOS call from register `C`:
   - `C=0` — warm boot (clean exit)
   - `C=2` — print character in `E`
   - `C=9` — print `$`-terminated string at `DE`
   then pops the return address and continues.
4. After each instruction the captured BDOS output is scanned
   (case-insensitively) for `ERROR` and `FAIL`. Either substring marks
   the ROM as failed.
5. Each run has a generous per-ROM cycle limit (6×10¹⁰ cycles) so a
   stuck program can’t hang the suite. Well-behaved ROMs exit early on
   warm boot, so the ceiling only matters for a broken core.

No I/O ports, sense switches, disk controller, or interrupts are needed
for these ROMs, so the harness installs no-op stubs for those callbacks.

The same harness binary is compiled twice — once with `X80_FORCE_8080`
(`run_8080_tests`) and once with `X80_FORCE_Z80` (`run_z80_tests`) — so
both instruction-set specializations of the single CPU core are
exercised by identical CP/M plumbing.

## CLI

`run_8080_tests` and `run_z80_tests` share the same CLI (only the forced
CPU mode differs):

```
<exe> --selftest               run the built-in self-test
<exe> --roms <directory>       run all .COM files in <directory>
<exe>                          selftest + roms in host_tests/roms
```

For example:

```sh
./build-host-tests/run_8080_tests --roms host_tests/roms
./build-host-tests/run_z80_tests  --roms host_tests/roms_z80
```

Exit codes:

- `0` — all selected tests passed.
- `1` — at least one test failed.
- `77` — `--roms` mode found no ROMs (CTest interprets this as `Skipped`).
- `2` — bad CLI usage.

## Adding new tests

For new programs you want to validate against the core, the easiest
path is to drop the assembled `.COM` file into `roms/` (8080) or
`roms_z80/` (Z80) — anything in those directories is picked up
automatically. For C-side unit tests of specific instructions, add a
new `add_test` entry in `CMakeLists.txt` and either embed the program
as a byte array (see `selftest_program[]` in `run_8080_tests.c`) or load
it from disk.
