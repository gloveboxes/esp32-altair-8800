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

7. If host-compiling for syntax, treat modern compiler K&R warnings as expected, but fix real syntax errors. Host compilers do not enforce all BDS C rules.

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

## Validation

Run `scripts/check_bds_c.py` on each changed BDS C source/header file. The script strips comments and literals, then reports:

- identifiers longer than 7 characters
- first-7-character collisions
- lowercase or mixed-case `.c`/`.h` filenames
- likely casts
- declarations after statements inside functions
- `//` comments and common unsupported keywords

Use it as a guardrail, not a substitute for compiling with BDS C.
