# Altair Local Runner

`altair-local` is a host build of the Altair 8800 emulator for quick CP/M app
testing in a local terminal. It uses the universal 88-DCDD disk controller and
the same `x80.cxx` / `memory.c` core as the ESP32 firmware. Terminal I/O
runs through stdio by default (or, with `--web`, through the same browser
terminal the ESP32 firmware serves) and the file transfer / time / utility /
chat ports use host-side port drivers. Boots from the disk images in the
project `disks/` folder by default.

## Build

```sh
cmake -S altair_local -B altair_local/build
cmake --build altair_local/build
```

The browser terminal (`--web`, below) is backed by the
[wsServer](https://github.com/Theldus/wsServer) git submodule under
`altair_local/external/wsServer`. If you cloned without `--recurse-submodules`,
fetch it once with:

```sh
git submodule update --init altair_local/external/wsServer
```

When the submodule is absent (or on Windows) the build still succeeds; `--web`
is simply unavailable and the runner uses the stdio terminal.

## Run

```sh
./altair_local/build/altair-local
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

## Browser terminal (`--web`)

Instead of the stdio terminal, the runner can serve the project's web terminal
(`terminal/index.html`, the same UI the ESP32 firmware uses) over HTTP and
bridge it to the emulator over WebSocket:

```sh
./altair_local/build/altair-local --web        # default port 8080
./altair_local/build/altair-local --web 9000    # custom port
```

Then open `http://localhost:8080/` in a browser. The emulator waits to boot
until the first browser connects, so the CP/M banner is delivered to the page
(matching the ESP32 behaviour). `Ctrl+]` in the browser exits the emulator;
`Ctrl+C` in the launching shell stops the server.

How it works: a small built-in HTTP server on the chosen port serves the
terminal HTML and reverse-proxies WebSocket upgrade requests (the page connects
to `ws://<host>:<port>/ws`) to a loopback [wsServer](https://github.com/Theldus/wsServer)
instance on port + 1. Keeping both behind one public port means `index.html`
needs no changes — it derives the WebSocket URL from the page's own host/port.
`--web` is POSIX-only; on Windows the runner falls back to the stdio terminal.

## CP/M 2.2 vs CP/M 3

By default the emulator boots **CP/M 2.2** (`disks/cpm63k.dsk`). To boot
**CP/M 3** (CP/M Plus, non-banked) instead, pass `--cpm3`:

```sh
./altair_local/build/altair-local --cpm3
```

`--cpm3` selects the DeRamp / Mike Douglas Altair CP/M 3 builds:

```text
A: disks_cpm_3/cpm3_56k_disk1.dsk   (bootable 56K system: CPM3.SYS, CCP, BDOS3, BIOS3)
B: disks_cpm_3/cpm3_56k_disk2.dsk   (utilities / HELP)
```

The cpm3 system disks live in `disks_cpm_3/` rather than `disks/` so they are
not bundled into the ESP32 storage-flash image (which packages everything in
`disks/`). The `--cpm3` profile also maps C: and D: from `disks_cpm_3/`
(`bdsc-v1.60.dsk` and `cpm63k.dsk`). Explicit `--drive-a` / `--drive-b`
override the `--cpm3` defaults. Both CP/M versions use the same Altair 88-DCDD 8" floppy controller and disk
boot ROM — no emulator changes are needed; CP/M 3 just ships its own cold-boot
loader, `CPMLDR`, and `CPM3.SYS` on the system disk. Source for the BIOS3 and
loaders
lives in `reference/cpm3_deramp/`.

The CP/M 3 disk images were downloaded from the DeRamp / Mike Douglas archive at
<https://deramp.com/downloads/altair/software/8_inch_floppy/CPM/CPM%203.0/>
(`cpm3_v1.0_56K_disk1.dsk` and `cpm3_v1.0_56k_disk2.dsk`, renamed to
`cpm3_56k_disk1.dsk` / `cpm3_56k_disk2.dsk`); a pristine mirror is kept in
`reference/cpm3_deramp/`.

### Setting the CP/M 3 memory footprint (GENCPM + COPYSYS)

CP/M 3 is generated with `GENCPM`, which decides how much of the 64K address
space the system (BIOS3 + BDOS3) occupies and therefore how big the TPA is.
The stock DeRamp build tops out around **41K TPA**. Because this emulator has a
flat 64K RAM and only uses the boot loader region (`FF00`) during cold boot, you
can reclaim it and push the non-banked TPA to about **49K** — the non-banked
ceiling, since the system code permanently occupies high memory.

`GENCPM` lives on the build disk, not the runtime A:/B: disks. Boot the build
disk as A::

```sh
./altair_local/build/altair-local --cpm3 \
  --drive-a reference/cpm3_deramp/cpm3_v1.0_56k_build.dsk
```

Then regenerate the system:

```text
A>SUBMIT GENCPM FF
```

`FF` sets the top page of memory to `0xFF00` (reclaiming the boot-loader area).
Or run `A>GENCPM` interactively and answer:

- Top page of memory: **FF**
- Bank switched system: **N**
- Double allocation vectors: **N**
- accept the remaining defaults

After this you should see the map `BIOS3 SPR E600`, `BDOS3 SPR C700`, giving
~49K TPA. `GENCPM` only writes the new `CPM3.SYS` to A:; it does not make the
disk bootable on its own. Copy the cold-boot loader and system file onto the
boot disk with `COPYSYS`, then **cold reboot** for the new footprint to take
effect:

```text
A>COPYSYS
```

On the next boot the banner reports the new BIOS3/BDOS3 addresses and `49K TPA`.

> Note: 64K is not reachable under CP/M 3 — BDOS/BIOS must live somewhere in the
> address space. For a full ~63K TPA use CP/M 2.2 (`cpm63k.dsk`); a banked CP/M 3
> could reach ~55-60K but requires emulator bank-switching support that is not
> implemented here.

### CP/M 2.2 vs CP/M 3 — major differences

CP/M 3 (CP/M Plus, 1982) was Digital Research's last 8-bit CP/M and a
significant step up from CP/M 2.2 (1979):

- **Banked memory.** CP/M 3 can use bank switching to put the OS in a second
  64K bank, leaving almost the entire first bank as TPA. (This emulator runs the
  *non-banked* build, so the practical gain over 2.2 is not realised here.)
- **Bigger, faster disk handling.** A built-in **record buffer cache** and
  deblocking reduce physical disk I/O. The BDOS supports larger files and
  drives, and hashed directory access speeds up file lookups.
- **Date and time stamps.** CP/M 3 directories can record file create/access/
  update time stamps, which CP/M 2.2 has no concept of (it has no clock and no
  file dates at all). Enabling them is a two-step process:
  - `INITDIR d:` reformats the drive's directory to add an extra "stamp" entry
    (an `SFCB`) for every group of three files, reserving the space the date
    fields are stored in. This is a one-time conversion per disk.
  - `SET d:[CREATE=ON,UPDATE=ON,ACCESS=OFF]` then tells CP/M 3 which stamps to
    maintain (create vs. last-update vs. last-access; access and update are
    mutually exclusive).
  - `DATE` sets/reads the system clock that supplies the time written into those
    stamps; without a running clock the stamps are recorded as zero. `DIR` with
    the `[FULL]` option (or `SHOW`) then displays the stored dates.
  Because the Altair BIOS here has no real-time clock, the date fields exist but
  are not advanced — which is what the bundled `DATE` app in `Apps/DATE/`
  works around by reading the emulator's host clock over an I/O port instead.
- **Better console / BDOS.** New BDOS calls for redirected and line-edited
  console input, plus error-handling modes that return errors to the program
  instead of always aborting to the prompt.
- **Resident System Extensions (RSX).** Loadable modules that hook BDOS calls
  (e.g. `GET`/`PUT` redirection), loaded ahead of a program from its `.COM` file.
- **Loadable, relocatable system.** CP/M 3 boots via `CPMLDR` reading a
  `CPM3.SYS` image generated by `GENCPM`, so the memory layout is configurable
  (see above). CP/M 2.2 is a fixed, pre-assembled system image.
- **Richer tools.** Ships utilities like `HELP`, `SET`/`SHOW`, `INITDIR`,
  `GENCPM`, `DATE`, and an improved `PIP` and `SUBMIT`.
- **Automatic startup file (`PROFILE.SUB`).** At cold boot CP/M 3 looks for a
  `PROFILE.SUB` on the system drive (A:) and, if found, runs the CCP commands in
  it automatically — the CP/M 3 equivalent of `AUTOEXEC.BAT`. It is an ordinary
  SUBMIT file (so `SUBMIT.COM` must be present) and is handy for `SETDEF`,
  `SET`, `DATE`, or launching an app on startup. CP/M 2.2 has **no** built-in
  equivalent; an auto-run file requires a patched CCP such as CCP+ or ZCPR, or a
  utility like `AUTORUN` (see below).
- **Compatibility.** CP/M 3 keeps the same FCB/BDOS function numbers for the
  core file calls, so most well-behaved CP/M 2.2 `.COM` programs run unchanged —
  which is why the BDS C toolchain and the apps in `Apps/` work under both.

### Auto-run on CP/M 2.2 (`AUTORUN`)

CP/M 2.2 has no `PROFILE.SUB`, but the bundled `AUTORUN` app (Mike Douglas,
in `Apps/AUTORUN/`) adds an `AUTOEXEC.BAT`-style auto-run command line by
patching the CP/M 2.2 boot tracks directly: it writes the command into the CCP
on track 0 and an enable flag into the BIOS on track 1. You can have the
command fire on **cold boot only, warm boot only, both, or disable** it.

Because it writes to fixed track/sector offsets in the Altair (Burcon) CP/M 2.2
boot tracks, it only works on a matching 2.2 system image (the bundled
`cpm63k.dsk`), and the change persists in the disk image since disks are opened
read/write.

Build and install it from inside CP/M (assemble with `ASM` on B:, then `LOAD`):

```text
B:
FT -G AUTORUN/AUTORUN.SUB
SUBMIT AUTORUN
```

Then run `AUTORUN` and follow the prompts to set the command line and when it
should trigger. The setting takes effect on the next (cold or warm) boot.

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
