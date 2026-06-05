# Altair Apps

A collection of games and utilities that run under CP/M on the Altair 8800
emulator and firmware. Every app in this catalog was **vibe coded** with
[Claude](https://www.anthropic.com/claude){:target=_blank} and OpenAI
foundation models, using a purpose-built toolchain for retro 8080 development:

- **BDS C apps** were generated with the `bds-c` skill, validated with the
  `check-bds-c.py` checker, and built and run end-to-end on the
  `altair_cpm_build` MCP server.
- **MAC assembly apps** were generated with the `mac-asm` skill, validated
  with the `check_mac_asm.py` checker, and built and run on the same
  `altair_cpm_build` MCP server.

This combination lets a foundation model write, lint, compile, link, and test
authentic Altair 8800 software without leaving the editor. Several apps ship in
both a BDS C (`.C`) and a MAC assembler (`.ASM`) implementation with identical
behavior.

!!! tip "Built-in help"
    Most apps surface their own help. Pass `-H` (or `/?`) on the command line
    for command-line apps, and press the in-app help key (often `Ctrl-W`) once
    a full-screen app is running. The help available in each app is reproduced
    below.

## Apps in this catalog

- [Breakout](#breakout) — brick-breaker arcade game
- [Chat](#chat) — OpenAI / compatible chat client
- [Clock](#clock) — 7-segment clock with weather panel
- [Edit](#edit) — WordStar-style full-screen text editor
- [ENV](#env) — environment-variable manager
- [FT (File Transfer)](#ft-file-transfer) — download files from a Remote FT Server
- [Mancala](#mancala) — Kalah-style bean game vs. computer
- [Sheets](#sheets) — tiny VT100 spreadsheet
- [Snake](#snake) — classic snake game
- [Tetris](#tetris) — full Tetris engine
- [TicTac](#tictac) — 5x5 Tic-Tac-Toe vs. computer

---

## Breakout

Classic brick-breaker rendered on a VT100/xterm.js terminal.

**Run**

```text
A> BREAKOUT
```

**Controls**

| Key                | Action        |
|--------------------|---------------|
| Left/Right arrows  | Move paddle   |
| Space              | Launch ball   |
| ESC or Ctrl-C      | Quit          |

---

## Chat

A CP/M client for the OpenAI Chat Completions API and any OpenAI-compatible
endpoint (Ollama, llama.cpp server, LM Studio, vLLM, ...). It talks to the host
over Intel 8080 I/O ports, backed by libcurl on the desktop emulator and the
ESP-IDF HTTP client on the ESP32 firmware.

**Run**

```text
A> CHAT          start a chat session
A> CHAT -H       show command-line help
```

**Configuration** is read from the Altair env store via the [ENV](#env) app.
Set values with `ENV NAME=value`; they are read fresh each time `CHAT` starts.

```text
A> ENV CHAT_PROVIDER=openai
A> ENV CHAT_OPENAI_KEY=sk-proj-xxxxxxxxxxxx
A> ENV CHAT_MODEL=gpt-4o-mini
A> CHAT
```

OpenAI-compatible (e.g. Ollama on the LAN):

```text
A> ENV CHAT_PROVIDER=compatible
A> ENV CHAT_ENDPOINT=http://192.168.1.20:11434/v1/chat/completions
A> ENV CHAT_MODEL=gemma3:1b
A> CHAT
```

| Variable           | Default     | Purpose                                  |
|--------------------|-------------|------------------------------------------|
| `CHAT_PROVIDER`    | —           | `openai` or `compatible`                 |
| `CHAT_ENDPOINT`    | —           | Full URL when `CHAT_PROVIDER=compatible` |
| `CHAT_OPENAI_KEY`  | —           | Bearer token for `openai`                |
| `CHAT_MODEL`       | `gemma3:1b` | Model name passed to the API             |
| `CHAT_MAX_TOKENS`  | `1024`      | `max_tokens` in the request              |
| `CHAT_TEMPERATURE` | `0.7`       | Sampling temperature                     |

**In-chat commands**

```text
Commands: /help (/?)  /history (/h)  /reset (/r)  /quit (/q)
```

---

## Clock

A 70s-style 7-segment psychedelic clock that reads the ESP32 local wall clock
and shows the time as chunky VT100 character blocks, with the colon blinking
every second. It also displays the current date and an optional weather panel.

**Run**

```text
A> CLOCK         start the clock
A> CLOCK -H      show command-line help
```

Press **ESC** or **Ctrl-C** to quit.

**Configuration** (set with the [ENV](#env) app):

```text
A> ENV UTC_OFFSET=10.0     UTC offset in hours (e.g. 8.5, -8.5)
```

Restart the ESP32/emulator after changing `UTC_OFFSET`; the offset is read once
at startup and cached by the firmware.

---

## Edit

A full-screen WordStar-style text editor for CP/M.

**Run**

```text
A> EDIT filename
```

Press **Ctrl-W** inside the editor at any time to show the help screen.

**Keys**

| Key          | Action                  |
|--------------|-------------------------|
| Arrow keys   | Move cursor             |
| Ctrl-R / V   | Page up / Page down     |
| Ctrl-T / B   | Go to top / bottom      |
| Ctrl-O       | Write file to disk      |
| ESC / Ctrl-Q | Exit editor             |
| Ctrl-K       | Cut current line        |
| Ctrl-C       | Copy current line       |
| Ctrl-U       | Paste (uncut) line      |
| Ctrl-F       | Find                    |
| Ctrl-N       | Find next               |
| Ctrl-W       | Help                    |

---

## ENV

The environment-variable manager for CP/M. Variables are stored on the ESP32
(or in `altair_local/altair_env.txt` on the host emulator) and shared by other
apps such as [Chat](#chat) and [Clock](#clock). The command line is parsed by
the firmware/host driver, which supports value copying, arithmetic, and
increment/decrement operators in addition to plain set/get.

**Run**

```text
A> ENV                  enter interactive mode (no arguments)
A> ENV -H               show the built-in help (also HELP or ?)
```

**Commands** (from the built-in `ENV -H` help)

| Command              | Action                                          |
|----------------------|-------------------------------------------------|
| `ENV`                | List all variables                              |
| `ENV NAME`           | Show `NAME`                                      |
| `ENV NAME=VALUE`     | Set `NAME` to `VALUE`                            |
| `ENV NAME=VALUE TWO` | Set `NAME` to text with spaces                   |
| `ENV NAME=OTHER`     | Copy value from `OTHER` if `OTHER` exists        |
| `ENV NAME=A+B`       | Store numeric `A` plus `B`                       |
| `ENV NAME=A-B`       | Store numeric `A` minus `B`                      |
| `ENV NAME+N`         | Add `N` to numeric `NAME` (default 0)            |
| `ENV NAME-N`         | Subtract `N` from numeric `NAME`                 |
| `ENV NOW`            | Show emulator uptime in seconds                  |
| `ENV -I NAME=VALUE`  | Set only if `NAME` is undefined                  |
| `ENV -D NAME`        | Delete `NAME`                                     |
| `ENV -N`             | Show variable count                              |
| `ENV -H` / `HELP` / `?` | Show the built-in help                        |

**Operators**

The `+` and `-` operators work on the numeric value of a variable. The
left-hand side may be a number, another variable name, or `NOW`; the
increment/decrement form treats a missing variable as `0`.

```text
A> ENV COUNT=0          set COUNT to 0
A> ENV COUNT+1          increment COUNT by 1  -> COUNT=1
A> ENV COUNT+10         add 10                -> COUNT=11
A> ENV COUNT-10         subtract 10           -> COUNT=1
A> ENV TOTAL=COUNT+5    store COUNT plus 5    -> TOTAL=6
A> ENV ELAPSED=NOW-START  difference of two numeric vars
```

!!! note
    Spaces around `=`, `+`, and `-` are optional, so `ENV COUNT + 1`,
    `ENV COUNT +1`, and `ENV COUNT+1` are equivalent. Arithmetic requires
    numeric values — applying an operator to non-numeric text returns an
    error. Names are limited to 16 characters (stored uppercase) and values
    to 127 characters.

**Interactive mode** (run `ENV` with no arguments) loads the variables from the
ESP32 and presents a menu:

```text
[number]=edit  A=add  D <n>=delete  N=next  P=prev  L=reload  R=reboot  Q=quit
```

Values are edited at a prompt that preserves mixed case.

---

## FT (File Transfer)

Downloads files from a Remote FT Server into CP/M. Ships as both a BDS C
(`FT.C`) and a MAC assembler (`FT.ASM`) build with identical behavior.

**Run**

```text
A> ft -g  <filename>    text file: normalize line endings to CP/M CR LF, add ^Z
A> ft -gb <filename>    binary file: store bytes verbatim, no ^Z
```

- Use `-g` for source and text files (`.C`, `.ASM`, `.SUB`, `.H`, ...). Incoming
  CR is stripped and each LF expanded to CR LF, then a single `^Z` end-of-text
  marker is written.
- Use `-gb` for binary files (`.COM`, images, archives, ...). Bytes are written
  exactly as received; the final partial 128-byte record is padded with NUL.

**Example**

```text
A> ft -g test.txt
Downloading 'test.txt' from Remote FT Server...
Done (84 bytes)
```

Requires the Altair emulator with WiFi/Remote FT support and a Remote FT Server
running on the configured IP address:8090.

---

## Mancala

The African bean game (Kalah-style rules), player versus computer, on a
VT100/xterm.js display.

**Run**

```text
A> MANCALA
```

**Controls**

| Key                | Action                      |
|--------------------|-----------------------------|
| E / M / H          | Easy / Medium / Hard        |
| Left/Right arrows  | Select pit                  |
| 1-6                | Select pit                  |
| Space or Return    | Sow selected pit            |
| U                  | Undo last player move       |
| ? or T             | Hint                        |
| Q, ESC, Ctrl-C     | Quit                        |

---

## Sheets

A tiny VT100 spreadsheet for CP/M. The grid is 26 columns (`A`..`Z`) by 99
rows, with 7 columns x 20 rows visible and scrolling. Cells hold text, an
integer, or a formula, with 32-bit signed integer arithmetic.

**Run**

```text
A> SHEETS
```

Press **Ctrl-W** for in-app help.

**Keys**

| Key            | Action                                |
|----------------|---------------------------------------|
| Arrow keys     | Move cursor                           |
| Enter          | Edit current cell (keep contents)     |
| Any printable  | Start a fresh edit with that char     |
| Backspace      | (in edit) delete left                 |
| ESC            | (in edit) cancel; (in nav) quit       |
| Ctrl-K         | Clear current cell                    |
| Ctrl-O         | Write file                            |
| Ctrl-L         | Reload file                           |
| Ctrl-G         | Go to cell (e.g. `C12`)               |
| Ctrl-W         | Help                                  |
| Ctrl-Q         | Quit                                  |

**Formulas** start with `=`. Supported: integers and unary minus, binary
`+ - * /` with precedence, parentheses, cell references (`=A1+B2*3`), and range
functions over a rectangular block — `SUM`, `AVG`, `MIN`, `MAX`, `COUNT`
(e.g. `=SUM(A1:B5)`). Errors render as `#ERR`.

---

## Snake

The classic snake game. Collect food (`*`) to grow longer and raise your score;
don't hit the walls or yourself.

**Run**

```text
A> SNAKE
```

**Controls**

| Key          | Action            |
|--------------|-------------------|
| Arrow keys   | Change direction  |
| ESC / Q      | Quit              |

---

## Tetris

A full Tetris engine with a 7-bag piece supply, wall-kick rotation, soft drop,
hard drop, scoring, and levels, on a bright block-terminal theme.

**Run**

```text
A> TETRIS
```

**Controls**

| Key           | Action      |
|---------------|-------------|
| Left/Right    | Move        |
| Up            | Rotate      |
| Down          | Soft drop   |
| Space         | Hard drop   |
| ESC / Q       | Quit        |

---

## TicTac

5x5 Tic-Tac-Toe — get four in a row to win. Player (X) versus computer (O) with
a heuristic AI on a colorful VT100 cabinet display.

**Run**

```text
A> TICTAC
```

**Controls**

| Key          | Action          |
|--------------|-----------------|
| Arrow keys   | Move cursor     |
| Space        | Place piece     |
| Q / ESC      | Quit            |
