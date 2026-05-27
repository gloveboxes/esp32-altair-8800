# Altair Local Runner

`altair-local` is a host build of the Altair 8800 emulator for quick CP/M app
testing in a local terminal. It uses the universal 88-DCDD disk controller and
the same `intel8080.c` / `memory.c` core as the ESP32 firmware. Terminal I/O
runs through stdio and the file transfer / time / utility / chat ports use
host-side port drivers. Boots from the disk images in the project `disks/`
folder by default.

## Build

```sh
cmake -S local_altair -B local_altair/build
cmake --build local_altair/build
```

## Run

```sh
./local_altair/build/altair-local
```

There is also a VS Code task: **Altair: Start Local Altair** (configure +
build + run).

The default disks match the ESP32 firmware (see `altair8800/esp32_88dcdd_sd_card.h`):

```text
A: disks/cpm63k.dsk
B: disks/bdsc-v1.60.dsk
C: disks/escape-posix.dsk
D: disks/blank.dsk
```

Disk images are opened read/write, so CP/M writes update the files in place.
Point at alternate images with `--drive-a`, `--drive-b`, `--drive-c`,
`--drive-d`.

File transfer uses the repo `Apps/` folder by default, so inside CP/M you can
do (for example):

```text
B:
FT -G BREAKOUT/BREAKOUT.SUB
SUBMIT BREAKOUT
```

Press `Ctrl-]` to exit the runner and restore the terminal.

## OpenAI chat port

If libcurl is installed and `CHAT_OPENAI_KEY` is set in `altair_env.txt`, the
chat port (120-124) will be enabled. Without libcurl the chat port returns an
error string.
