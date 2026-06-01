# Altair FT (File Transfer) App

A CP/M application that downloads files from a Remote FT Server. Two
feature-for-feature implementations live in this folder:

- `FT.C` - the original BDS C source.
- `FT.ASM` - an 8080 macro-assembler (MAC) port with identical behaviour.

## Building

BDS C:

```
A> cc ft.c
A> clink ft
```

MAC (macro assembler):

```
A> mac ft
A> load ft
```

`FT.SUB` builds the assembler version end-to-end (fetch, `mac`, `load`, copy
to `A:`, clean up) for the CP/M MCP build server.

## Usage

```
A> ft -g  <filename>    text file: normalize line endings to CP/M CR LF, add ^Z
A> ft -gb <filename>    binary file: store bytes verbatim, no ^Z
```

Use `-g` for source and text files (`.C`, `.ASM`, `.SUB`, `.H`, ...). It strips
any incoming CR and expands each LF to CR LF, so the saved file always has
canonical CP/M line endings whether the source used LF-only or CRLF, then
writes a single `^Z` end-of-text marker. This is what `SUBMIT`, `MAC`, and
`CC` expect.

Use `-gb` for binary files (`.COM`, images, archives, ...). Bytes are written
exactly as received with no `^Z`; the final partial 128-byte record is padded
with NUL (`00H`).

## Examples

```
A> ft -g test.txt
Downloading 'test.txt' from Remote FT Server...
Done (84 bytes)

A> ft -g subdir/hello.c
Downloading 'subdir/hello.c' from Remote FT Server...
Saving as 'hello.c'
Done (256 bytes)

A> ft -gb prog.com
Downloading 'prog.com' from Remote FT Server...
Done (3072 bytes)
```

## Protocol

The FT app uses I/O ports 60 and 61 to communicate with the firmware:

- **Port 60 (OUT)**: Command port - sends commands to start transfer, request chunks, close
- **Port 60 (IN)**: Status port - returns transfer status (IDLE, DATAREADY, EOF, ERROR)
- **Port 61 (OUT)**: Data port - sends filename characters
- **Port 61 (IN)**: Data port - reads file data bytes

## Requirements

- Altair emulator with WiFi and Remote FT support
- Remote FT Server running on the configured IP address:8090
