---
name: mac-asm
description: Write, edit, and review Digital Research MAC macro assembler source for 8080 CP/M / Altair 8800 projects. Use when working on .ASM source that uses macros (MACRO/ENDM, REPT, IRP, IRPC, conditional assembly), migrating BDS C code to MAC/8080 assembly, or when a .SUB build script assembles 8080 code on CP/M. MAC.COM is a superset of ASM.COM: it assembles everything ASM.COM can plus macros. Covers when to choose MAC over ASM, the MAC build/clean flow (mac -> load -> .COM), the extra .SYM/.PRN artifacts, and the 6-character symbol and decimal-port constraints shared with ASM.
---

# MAC (Digital Research Macro Assembler)

## What MAC Is

`MAC.COM` is Digital Research's **macro assembler** for the Intel 8080 under
CP/M. It is a strict **superset of `ASM.COM`**: any source `ASM.COM` accepts,
`MAC.COM` also accepts and produces the same object code. The reverse is not
true — source that uses MAC-only features will not assemble under `ASM.COM`.

Lineage: `ASM` -> `MAC` -> `RMAC` (relocating macro assembler emitting `.REL`
for `LINK-80`). This skill covers `MAC` only.

In this repo, `MAC.COM` lives on the `B:` drive alongside `LOAD.COM`, `CC`,
`CLINK`, `FT`, and the other build tools. `ASM.COM` is **not** on `B:` — MAC is
the only assembler present, which is fine because it assembles everything ASM
would. Always call `mac` (never `asm`) in build scripts.

## When To Use MAC Instead Of ASM

Use `MAC.COM` when the source uses any of these (none are understood by ASM):

- `MACRO` / `ENDM` macro definitions
- Repetition blocks: `REPT`, `IRP`, `IRPC`
- `LOCAL` macro-local labels, `EXITM`
- `MACLIB` macro-library includes
- Richer conditional assembly (`IF` / `ELSE` / `ENDIF`)

For plain inline 8080 with no macros, MAC still works and produces the same
binary ASM would, so there is no reason to look for ASM. Historically `ASM.COM`
shipped free with CP/M while `MAC.COM` was a paid add-on, but in this repo only
MAC is present, so always assemble with `mac`. Use macros when they genuinely
make the source clearer (repeated BDOS call sequences, table generation, etc.).

## Build And Clean Flow

MAC assembles a `.ASM` source to Intel `.HEX`, then `LOAD.COM` turns the `.HEX`
into the runnable `.COM`. The flow is identical to ASM except for the assembler
name and the extra artifacts MAC produces.

```
mac  myapp        -> MYAPP.HEX, MYAPP.PRN, MYAPP.SYM
load myapp        -> MYAPP.COM
```

ASM emits `.HEX` + `.PRN`. **MAC also emits `.SYM`** (the symbol table, used by
`SID`). When a `.SUB` build script switches from `asm` to `mac`, add cleanup
for the extra files so they do not litter the disk:

```
b:mac myapp
b:load myapp
a:pip a:myapp.com=myapp.com

era myapp.asm
era myapp.hex
era myapp.prn
era myapp.sym
```

If the script already uses a wildcard clean such as `era myapp.*`, no extra
erase lines are needed.

For very large `.ASM` sources, the `.PRN` listing and `.SYM` file can overflow
the small CP/M work disk before the `.HEX` is complete, producing
`OUTPUT FILE WRITE ERROR`. If you only need the `.HEX` for `LOAD`, suppress
the listing and symbol output by sending PRN and SYM to the null device:

```
mac myapp $PZ SZ
```

Inside a `.SUB` file, `$` starts SuperSUB parameter substitution, so escape it
as `$$`:

```
b:mac myapp $$PZ SZ
```

## Source Constraints (shared with ASM)

These limits come from the CP/M assembler/loader toolchain, not from C:

- Symbols must be **unique within their first 6 characters**. This is about
  *significance*, not a hard length cap: the assembler accepts longer names and
  silently truncates to 6, so names like `MESSAGE`, `IBUFLEN`, or `FLAGTRK`
  assemble fine on their own. The only real error is a first-6 *collision*
  between two distinct names. Do not port BDS C's hard 7-character length
  rejection onto assembly — flagging raw length alone produces false positives
  on sources (and was a real bug fixed in `check_mac_asm.py`).
- Express I/O ports in **decimal** in `IN` / `OUT` (the assembler is picky about
  port radix in some builds; the existing repo `.ASM` files use decimal ports).
- `ORG 0100H` — transient programs load at the base of the TPA.
- BDOS entry is `CALL 0005H`; common functions: 2 = console out (char in `E`),
  9 = print `$`-terminated string (`DE` -> string).
- Reading the command tail: length byte at `0080H`, characters from `0081H`
  (already upper-cased by the CCP).
- `!` is a statement separator in MAC/ASM, even when it appears after `;` in a
  comment. Do not write comments such as `; x != y`; use `<>` or words instead.
- Prefer explicit hex for negative immediates (`0FFFFH`, `0FFE6H`) instead of
  relying on unary minus syntax.

## Reserved-Word Caution When Porting ASM -> MAC

Because MAC adds directives (`MACRO`, `ENDM`, `REPT`, `IRP`, `IRPC`, `EXITM`,
`LOCAL`, `MACLIB`, etc.), an old ASM source that used one of those as a label or
symbol will clash under MAC. This is uncommon but is the one real gotcha when
converting `asm` build steps to `mac`. Scan for those names before switching.

Also avoid labels that behave like assembler pseudo-ops or module directives in
some MAC-family tools. In practice, `TITLE`, `IRP`, and `NAME` have all caused
bad assemblies or validator failures in this repo. Rename them to local forms
such as `TTLBAR`, `IRPT`, or `FNAME`.

## Porting C Runtime Logic To 8080

When migrating BDS C code by hand, do an extra pass over helper conventions and
heap behavior. These bugs assemble cleanly but fail at runtime:

- If a helper has a stack-preserving prologue, call its public entry point. Do
  not jump into an internal loop label past a `PUSH`; the matching `POP` will
  consume the caller's return address.
- Write each helper's register convention in a comment and audit `XCHG` before
  calls. For example, `strcpy`-style helpers here use `DE=dst, HL=src`; one
  extra `XCHG` silently reverses a copy.
- If a custom allocator stores a free-list `next` pointer inside freed payloads,
  make every block large enough for that pointer. With a 2-byte header and a
  2-byte `next`, the minimum total block size is 4 bytes; otherwise freeing an
  empty string overwrites the next block.
- Preserve working heap during large file loads. A load can be technically
  successful but leave too little memory for editing, redraw, save, prompts, or
  recovery. Add a load-time reserve and truncate before the allocator is nearly
  exhausted.

## Macro Patterns That Work Well Here

Wrap repeated BDOS call sequences as macros to cut boilerplate:

```
DOS     MACRO   FUNC            ; invoke a BDOS function
        MVI     C,FUNC
        CALL    BDOS
        ENDM

PRINT   MACRO   STR             ; print a '$'-terminated string
        LXI     D,STR
        DOS     FPRSTR
        ENDM
```

Keep macro and symbol names within the 6-character uniqueness rule, the same as
any other label. Reference implementation: `Apps/MCPDONE/MCPDONE.ASM` (built via
`mac` in `Apps/MCPDONE/MCPDONE.SUB`).

## Workflow

1. Decide ASM vs MAC: if the source needs macros/repetition, use MAC; otherwise
   ASM is fine. Match the surrounding repo style.
2. Keep every symbol unique in its first 6 characters; use decimal ports.
3. Run the symbol checker on each `.ASM` file you changed before building:

   ```bash
   python3 .codex/skills/mac-asm/scripts/check_mac_asm.py path/to/FILE.ASM
   ```

   It flags first-6-character symbol collisions, labels that reuse a MAC
   directive name, and hex `IN`/`OUT` ports that should be decimal. Pass all
   changed files in one command.
4. Update the app's `.SUB` to call `mac` (not `asm`) and to erase the extra
   `.HEX` / `.PRN` / `.SYM` artifacts (or use a `era <app>.*` wildcard).
5. If the app is part of `Apps/BUILDALL/BUILDALL.SUB`, update that line too.
6. Build end-to-end in real CP/M via the `altair-cpm-build` MCP server. Use
   `build_app` with `app: "<app>"` for apps whose `.SUB` ends with
   `mcpdone <app>` (the `MCP-TOOL-COMPLETED <APP>` marker is the only success
   signal), or `run_submit` for `BUILDALL.SUB`. Apps with no `mcpdone` marker
   (e.g. `AUTORUN`) will assemble and produce a `.COM`, but `build_app` reports
   a timeout because there is no completion marker — that is expected, not a
   build failure.

## Build And Test In CP/M Via The MCP Server

This repo ships an MCP server at `altair_mcp_server/` that boots the Altair 8800
emulator into CP/M 2.2 and exposes build tools. Drives:

- `A:` `altair_mcp_server/disks/cpm63k.dsk` (CP/M boot)
- `B:` `altair_mcp_server/disks/bdsc-v1.60.dsk` (MAC, LOAD, CC, CLINK, FT,
  MCPDONE, SuperSUB — note: no ASM.COM, MAC is the only assembler)
- `C:` / `D:` blank scratch disks

`FT` (`ft -g ...`) serves files from this repo's `Apps/` directory. Use
`build_app` for the normal edit/build loop and `run_submit` for `BUILDALL.SUB`.
Treat `MCP-TOOL-COMPLETED <NAME>` as the only success signal.

The MCP console path is good for smoke tests with printable characters, Enter,
Tab, and ordinary command text. Some control bytes are swallowed or transformed
by host flow control / JSON escaping (`Ctrl-Q`/XON and sometimes ESC or `Ctrl-F`),
so do not treat failure to inject those keys as proof that the CP/M program is
broken. Use source-level review or an interactive terminal for those paths.
