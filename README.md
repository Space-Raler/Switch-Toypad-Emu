# Skylanders Portal for Switch (+ Disney Infinity base)

Makes a Nintendo Switch act as a Skylanders **Portal of Power** over USB-C, or (v11) as a
**Disney Infinity base**. Plug the Switch into the machine that runs the game (PC, Wii/Wii U,
PS3, ...) and load figure dumps onto the "portal" / "base" from the Switch screen. Press **L**
(or tap *Switch mode*) to change between the two; the app remembers the last one.

> The Switch is the USB *device* here, so this is for games running on **another**
> machine. It cannot serve Skylanders games running on the same Switch (those need
> the Switch to be the USB *host* for a real portal).

## Status

| Part | State |
|---|---|
| Portal protocol (`portal.*`) | done, 126 host checks |
| USB descriptors + HID control requests (`hid_function.*`) | done, 89 host checks |
| Figure files: scan / load / safe save (`figures.*`) | done, 42 host checks |
| libnx USB device layer (`usb_device.*`) | written; checked against libnx's real headers and a simulated USB host (see below). **Not yet run on a real Switch.** |
| UI (`main.cpp`) | **new: graphical SDL2 UI with touch support.** Syntax-checked against stand-in headers only; not built against real SDL2/libnx, not yet seen on screen. The old text UI is in `extras/main_console.cpp.txt` |
| `Makefile` | standard devkitPro template, **not yet run** (no devkitPro where this was written); v11 adds `source/infinity` (C files) to `SOURCES`/`INCLUDES` |
| v11 | **Disney Infinity mode.** The Switch can come back as an Infinity base (VID 0E6F / PID 0129, 32-byte interrupt IN+OUT) instead of the portal. The protocol is the FlipBase C core (`source/infinity/`, checked there against RPCS3 byte for byte); the USB descriptors and control handling are `infinity_function.*`; `usb_device.cpp` registers either device (switching stops and re-registers usb:ds, so the console sees an unplug and a plug-in). `make test`: 422 checks. **Not run on a real Switch or console**: the Infinity USB path is host-tested and syntax-checked against stand-in libnx/SDL2 headers only |
| v10 | **control status stage.** After a data-in reply (report descriptor) the app now posts an explicit zero-length OUT, and after SET_REPORT a zero-length IN. Found on a Linux host: enumeration worked but `usbhid` failed with `can't add hid device: -110` (the PS5 re-read the report descriptor 3 times and gave up). `ctrl_status=0` in usb.ini restores v9 behaviour. Debug log shows `sent+status` / `status FAILED`. Syntax-checked only; not run on hardware |
| v9 | `max_speed` below 2 is ignored: registering only full-speed descriptors left the high-speed link with no endpoints (all posts failed) |
| v8 | debug page pins the settings actually in effect (yellow line) so a wrong or unread usb.ini is obvious |
| v7 changes | protocol parity with the Flipper build; IN polling at 22 ms; re-plug button; usb:ds fallback profile; more USB diagnostics. `make test`: 275 checks. `usb_device.cpp` was only syntax-checked against a stand-in `switch.h` this round |

## Build

Needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `switch-dev` group, plus SDL2 for the UI:

    sudo dkp-pacman -S switch-sdl2 switch-sdl2_ttf     # (or: pacman -S ... in the devkitPro MSYS2 shell)

    export DEVKITPRO=/opt/devkitpro
    make            # -> skyportal.nro
    make test       # host tests only, needs just g++ (no devkitPro)

Copy `skyportal.nro` to `sd:/switch/` and launch it from the Homebrew Menu.

## Disney Infinity mode

Press **L** (or tap *Switch mode (L)*). The screen says "Switching ...", the Switch drops off USB for
about a second and comes back as an Infinity base. Press **L** again for the Skylanders portal.

1. Put your own Infinity figure dumps (exactly **320 bytes** each, any file name) in
   `sd:/switch/skyportal/infinity/` (created on first launch). The app ships none and cannot
   create figures. Keep backups.
2. Plug the Switch into the console / PC running the game, wait for "Console linked", start the game.
3. Highlight a file and press **A** (or tap it): the app reads the figure number from the dump and
   picks the slot by type: characters go to **Player 1** then **Player 2**, a play set to **Play set**,
   power discs to **P1 disc 1/2**, **P2 disc 1/2**, then **Disc 2/3**. The slot card shows the figure's
   name from the RPCS3 figure list.
4. To choose the slot yourself: press **Y** (or tap a slot), highlight an **empty** slot, press **A**
   (or just tap the empty slot): the file highlighted in the list goes there. A dump the list does not
   know can only be placed this way.
5. **A** / a tap on an occupied slot takes the figure off. **−** clears the base.

The game's progress is written back to the dump file about once a second and on removal / quit,
exactly like the Skylanders mode (first save keeps `<name>.bak`, saves go through `<name>.tmp`).
Both emulated devices keep their figures while you switch, so a figure placed in one mode is still
there when you come back.

The debug page (**X**) shows the base's command counters, whether the game has started its handshake
("Game: connected" in the header means the same), and the last eight commands.

Infinity-specific `usb.ini` keys are listed below. The Skylanders keys do not affect Infinity mode.
`mode.txt` (next to usb.ini) holds the last mode.

*Licensing note:* the Infinity protocol/figure code derives from RPCS3 and is **GPL-2.0-only**
(`LICENSE`); a build that includes it is distributed under the same terms. Disney and Disney Infinity
are trademarks of their owners.

## Use

1. Put figure dumps (exactly **1024 bytes** each, any file name) in
   `sd:/switch/skyportal/figures/`. The app creates the folder on first launch.
2. Launch the app, connect the Switch to the game machine with a USB-C cable
   (use a USB-A to USB-C cable into a console/PC; handheld mode).
3. Pick a file and press **A** to put it on the portal; the game sees it arrive.

Touch: tap a figure to put it on the portal, tap a portal slot to take it off, drag the list to scroll, tap Rescan / Clear portal / Debug.

| Button | Action |
|---|---|
| D-pad | move (left/right jump 5) |
| A | files pane: put figure on portal / slots pane: take it off |
| Y | switch between files and slots panes |
| X | toggle USB debug page |
| R | rescan the figures folder |
| − | clear the portal |
| + | quit |

**Saving.** When the game writes to a figure (levels, gold, upgrades) the change is
saved back to the dump file about once a second and on removal/quit. The first
time a file is saved, the original is kept as `<name>.bak`; saves are written to
`<name>.tmp` first, so an interrupted save does not corrupt the dump. A figure with
unsaved changes that could not be saved is never removed silently.

## If it doesn't work on real hardware

Press **X** (or tap Debug) for the debug page. **A** (or the Re-plug button) detaches and
re-attaches the USB device, so the console sees the portal arrive again without touching the cable.

How to read it (PS5 / PS4 / any console):

- `IN done` / `OUT done` / `portal commands` above 0: the console is talking, the USB layer works.
- `state: Configured` but `IN done: 0` and only a few `SETUP 81 06 v2200 ...` lines (the report
  descriptor): the console enumerated the portal and read its HID descriptor, but the *game* has
  not opened it. Try, in this order: (1) press **A** to re-plug; (2) plug in *before* starting the
  game, wait for "Console linked", then start the game; (3) `strings=0` (no manufacturer/product strings, like the Flipper build).
- The log lines `NNNms state: ...` show how the console walks the port through Reset / Addressed /
  Configured / Suspended. `raw events` counts every usb:ds event, including ones that carried no data.
- If the host hangs right after configuring, or the debug page shows `ack failed`, set
  `kZeroLengthStatusForNoData` in `usb_device.cpp` to `false` and rebuild.

usb.ini keys (all optional; a missing key uses the default shown):

    full_interval=22        IN poll interval, ms at full speed (the Flipper build uses 22)
    high_interval=8         IN poll interval at high/super speed, exponent 1..16 (8 = 16 ms)
    out_full_interval=1     OUT poll interval, full speed
    out_high_interval=4     OUT poll interval, high/super speed (4 = 1 ms)
    out_endpoint=2          OUT endpoint number, 1..15 (real portal: 2)
    max_speed=2             ignored below 2: usb:ds cannot be held at full speed (v9)
    usb11=0                 1 = bcdUSB 1.10 and no BOS, like the real portal
    strings=1               0 = no manufacturer/product strings
    ctrl_status=1           1 = explicit control status stage (v10); 0 = v9 behaviour

Disney Infinity mode (v11; defaults are what the Flipper build uses):

    inf_full_interval=1     poll interval of both endpoints, ms at full speed
    inf_high_interval=4     poll interval of both endpoints at high/super speed (4 = 1 ms)
    inf_out_endpoint=1      OUT endpoint number, 1..15
    inf_usb11=0             1 = bcdUSB 1.10 and no BOS
    inf_strings=0           1 = report manufacturer/product strings (made-up text, the Flipper build has none)

An old usb.ini keeps working, but its `full_interval=4` / `high_interval=4` lines mean the IN
endpoint is polled every 4 ms / 1 ms, not at the 22 ms the Flipper build uses. Delete those lines.

**Which host.** Xbox 360 portals use a different protocol and are not supported.

## What was verified without hardware

- `make test`: 257 checks across the protocol core, USB descriptors (lengths,
  HID report-descriptor parse, endpoint addresses per bus speed), control requests
  and figure-file handling.
- `usb_device.cpp` and `main.cpp` compile against libnx's real headers (`-Wall -Wextra`, clean).
- `usb_device.cpp` was run against a fake `usb:ds` service with a scripted USB host:
  enumeration, report-descriptor fetch, SET_IDLE, SET_REPORT, interrupt IN/OUT
  commands, figure arrival/read/write, cable pull and re-plug. Clean under
  AddressSanitizer, UBSan and ThreadSanitizer. This checks the logic and
  threading, not the real `usb:ds` behaviour, which is what the section above covers.

## Layout

    source/portal.*        protocol: commands A C J L M Q R S V W, 16 slots, status queue
    source/hid_function.*  USB descriptors and HID control requests (no libnx)
    source/figures.*       figure dump files (POSIX, no libnx)
    source/infinity_function.*  Infinity base: USB descriptors and HID control requests (no libnx)
    source/infinity/       Infinity protocol core, figure list and AES/SHA-1 (C, from FlipBase; GPL-2.0-only)
    source/usb_device.*    libnx usb:ds glue and worker thread (either device)
    source/main.cpp        SDL2 graphical UI (extras/main_console.cpp.txt = old text UI, Skylanders only)
    tests/                 host tests (make test)

Protocol behaviour is modeled on RPCS3's emulated portal, extended from 8 to 16 slots, and
checked against the ToyBox Flipper build (works with Trap Team): 'A' always activates, 'R' reports
firmware 0x1B, a leading HID report ID 0 is skipped, and interrupt-OUT packets are treated as
speaker audio (never as commands) while the game has the speaker on ('M').
