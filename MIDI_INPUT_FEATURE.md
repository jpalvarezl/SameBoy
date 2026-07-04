# MIDI Input Accessory — Design & Learning Notes

> Goal: Add a **MIDI Input** serial accessory to SameBoy (like *Game Boy Printer* or
> *Workboy*) so that a host MIDI source can be sent into the emulator, and ROMs like
> **mGB** and **LSDJ** consume it over the emulated Game Boy Link Cable — i.e. a built‑in
> software **Arduinoboy**.
>
> This document is a self‑study guide. It explains *why* the feature is shaped the way it
> is, points at the exact code you'll touch, and links to primary references so you can
> implement it yourself as a learning exercise.

---

## 1. The problem, in one paragraph

mGB and LSDJ do **not** speak MIDI. They speak the Game Boy Link Cable serial protocol.
On real hardware an **Arduinoboy** sits between a MIDI cable and the Game Boy Link port,
translating MIDI into the byte stream those ROMs expect (and vice‑versa). In SameBoy today
the only things you can attach to the serial port are the **Printer**, the **Workboy**, or
**another running emulator instance** (the two‑window Link Cable). There is no virtual
device to feed data in, which is exactly why "connect the cable" feels like a dead end when
you only have one window open. The feature adds a virtual Arduinoboy‑like device plus a MIDI
source picker.

---

## 1.5 Building & running SameBoy on macOS (do this first)

You need **full Xcode** installed (not just Command Line Tools) because the Cocoa app
compiles `.xib` UI files with `ibtool`. You also need `rgbds` (boot-ROM assembler),
installable with `brew install rgbds`.

Build + launch in one line (works even if `xcode-select` points at Command Line Tools —
no `sudo` needed):

```bash
cd ~/code/forks/SameBoy
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer make -j$(sysctl -n hw.ncpu) && open build/bin/SameBoy.app
```

- First build ~2 min (also assembles the Game Boy boot ROMs). Incremental rebuilds after a
  code change take **seconds** — `make` only recompiles what changed.
- Clean rebuild: `make clean`.
- The app bundle lands at `build/bin/SameBoy.app`.

**Toolchain note (Xcode 26 / clang 21):** a very new compiler promotes a harmless warning in
the bundled HexFiend library to an error (because SameBoy builds with `-Werror`). This branch
adds `-Wno-implicit-const-int-float-conversion` to the Makefile `WARNINGS` to work around it.
On older compilers the flag is ignored, so it's safe.

---

## 2. Background you need before touching code

### 2.1 Game Boy Link Cable / serial hardware

The Game Boy serial port is an 8‑bit shift register clocked one bit at a time. Two registers
matter:

- **`SB` (0xFF01)** — Serial transfer data. The byte being shifted out/in, MSB first.
- **`SC` (0xFF02)** — Serial control:
  - bit 7 = transfer start / in‑progress
  - bit 1 = clock speed (CGB fast mode)
  - bit 0 = **clock source**: `1` = internal clock (this Game Boy is the **master**),
    `0` = external clock (this Game Boy is a **slave**, waiting to be clocked by the other side)

Whoever owns the clock decides timing. When a full 8 bits have shifted, a serial interrupt
(`IF` bit 3) fires. Read the Pan Docs "Serial Data Transfer" section (link in §7) until this
is second nature — the whole feature hinges on **who provides the clock**.

**Key consequence for us:** mGB is a *slave* (external clock). The Arduinoboy is the *master*
that generates clock edges and shifts bytes in. So our virtual device must **generate the
clock**, not just respond to it.

### 2.2 mGB (MIDI → Game Boy synth)

mGB turns the Game Boy into a 4/5‑voice MIDI sound module. The Arduinoboy in "mGB mode"
essentially **forwards raw MIDI bytes** to the Game Boy; mGB parses standard MIDI messages:

- Note On / Note Off on MIDI channels 1–5 → Pulse 1, Pulse 2, Wave, Noise, and a poly mode.
- Control Change (CC) → synth parameters (duty, envelope, sweep, etc.).
- Pitch Bend, Program Change.

Because it's *raw MIDI passthrough*, **mGB is the easiest and highest‑value first target**.

### 2.3 LSDJ (Little Sound Dj)

LSDJ uses several sync/keyboard modes chosen on its tempo/project screens, and the Arduinoboy
*translates* rather than passing raw MIDI:

- **LSDJ (slave) sync** — Arduinoboy clocks LSDJ; sends transport/tempo bytes. GB is slave.
- **LSDJ (master) sync** — LSDJ is the master and emits a byte per row/step; Arduinoboy reads
  it and produces MIDI clock/notes. GB is **master** here.
- **Keyboard mode** — MIDI notes drive LSDJ instruments; Arduinoboy translates note data.

So LSDJ needs both directions and per‑mode translation → treat it as **phase 2**, after mGB.

### 2.4 Direction-of-clocking cheat sheet

| Target | Data direction | Clock master | SameBoy mechanism |
|---|---|---|---|
| mGB | MIDI → GB | **our device** | generate clock; push bits with `GB_serial_set_data_bit` |
| LSDJ keyboard / slave sync | MIDI → GB | **our device** | same, but translate MIDI → LSDJ bytes |
| LSDJ master sync / note‑out | GB → MIDI | **the Game Boy** | capture bytes via `serial_transfer_bit_start_callback` → emit MIDI |

---

## 3. How SameBoy's serial layer is built (read these files)

All paths are relative to the repo root. Line numbers are approximate — grep for the symbol.

### 3.1 The two callbacks (used when the *Game Boy* is the clock master)

```
Core/gb.h        : GB_serial_transfer_bit_start_callback_t / _bit_end_callback_t typedefs
Core/gb.c  ~1332 : GB_set_serial_transfer_bit_start_callback()
Core/gb.c  ~1340 : GB_set_serial_transfer_bit_end_callback()
```

- `..._bit_start(gb, bit_to_send)` — the Game Boy is about to shift a bit **out**.
- `..._bit_end(gb) -> bit` — returns the bit the Game Boy shifts **in**.

These fire from `GB_serial_master_edge()` **only** when `(SC & 0x81) == 0x81`
(internal clock / master). The Printer and Workboy are implemented entirely with these two
callbacks because in those scenarios the Game Boy always owns the clock.

```
Core/printer.c   : reference peripheral (medium complexity)
Core/workboy.c   : reference peripheral (simplest full example — read this first)
```

### 3.2 The external‑master path (used when the *Game Boy* is a slave)

```
Core/gb.c  ~1348 : GB_serial_get_data_bit(gb)        // read the bit GB is presenting
Core/gb.c  ~1363 : GB_serial_set_data_bit(gb, bit)   // shift a bit INTO the GB, advance its count
```

This is how an *external* clock master feeds a slave Game Boy. The two‑window Link Cable uses
exactly this: the master window's bit‑end handler calls `GB_serial_set_data_bit` on the slave
window (`Cocoa/Document.m`, `_linkCableBitEnd` ~2660). **mGB needs this path**, because mGB
is the slave.

### 3.3 The periodic serial "heartbeat"

```
Core/timing.c ~184 : GB_serial_master_edge(gb)
Core/timing.c ~242 : called from GB_set_internal_div_counter when (triggers & serial_mask)
Core/memory.c ~1745: serial_mask = cgb_mode && fast ? 4 : 0x80  (set on SC writes)
```

Important insight: `GB_serial_master_edge()` is invoked at the serial **base rate** every time
the relevant `div` bit toggles — **regardless of whether the Game Boy is master or slave**.
Inside, it only does shift work when the GB is the master. That makes it a convenient,
correctly‑paced hook where our device can, when the GB is an **armed external‑clock slave**
(`SC & 0x80` set, bit 0 clear), clock queued MIDI bytes in via `GB_serial_set_data_bit`.
This is the piece the plain callback model does *not* cover.

### 3.4 Accessory registration & lifecycle

```
Core/gb.h  ~331  : enum GB_accessory_t { GB_ACCESSORY_NONE, _PRINTER, _WORKBOY }
Core/gb.h  ~645  : GB_SECTION(accessory, ...) union — per‑accessory state lives here
Core/gb.c  ~1386 : GB_disconnect_serial() — clears callbacks + memsets the accessory section
Core/gb.c  ~1400 : GB_get_built_in_accessory()
```

A built‑in accessory: (1) sets `gb->accessory = GB_ACCESSORY_*`, (2) registers callbacks,
(3) keeps its state inside the `accessory` `GB_SECTION` union so save‑states and
`GB_disconnect_serial()` handle it automatically.

### 3.5 Build system

`Makefile` builds the core from `$(shell ls Core/*.c)` (see `CORE_SOURCES`, ~line 431), so a
new `Core/midi.c` is picked up **automatically** — no Makefile edit needed for the core.

### 3.6 macOS frontend wiring (the "Connect" menu)

```
Cocoa/MainMenu.xib      : "Connect" submenu — None / Link Cable / Printer / Workboy items
Cocoa/Document.m ~2519  : disconnectAllAccessories:  (None)
Cocoa/Document.m ~2527  : connectPrinter:
Cocoa/Document.m ~2535  : connectWorkboy:
Cocoa/Document.m ~1449  : validateUserInterfaceItem: — sets the menu checkmark/state
Cocoa/GBApp.m    ~321   : dynamically builds the Link‑Cable partner submenu (pattern to copy
                          for a "MIDI source" submenu)
```

---

## 4. CoreMIDI (the macOS input side)

On macOS you receive MIDI with **CoreMIDI** (`<CoreMIDI/CoreMIDI.h>`, link with the
`CoreMIDI` framework):

1. `MIDIClientCreate` → a client.
2. `MIDIInputPortCreate` (or the newer block‑based `MIDIInputPortCreateWithProtocol`) → a port.
3. Enumerate sources with `MIDIGetNumberOfSources` / `MIDIGetSource` and read each source's
   display name (`kMIDIPropertyDisplayName`) to populate the submenu.
4. `MIDIPortConnectSource` on the chosen source.
5. In the read callback, walk the `MIDIPacketList` and hand each raw MIDI byte to the core
   (e.g. `GB_midi_input_byte(gb, byte)`).

⚠️ The CoreMIDI read callback runs on a **high‑priority MIDI thread**, not the emulator
thread. Push bytes through a lock‑free/atomic ring buffer (or SameBoy's existing
`performAtomicBlock:` pattern) rather than touching core state directly.

---

## 5. Suggested implementation plan (incremental)

### Phase 0 — Scaffolding
- [ ] `Core/midi.h` / `Core/midi.c`: define `GB_midi_t` state struct + a MIDI‑byte ring buffer.
- [ ] Add `GB_ACCESSORY_MIDI` to the `GB_accessory_t` enum (`Core/gb.h`).
- [ ] Add a `GB_midi_t midi;` member to the `accessory` `GB_SECTION` union (`Core/gb.h`).
- [ ] `GB_connect_midi(gb, ...)` — set accessory, register callbacks, zero state.
- [ ] `GB_midi_input_byte(gb, uint8_t)` — enqueue a raw MIDI byte from the frontend.

### Phase 1 — mGB (MIDI → GB, raw passthrough) — *the milestone*
- [ ] Hook `GB_serial_master_edge()` (or a small helper it calls) so that when the GB is an
      armed external‑clock slave and the queue is non‑empty, we clock one byte in bit‑by‑bit
      via `GB_serial_set_data_bit`, pacing it so mGB's ISR can re‑arm `SC` between bytes.
- [ ] Test with the mGB ROM: play notes from a DAW / MIDI keyboard, hear sound.

### Phase 2 — macOS UI + CoreMIDI
- [ ] Add a "MIDI Input" item to the Connect submenu in `MainMenu.xib` with a dynamic
      source submenu (copy the `GBApp.m` Link‑Cable partner pattern).
- [ ] `connectMIDI:` in `Document.m` → `GB_disconnect_serial` + `GB_connect_midi`; add a
      `validateUserInterfaceItem:` branch for the checkmark.
- [ ] Create the `MIDIClientRef` / input port; read callback → `GB_midi_input_byte`.
- [ ] Persist the chosen source name in `NSUserDefaults`.

### Phase 3 — LSDJ (later)
- [ ] Add sync‑mode handling (LSDJ slave/master sync, keyboard) with MIDI↔byte translation.
- [ ] GB → MIDI OUT direction via `serial_transfer_bit_start_callback` (for LSDJ master sync).

### Phase 4 — Other frontends (optional)
- [ ] SDL frontend MIDI input (RtMidi / platform APIs) mirroring the Cocoa accessory.

---

## 6. Testing tips

- **mGB first** — it's raw passthrough, so a wiring bug is obvious (silence vs. notes).
- Use a virtual MIDI source (macOS **IAC Driver** in *Audio MIDI Setup*) so you can drive it
  from any DAW without hardware.
- A MIDI monitor app confirms your source actually emits bytes before you blame the core.
- Watch the serial interrupt: if mGB never re‑arms `SC`, you're clocking bytes too fast —
  add spacing between bytes (wait for `SC & 0x80` to be re‑set by the ROM).
- Study `Core/workboy.c` end‑to‑end first; it's the smallest complete serial device.

---

## 7. References

### Game Boy hardware / serial
- Pan Docs — Serial Data Transfer (SB/SC, clocking, interrupt): https://gbdev.io/pandocs/Serial_Data_Transfer_(Link_Cable).html
- Pan Docs — full index: https://gbdev.io/pandocs/
- gbdev community & resources: https://gbdev.io/

### Arduinoboy / mGB / LSDJ (the protocols we're emulating)
- Arduinoboy (Trash80) — the reference hardware & source: https://github.com/trash80/Arduinoboy
- mGB (Trash80) — the MIDI synth ROM: https://github.com/trash80/mGB
- LSDJ official site & docs: https://www.littlesounddj.com/lsd/index.php
- LSDJ manual (sync/keyboard/MIDI modes): https://www.littlesounddj.com/lsd/latest/documentation/
- Arduinoboy wiki / mode explanations: https://github.com/trash80/Arduinoboy/wiki

### CoreMIDI (macOS input)
- Apple — Core MIDI framework: https://developer.apple.com/documentation/coremidi
- Apple — MIDIInputPortCreateWithProtocol: https://developer.apple.com/documentation/coremidi/2876500-midiinputportcreatewithprotocol
- MIDI 1.0 message reference (status bytes, running status): https://midi.org/summary-of-midi-1-0-messages

### SameBoy internals to read
- `Core/workboy.c` — simplest complete serial peripheral (read this first).
- `Core/printer.c` — a more involved peripheral with a command state machine.
- `Core/timing.c` — `GB_serial_master_edge()` and the serial heartbeat.
- `Core/gb.c` — `GB_serial_get_data_bit` / `GB_serial_set_data_bit` / `GB_disconnect_serial`.
- `Cocoa/Document.m` — `_linkCableBitEnd` and the `connect*` accessory actions.
- `Cocoa/GBApp.m` — dynamic "Connect" submenu construction (source‑picker pattern).
- `CONTRIBUTING.md` — coding style / PR expectations before you upstream anything.

---

## 8. Notes on contributing upstream

- Work on this fork (`origin = github.com/jpalvarezl/SameBoy`), on the `feature/midi-input`
  branch, then open a PR against `LIJI32/SameBoy` if you want it upstream.
- Read `CONTRIBUTING.md` and match SameBoy's C style (it's specific about braces/naming).
- Upstream will likely want the **core device to be frontend‑agnostic** (no CoreMIDI in
  `Core/`) — keep all CoreMIDI code in `Cocoa/`, and feed the core only raw MIDI bytes via a
  small public API. That separation also makes SDL/other frontends easy later.
```
