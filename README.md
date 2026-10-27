# SkyPortal Switch — Skylanders, Disney Infinity, and LEGO Dimensions

Makes a Nintendo Switch act as a Skylanders **Portal of Power**, a **Disney Infinity base**, or a **LEGO Dimensions ToyPad** over USB. Connect the Switch to the console/PC running the game and choose a mode from the UI. Press **L** (or tap *Switch mode*) to cycle through all three modes; the app remembers the last one.

> The Switch is the USB *device* here, so this is for games running on **another**
> machine. It cannot serve Skylanders games running on the same Switch (those need
> the Switch to be the USB *host* for a real portal).

## Status

| Part | State |
|---|---|
| Portal protocol (`portal.*`) | done, 126 host checks |
| USB descriptors + HID control requests (`hid_function.*`) | done, 89 host checks |
| Figure files: scan / load / safe save (`figures.*`) | done, 42 host checks |
| libnx USB device layer (`usb_device.*`) | written; checked against libnx's real headers and a simulated USB host (see below). **has ran on a real Switch.** |
| UI (`main.cpp`) | **new: graphical SDL2 UI with touch support.** Syntax-checked against stand-in headers only; not built against real SDL2/libnx, not yet seen on screen. The old text UI is in `extras/main_console.cpp.txt` |
| `Makefile` | standard devkitPro template; the updated source in this package builds `skyportal.nro` with devkitPro, but hardware behavior still needs testing |
| v17 | **LEGO "Game: connected" + automatic re-plug.** The LEGO pill was hard-wired to "waiting"; it now turns green once the game has sent its wake command (and back to waiting after a bus reset). New `auto_replug=1` / `auto_replug_after=8` in usb.ini (all modes): while the console is linked but the game has not started using the device, or enumeration is stuck for 6 s, the app detaches and re-attaches by itself (every `auto_replug_after` seconds for 3 tries, then every 20 s; it stops when the game talks to us and starts over after a real unplug). A missing `lego_strings` now defaults to 1 as documented (an old usb.ini has no lego_ lines). Cancelled transfers when the console drops the connection are no longer counted as errors. LEGO test updated for the v16 wake reply. Host tests pass; **not run on hardware, and the .nro is NOT rebuilt in this package** |
| v15 | **Portal UI and library tools.** LEGO ToyPad visual layout and quick-swap, persistent favorites, recursive game-folder scanning, and game filters for Skylanders / Disney Infinity. The updated source builds here; hardware behavior still needs testing |
| v13 | **LEGO Dimensions mode.** Adds the `0E6F:0241` ToyPad identity, seven positions, wake/read/write/model/seed/challenge commands, synthetic test profiles, and persistence of profile writes. Host-tested only; not yet verified on Switch/PS5 hardware |
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

Press **L** (or tap *Switch mode (L)*) to cycle from Skylanders to Disney Infinity, then LEGO Dimensions, then back to Skylanders. Switching drops USB briefly so the host sees the selected device re-enumerate.

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

LEGO Dimensions mode (v13; defaults based on the public Flipper LDToyPad emulator):

    lego_full_interval=1    poll interval of both endpoints, ms at full speed
    lego_high_interval=4    poll interval of both endpoints at high/super speed
    lego_out_endpoint=1     OUT endpoint number, 1..15
    lego_usb11=0            1 = bcdUSB 1.10 and no BOS
    lego_strings=1          1 = include ToyPad manufacturer/product strings

An old usb.ini keeps working, but its `full_interval=4` / `high_interval=4` lines mean the IN
endpoint is polled every 4 ms / 1 ms, not at the 22 ms the Flipper build uses. Delete those lines.

**Which host.** Xbox 360 portals use a different protocol and are not supported.

## What was verified without hardware

- The Switch `.nro` builds with devkitPro/libnx. Host-side protocol tests are included in the source tree; they were not run for this startup-handshake fix. A successful build does not verify USB enumeration or in-game behavior.

## Layout

    source/portal.*        protocol: commands A C J L M Q R S V W, 16 slots, status queue
    source/hid_function.*  USB descriptors and HID control requests (no libnx)
    source/figures.*       figure dump files (POSIX, no libnx)
    source/infinity_function.*  Infinity base: USB descriptors and HID control requests (no libnx)
    source/infinity/       Infinity protocol core, figure list and AES/SHA-1 (C, from FlipBase; GPL-2.0-only)
    source/lego_dimensions_function.*  LEGO ToyPad USB descriptors, frame commands, tag memory, TEA challenge
    source/lego_dimensions_protocol.* transport-independent 32-byte ToyPad frames
    source/usb_device.*    libnx usb:ds glue and worker thread (three modes)
    source/main.cpp        SDL2 graphical UI (extras/main_console.cpp.txt = old text UI, Skylanders only)
    tests/                 host tests (make test)

Protocol behaviour is modeled on RPCS3's emulated portal, extended from 8 to 16 slots, and
checked against the ToyBox Flipper build (works with Trap Team): 'A' always activates, 'R' reports
firmware 0x1B, a leading HID report ID 0 is skipped, and interrupt-OUT packets are treated as
speaker audio (never as commands) while the game has the speaker on ('M').


## Portal UI, quick-swap, favorites, and game filters (v15)

- LEGO Dimensions uses a ToyPad illustration with seven circular pads rather than a generic slot grid. Tap a filled pad to select its token, then tap an empty pad to move it to that position; tap the selected pad again to remove it. The move operation preserves the active in-memory tag profile and emits removal/placement events.
- The figure browser has a persistent favorite toggle. Favorites are stored in `sdmc:/switch/skyportal/favorites.txt` as full paths, so they work for Skylanders, Disney Infinity, and LEGO profiles. If a library is moved to a different folder, favorites may need to be re-marked.
- Skylanders and Disney Infinity include a game filter cycling through known game labels plus Favorites. The scanner now searches subfolders recursively, so libraries organized into per-game folders are discoverable. For game filtering to work reliably, put dumps into folders named for the game or include a recognizable game name in the file name.
- The filter is based on folder/file names, not an analysis of the dump contents. Unknown or generically named files may appear under All games but not under a specific game filter.

## LEGO Dimensions protocol references

Protocol behaviour was independently implemented from public documentation and the public Flipper LDToyPad emulator source: https://github.com/SegerEnd/Flipper-Zero-LD-Toypad-Emulator, https://github.com/ev3dev/ev3dev.github.io/blob/master/docs/tutorials/using-lego-dimensions-toy-pad.md, and https://github.com/Ellerbach/LegoDimensions. The Flipper repository has a character/vehicle catalog and emulator source, but no raw dump folder; the app generates synthetic test profiles instead of pretending those are physical tag dumps.

## LEGO Dimensions mode (PS5 / PS4 target)

LEGO Dimensions mode is selectable from the main UI with **L** or **Switch mode**. The Switch app presents a USB HID ToyPad identity (`0E6F:0241`) and supports seven visual positions mapped onto the three logical ToyPad pads. The USB worker handles the same 32-byte frame structure used by the public Flipper LDToyPad emulator, including wake, tag-page read/write, model, seed/challenge, and LED/status command families. Vehicle page writes are retained in the active profile and flushed back to the SD card file.

### Test profile files

The public [Flipper-Zero-LD-Toypad-Emulator](https://github.com/SegerEnd/Flipper-Zero-LD-Toypad-Emulator) repository contains source code and a character/vehicle ID catalog; it does **not** publish a directory of raw character/vehicle NFC dump files. To make first-run testing possible without distributing third-party tag dumps, this app creates a small synthetic profile set the first time LEGO mode is selected, when no valid profiles exist. These profiles are protocol test fixtures generated from public IDs, not verified byte-for-byte dumps of physical tags.

Profile directory: `sdmc:/switch/skyportal/lego_uids/`

Each `.ldtag` file is 192 bytes:

- bytes `0..6`: seven-byte UID
- byte `7`: `0` for character, `1` for vehicle
- bytes `8..11`: figure/vehicle ID, little-endian
- bytes `12..191`: 180-byte tag memory (45 four-byte pages)

The built-in fixtures include Batman, Gandalf, Wyldstyle, Emmet, Harry Potter, Sonic, Chase McCain, and several vehicles. You can add profiles in the same format. Keep backups of edited files; in-game tag writes update the loaded profile and the app's normal save routine creates a `.bak` before overwriting an existing profile.

The ToyPad initialization frame (`55 0F B0 ...`) is a host-to-pad OUT packet. On USB reset, the emulator waits for that packet before sending tag events on IN. This avoids sending the host's initialization frame in the wrong direction or before the game is ready.

### Known testing boundary

Host-side tests exercise HID descriptors, frame validation, encrypted model requests, tag-memory reads/writes, tag placement/removal, and existing Skylanders/Infinity cores. The updated source builds a Switch `.nro`, but this package has not been tested on hardware. A successful build is not proof that the PS5 accepts the USB device or that every game flow works. The next verification must be done with the compiled `.nro` on the Switch and the PS5 running the PS4 version of LEGO Dimensions.

## If the app crashes at launch: startup.log

Each launch writes `sdmc:/switch/skyportal/startup.log`, one line per startup step (SDL init, window, renderer, fonts, USB
registration, worker thread), flushed to the SD card immediately. It also records how the app was started (applet type) and how
much memory the process has. After a crash, **the last line is the step that was running**. Send that file, plus the Atmosphere
crash report if one was written (`/atmosphere/crash_reports/*.log`, and the error code shown on screen, e.g. 2162-xxxx).
The first frame is drawn before USB is started, so a USB-service problem no longer hides the UI.
