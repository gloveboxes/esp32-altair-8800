---
name: bds-c
description: Write, edit, and review BDS C 1.6 code for CP/M/Altair-style projects. Use when working on .C, .H, .SUB, or assembly-adjacent code intended for the BD Software C compiler, especially when the task mentions BDS C, CP/M, Altair 8800, VT100 terminal apps, emulator ports, SDK helpers, or compiler quirks such as 7-character symbols, no casts, K&R declarations, and no block-local declarations.
---

# BDS C

## Core Rules

Treat BDS C as its own language, not as old ANSI C.

- Keep every identifier at 7 characters or less: globals, locals, parameters, functions, labels, macros, struct tags, members, and preprocessor symbols.
- Also ensure no two identifiers collide in their first 7 characters.
- Name BDS C source/header files in all caps, including the extension: `FOO.C`, `FOO.H`.
- Do not use casts. Rewrite with temporary variables, helper functions, or compatible types.
- Use K&R function definitions:

```c
int fn(a, b)
int a;
char *b;
{
    return 0;
}
```

- Declare formal parameters immediately after the function parameter list and before `{`.
- Declare function-local variables only at the start of the function body. BDS C has no block-local variable scope.
- Explicitly type all external/global data definitions.
- Do not use ANSI prototypes, `void`, `typedef`, `const`, `enum`, `signed`, `long long`, `static inline`, `//` comments, or mixed declarations and statements.
- Do not rely on C library headers unless the project already does. Prefer existing project patterns.
- Keep function calls parenthesized, including no-argument calls: `fn()`, never `fn`.
- Avoid side effects in function-call arguments. BDS C evaluates arguments in reverse order.
- Parenthesize constant arithmetic when used inside larger expressions.

## Workflow

1. Read nearby working code first. Match the repo's successful BDS C style over modern preferences.
2. Check included headers and linked modules for 7-character hazards. If a public helper name exceeds 7 characters, either use an already-working project pattern or write a tiny local helper with a short name.
3. Implement with short, distinct names from the start. Renaming later is where breakage sneaks in.
4. Avoid clever expressions. Prefer simple statements and small helpers.
5. Update `.SUB` files when dependencies change.
6. Run the identifier checker on each BDS C source/header file you changed before finalizing:

```bash
python3 .codex/skills/bds-c/scripts/check_bds_c.py path/to/file.c
```

For multiple changed files, pass them all in one command.

7. After the linter passes, build the app end-to-end in the real BDS C 1.6 toolchain by calling the `altair-cpm-build` MCP server's `build_app` tool with `app: "<app>"` (or `run_submit` for non-`<APP>/<APP>.SUB` workflows). Treat `MCP-TOOL-COMPLETED <APP>` as the only success signal. Only flash the ESP32 firmware after this host-side build passes. See the "Build And Test In CP/M Via The MCP Server" section below.

8. If host-compiling for syntax, treat modern compiler K&R warnings as expected, but fix real syntax errors. Host compilers do not enforce all BDS C rules.

Optional host syntax check:

```bash
gcc -x c -std=gnu89 -Wno-implicit-function-declaration -Wno-deprecated-non-prototype -Wno-incompatible-library-redeclaration -fsyntax-only path/to/file.c
```

Use this only as a rough extra check. Modern GCC/clang may reject BDS-style project headers with untyped parameter lists such as `int x_curmv(row, col);`; in that case either skip the host check or use temporary host-only shim headers. Do not rewrite working BDS headers just to satisfy a host compiler.

## Terminal And Emulator Code

For Altair/VT100 apps:

- Prefer simple BIOS/BDOS wrappers with names under 7 chars, such as `outch`, `pstr`, `cur`, `cls`, `hide`, `show`, `rst`.
- Use literal VT100 escape sequences when that avoids long SDK names.
- Keyboard and timer ports are often accessed directly in these apps; keep helper names short and document port choices briefly.
- If using SDK functions, verify the symbol names are safe for the compiler and linker in that build path.

## Common Hazards

- `x_tmrset` and `x_tmract` are 8 characters. In strict BDS C work, prefer a local `tset`/`tact` wrapper or direct port access.
- `x_setcol`, `x_rstcol`, and similar SDK names are also over 7 characters.
- Struct member names share a constrained namespace. Avoid reusing member/tag names unless matching the compiler's exact allowed case.
- There is no `extern` keyword behavior like modern C. Multi-file external data layout must match exactly and all external variables used by a program must be declared in the source file containing `main`.
- Do not place `#include` inside conditional compilation blocks; BDS C processes includes before conditionals.

## STDIO.H Must Be The First Include In Every Source File

BDS C externals work like FORTRAN COMMON: variables are matched by **offset across all linked source files**, not by name. `STDIO.H` declares `struct _header _base;` and `struct _header *_allocp;` as externals; `_allocp` is used internally by `alloc`/`free`, and `fopen` calls `alloc` to allocate its file buffer.

If any `.c` file in a multi-file program does not include `stdio.h` first, that file's external area starts with different variables, shifting `_allocp` to a different offset. The linker silently produces a broken `.com` where `fopen` returns NULL and `alloc` corrupts memory.

Rule (BDS C v1.6 manual, page 26 caveat 3 and page 84):

- `#include "stdio.h"` MUST be the very first `#include` in EVERY `.c` file of the program, even if that file does not call any stdio function.
- No data declarations may physically precede it.
- The source file containing `main` must declare every external used anywhere in the program.

Classic symptom: `fopen("somefile", "r")` returns NULL even though the file exists and CP/M `TYPE somefile` works (TYPE goes straight to BDOS and never touches `alloc`). The bug appears identically on host emulators and ESP32 because the broken `.com` binary is the same on every platform.

When creating or editing any BDS C `.c` file, ensure line 1 (after the comment header) is `#include "stdio.h"`.

## 32-bit Longs: LONG.C

BDS C 1.6 has no native `long` type (and `long long` is banned). For 32-bit signed and unsigned integer math, use the `LONG.C` package at `Apps/SDK/LONG.C` (Rob Shostak, 1982). It is the canonical 32-bit helper in this repo.

A "long" is a 4-byte `char[4]` array, big-endian (byte 3 is least significant). You declare storage yourself (`char total[4];`) and pass pointers to the helpers. All function names are already 7 characters or less, so they are safe to call directly from BDS C.

Signed operations:

- `itol(result, n)` — int -> long.
- `ltoi(l)` — long -> int (low 16 bits).
- `lassign(dest, src)` — copy a long.
- `lcomp(op1, op2)` — compare; returns >0, 0, <0.
- `ladd` / `lsub` / `lmul` / `ldiv` / `lmod (result, op1, op2)` — arithmetic; each returns `result`.
- `atol(result, s)` — ASCII string -> long.
- `ltoa(result, op1)` — long -> ASCII string (`result` must hold at least 12 chars).

Unsigned operations:

- `utol(l, u)` — unsigned -> long.
- `ltou(l)` — long -> unsigned (truncates to low 16 bits).

Usage notes:

- Every helper that produces a long takes the destination as its first argument and returns it, so calls chain: `ladd(sum, lmul(sum, sum, ten), itol(t, d));`.
- There is no `LONG.H`; declare the helpers you call (they return `char *` / `int`) or rely on BDS C's implicit `int` for the integer-returning ones. Keep `#include "stdio.h"` first regardless.
- In a submit file, fetch and compile `LONG.C` before the app that uses it, then link it in: `cc long`, then `clink <app> long`. See the submit-file template below, which already lists `ft -g file://sdk/long.c`.

## Build And Test In CP/M Via The MCP Server

This repo ships an MCP server at `altair_mcp_server/` that boots the Altair 8800 emulator into CP/M 2.2 on the host and exposes four tools to MCP clients:

- `build_app` — one-shot end-to-end build. Resets fresh disks, switches to `B:`, fetches `Apps/<app>/<app>.sub` over FT, runs `submit <app>`, and stops on `MCP-TOOL-COMPLETED <APP>`. Use this for the normal edit/build loop on apps with an `Apps/<APP>/<APP>.SUB` driver.
- `run_submit` — same as `build_app` but for arbitrary submit files such as `BUILDALL.SUB`. Supports a custom `fetch` path and `marker`.
- `run_cpm` — sends terminal text to the live CP/M session and returns output. Session state (disks, memory) persists between calls until `reset`. Newlines become carriage returns. Empty input advances SuperSUB one step.
- `reset` — restores pristine disks and reboots to `A>`.

Drives mounted by the server:

- `A:` `altair_mcp_server/disks/cpm63k.dsk` (CP/M boot)
- `B:` `altair_mcp_server/disks/bdsc-v1.60.dsk` (BDS-C, CLINK, FT, MCPDONE, SuperSUB)
- `C:` `altair_mcp_server/disks/blank.dsk`
- `D:` `altair_mcp_server/disks/blank_d.dsk`

Pristine sources for `reset` live in `altair_mcp_server/pristine/`. FT (`ft -g ...`) serves files from this repo's `Apps/` directory.

Build the server once after pulling:

```bash
cmake -S altair_mcp_server -B altair_mcp_server/build
cmake --build altair_mcp_server/build
```

The server is registered in `.vscode/mcp.json` as `altair-cpm-build`. Inside VS Code, prefer the MCP tools over hand-driving the emulator:

- New or changed app `<app>` with an `Apps/<APP>/<APP>.SUB` file: call `build_app` with `app: "<app>"`. Typical builds complete in well under a second; pass `verbose: false` for a compact summary.
- Multi-app or custom workflow: call `run_submit` with the submit base name (and optional `fetch` / `marker`).
- Interactive debugging at the CP/M prompt: call `run_cpm` with text input. Build pattern from a fresh `A>`:

  1. `run_cpm` input `b:\nft -g <app>/<app>.sub`
  2. `run_cpm` input `submit <app>`
  3. `run_cpm` with empty input repeatedly until `MCP-TOOL-COMPLETED <APP>` appears.

End every submit file with `mcpdone <app>` so `MCPDONE.COM` emits the stable `MCP-TOOL-COMPLETED <APP>` completion marker. Build/submit results also include elapsed time in milliseconds, for example `BUILD RESULT: PASS (530 ms) - MCP-TOOL-COMPLETED BREAKOUT`.

Guidance for using the tools while iterating on BDS C code:

- Run `check_bds_c.py` first, then `build_app`. The MCP server compiles with the real BDS C 1.6 toolchain, so it catches anything the static checker cannot.
- Treat any line containing `?` at the CP/M prompt (`SUBMIT?`, `CC?`, `CLINK?`, `FT?`, `MCPDONE?`) as failure and read the transcript above it.
- Do not call `reset` between dependent `run_cpm` steps; it discards any files created on the working disks.
- The MCP server is independent of the ESP-IDF build. It is the fastest correctness check for BDS C app changes; flash the firmware only after the host-side CP/M build passes.

## FT, SUBMIT, And SuperSUB Reference

These three CP/M utilities drive every `Apps/<APP>/<APP>.SUB` build. Knowing how they interact is essential when writing or fixing a submit file.

### FT — file transfer from the host

`FT.COM` lives on `B:` and talks to the MCP server's file-transfer endpoint. It pulls files from this repo's `Apps/` tree onto the current CP/M drive.

```
B>ft -g <relative/path>           ; fetch a file, save under its basename
```

- Path is resolved against `Apps/` in the repo. `ft -g sheets/sheets.c` fetches `Apps/SHEETS/SHEETS.C`.
- A `file://` prefix is allowed and ignored. `ft -g file://sdk/long.c` works the same as `ft -g sdk/long.c`.
- FT writes to the **current** drive. Always `b:` before fetching build sources so the BDS C toolchain finds them.
- FT overwrites existing files silently. If a previous run left a stale copy on `B:`, the fresh fetch replaces it.
- FT prints `Done (<bytes> bytes)` on success. A `FT?` line at the prompt means the binary is missing or the path is wrong.
- FT cannot create directories. Plan filenames to fit CP/M's 8.3 limit (BDS C `.C`/`.H`/`.CRL` already do).

### SUBMIT and SuperSUB — driving a build script

`SUBMIT.COM` reads a `.SUB` file and queues each line as the next console input. The repo uses **SuperSUB V1.1** (a hardened variant on `B:`) which prints `SuperSUB V1.1` at startup and advances one command per CP/M prompt.

```
B>submit <name>                   ; runs <name>.SUB from the current drive
```

- Always `b:` first, then `ft -g <app>/<app>.sub` to pull the latest submit file from the repo, then `submit <app>`. The MCP `build_app` tool follows exactly this sequence.
- Each non-blank, non-comment line is fed at the next prompt. Blank lines are skipped. Lines starting with `;` are comments.
- Drive prefixes work: `a:pip a:foo.com=foo.com` runs `PIP` from `A:` against the current drive's `FOO.COM`.
- A `SUB?` or any `?` at the prompt is fatal: the queue stops and the MCP tool reports `BUILD RESULT: FAIL`.
- SuperSUB does not handle interactive prompts. Every command in the script must complete without waiting for keystrokes.
- The MCP server advances SuperSUB by calling `run_cpm` with empty input. `build_app` and `run_submit` do this automatically.

### Required completion marker

Every submit file driven by the MCP server **must** end with:

```
mcpdone <name>
```

`MCPDONE.COM` (on `B:`) prints `MCP-TOOL-COMPLETED <NAME>` in uppercase. `build_app` and `run_submit` poll for that exact string and stop the moment it appears. Without it, the build always times out even when the binary built correctly. Do not put extra commands after `mcpdone`.

### Submit file template

```
; Apps/FOO/FOO.SUB — driver for the FOO app
ft -g file://foo/foo.c            ; source
ft -g file://sdk/long.c           ; any SDK helpers

cc long                           ; compile dependencies first
era long.c

cc foo                            ; compile the app
clink foo long                    ; link
era a:foo.*                       ; clear stale copy on A: before PIP
a:pip a:foo.com=foo.com           ; install on A:
era foo.*                         ; tidy the build drive
era long.*
mcpdone foo                       ; REQUIRED completion marker
```

### PIP-to-A pitfall

`PIP` does not overwrite existing files cleanly when `A:` is tight on directory entries or free space — it leaves a `$$$` temp file and fails with `DISK WRITE ERROR`. Always erase first:

```
era a:foo.*
a:pip a:foo.com=foo.com
```

Apply this rule for every `a:pip a:<app>.<ext>=...` step in `BUILDALL.SUB` and any per-app submit. This was the cause of the 2026-05 buildall failure at the `BREAKOUT` step.

### Submit-file authoring checklist

- File lives at `Apps/<APP>/<APP>.SUB`, all uppercase.
- Filenames inside the script are lowercase or uppercase — CP/M is case-insensitive — but stay consistent with the rest of the repo.
- First action after `submit` is usually `ft -g file://<app>/<app>.c` to pull the latest source.
- Compile dependencies before the main translation unit; erase each `.C` after `CC` to free B: space.
- Erase `STRING.H`, `LONG.H`, etc. on `B:` after they are no longer needed by later `CC` steps.
- Before any `a:pip a:<app>.<ext>=...`, insert `era a:<app>.*`.
- After PIP, `era <app>.*` (and any imported sources) on the build drive so the next run starts clean even without `reset`.
- Final line is `mcpdone <app>`. Nothing else.

### Common transcript symptoms

| Symptom in transcript                          | Likely cause                                                                 |
|------------------------------------------------|------------------------------------------------------------------------------|
| `Write error` during `CC <name>`               | `B:` is full. Erase intermediates earlier, or slim the pristine B: image.    |
| `No main function in 0/B:<NAME>.CRL`            | Previous `CC` failed (often due to `Write error`); `.CRL` is the 0-byte stub. |
| `DISK WRITE ERROR: =<NAME>.COM`                 | Missing `era a:<name>.*` before `a:pip`.                                     |
| `SUB?` / `CC?` / `CLINK?` / `FT?` / `MCPDONE?` | Command not found on the current drive, or argument is wrong.                |
| Build times out, no `MCP-TOOL-COMPLETED <APP>`  | Submit file is missing `mcpdone <app>` at the end.                           |
| `build_app` reports stale behavior             | A submit baked into pristine `B:` is overriding the repo copy. Either remove it from the pristine `bdsc-v1.60.dsk` or call `run_submit` with `fetch: "<app>/<app>.sub"` to force a fresh pull. |

## Validation

Run `scripts/check_bds_c.py` on each changed BDS C source/header file. The script strips comments and literals, then reports:

- identifiers longer than 7 characters
- first-7-character collisions
- lowercase or mixed-case `.c`/`.h` filenames
- likely casts
- declarations after statements inside functions
- `//` comments and common unsupported keywords

Use it as a guardrail, not a substitute for compiling with BDS C.
