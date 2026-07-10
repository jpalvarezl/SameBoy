# MIDI Input Accessory — Design & Learning Notes

> Current goal: add a contained **MIDI Input accessory for mGB**. A host MIDI source sends
> raw MIDI bytes through the emulated Game Boy Link port, matching the observable behavior
> of an Arduinoboy-style adapter in mGB mode.
>
> A broader, explicitly modeful **Arduinoboy-compatible accessory** is a plausible follow-up.
> Standard LSDJ MIDI clock synchronization has already been prototyped successfully, but its
> inclusion is pending a scope discussion with SameBoy's maintainer; it is not silently part
> of the first PR.
>
> This document is a self-study guide and a record of those scope decisions. It explains
> *why* the feature is shaped the way it is, points at the exact code involved, and links to
> primary references.

---

## 1. The problem, in one paragraph

mGB parses standard MIDI messages, but it receives their bytes through the Game Boy Link
port rather than a native MIDI connector. On real hardware an **Arduinoboy** in mGB mode (or
a compatible USB-MIDI adapter) provides the electrical clock and serializes those raw bytes.
SameBoy can currently attach a **Printer**, a **Workboy**, or **another emulator instance**,
but its virtual Link Cable cannot connect Ableton or a MIDI controller to mGB. The contained
feature fills exactly that gap: a frontend MIDI source picker plus a frontend-neutral core
API that clocks raw bytes into an armed, external-clock Game Boy serial transfer.

SameBoy's existing two-window Link Cable already handles native ROM-to-ROM protocols such as
LSDJ-to-LSDJ synchronization; both LSDJ instances use `SYNC: LSDJ`, and one becomes `LEAD`.
MIDI translation is only needed when crossing between the Link protocol and an external DAW
or MIDI device.

## 1.1 Current scope decision and upstream proposal

The implementation has established two different, valid scopes:

1. **Contained first contribution — mGB MIDI input.** Raw MIDI bytes are queued in the core
   and clocked into mGB; Cocoa supplies CoreMIDI endpoint selection. This is complete and
   verified with Ableton through an IAC bus.
2. **Possible follow-up — an Arduinoboy-compatible peripheral.** A real Arduinoboy is
   modeful: mGB is raw byte transport, while standard LSDJ slave/master sync translates
   between MIDI real-time messages and Link clock behavior. The output prototype correctly
   converted LSDJ `SYNC: LSDJ` into MIDI Start/Clock/Stop and locked Ableton to LSDJ's tempo.

Do not conflate the second scope with musical MIDI output from ordinary LSDJ tracks. Standard
LSDJ 9.4.2 has `SYNC: MIDI` as a clock-follower mode and does **not** expose normal song notes
as MIDI Note On/Off. Historical Arduinoboy MIDIOUT support required a special LSDJ ROM.

The next action is to ask SameBoy's maintainer which model belongs upstream before doing more
protocol work. Suggested issue draft:

> **Title: Proposal: MIDI input accessory for mGB, with possible Arduinoboy-compatible modes**
>
> I have a working prototype of a frontend-neutral MIDI serial accessory plus a Cocoa
> CoreMIDI source picker. It clocks raw host MIDI bytes into an external-clock Game Boy
> serial transfer, matching an Arduinoboy-compatible adapter in mGB mode. I have verified
> Ableton → IAC Driver → SameBoy → mGB live, and added focused C tests around the queue and
> serial transfer behavior.
>
> Before preparing a PR, I would like guidance on the preferred scope and naming:
>
> 1. Would a contained **mGB MIDI-input accessory** be useful as its own contribution?
> 2. Should the core model remain a generic raw MIDI byte pipe, or should it be named and
>    designed as a modeful **Arduinoboy-compatible accessory**?
> 3. If the latter is desirable, should standard LSDJ MIDI clock follower/leader modes be
>    separate follow-up changes?
>
> The proposed first change keeps CoreMIDI entirely in the Cocoa frontend. The core only
> owns a bounded byte queue and Game Boy serial clocking, so other frontends could provide
> MIDI later. It does not inspect ROM state or translate ordinary LSDJ notes.
>
> I also prototyped LSDJ leader → MIDI Clock/Start/Stop and confirmed that Ableton follows
> its tempo, but I am deliberately holding that work back until the accessory boundary is
> agreed.

### Research boundary: other music ROMs

Research did not reveal a universal "Game Boy MIDI" wire protocol:

- **mGB** is the clear raw-byte case and the direct target for this contribution.
- **LSDJ** uses native Link synchronization between two Game Boys; an Arduinoboy translates
  that behavior only when connecting to MIDI equipment. Standard LSDJ does not send track
  notes as MIDI.
- **FMS** documents receive-only Arduinoboy/Pocket MIDI synchronization, but it is GBA
  software and cannot run in SameBoy.
- **nanoloop** behavior varies by product generation. Its official USB adapter has distinct
  MIDI (mGB) and SYNC modes; current cartridges may use cartridge-integrated sync jacks, and
  nanoloop-specific Link sync uses line pulse patterns rather than a universal MIDI byte
  stream.

This supports keeping the first PR mGB-specific while discussing a larger, explicitly
modeful adapter separately.

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

Standard LSDJ 9.4.2 has two relevant but distinct synchronization paths:

- **`SYNC: LSDJ`** is its native Game Boy-to-Game Boy protocol. With SameBoy's existing
  virtual Link Cable, both instances use this mode; the one started first becomes `LEAD`.
- **`SYNC: MIDI`** makes LSDJ a follower. A physical adapter consumes MIDI Start/Clock/Stop
  and converts those messages into Link clock behavior; it does not merely pass every MIDI
  byte through as mGB mode does.
- When LSDJ is the `SYNC: LSDJ` leader, an Arduinoboy Master Sync adapter can observe its Link
  cadence and generate MIDI Start/Clock/Stop. This hardware-faithful direction was
  prototyped successfully, but remains outside the first mGB-input scope pending maintainer
  discussion.

Standard LSDJ does **not** output the musical notes from its four tracks as ordinary MIDI.
The Arduinoboy firmware can emit one startup row marker, but that is not song-note playback.
Historical MIDIOUT/MIDIMAP behavior depended on special LSDJ ROMs and is not a target here.

### 2.4 Direction-of-clocking cheat sheet

| Adapter mode | Data/meaning | Clock master | SameBoy mechanism/status |
|---|---|---|---|
| mGB | raw MIDI bytes → GB | **adapter** | current target: `GB_serial_set_data_bit`, MSB-first |
| LSDJ MIDI follower | MIDI real-time → Link clock behavior | **adapter** | possible future Arduinoboy mode; not raw passthrough |
| LSDJ Link leader → MIDI | Link cadence → MIDI Start/Clock/Stop | **Game Boy** | successful prototype; pending scope decision |
| LSDJ ↔ LSDJ | native Link protocol, no MIDI | whichever instance is `LEAD` | already handled by SameBoy's virtual Link Cable |

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

### Phase 0 — Scaffolding — complete
- [x] `Core/midi.h` / `Core/midi.c`: `GB_midi_t` state and bounded MIDI-byte ring buffer.
- [x] `GB_ACCESSORY_MIDI` and accessory-section state.
- [x] `GB_connect_midi` and `GB_midi_input_byte` frontend-neutral API.

### Phase 1 — mGB (MIDI → GB, raw passthrough) — complete
- [x] When the GB is an armed external-clock slave and the queue is non-empty, clock bytes
      in MSB-first through `GB_serial_set_data_bit`.
- [x] Verify live with Ableton / IAC Driver → mGB.
- [x] Add a focused C harness covering queue behavior and serial pumping. This harness is a
      development stopgap rather than a new project-wide test framework.

### Phase 2 — macOS UI + CoreMIDI — complete
- [x] Dynamic MIDI input-source submenu.
- [x] CoreMIDI client/input port and read callback → `GB_midi_input_byte`.
- [x] Source selection persistence and teardown.

Commit `39ae967` is the input-only integration boundary.

### Phase 3 — output experiment — proven, but scope pending
- [x] Capture Game Boy-master serial bytes through a registered output callback.
- [x] Prototype standard LSDJ leader → MIDI Start/Clock/Stop.
- [x] Reconstruct CoreMIDI timestamps from emulated time; Ableton follows LSDJ tempo.
- [ ] Do **not** prepare this as part of the mGB PR until the maintainer responds to §1.1.

Commits `4d64eba` and `61bdef9`, plus the current uncommitted Cocoa output changes, belong to
this exploratory phase. Musical note extraction and historical special-ROM modes are not
planned.

### Phase 4 — Other frontends (optional)
- [ ] SDL frontend MIDI input (RtMidi / platform APIs) mirroring the Cocoa accessory.

### Phase 5 — Automated tests (match the project's conventions)
> ⚠️ Reality check: SameBoy has **no C unit‑test framework** and **no tests at all** for the
> existing serial peripherals (Printer, Workboy). Its testing is entirely **ROM‑based image
> hashing** (`Tester/main.c` → `sameboy_tester`, pinned SHA‑1s in
> `.github/actions/sanity_tests.sh`). `CONTRIBUTING.md` forbids new languages, so any test
> must stay in C11 and fit that model. So there isn't a serial‑device test to "copy" — we're
> setting the precedent. Keep it deterministic and minimal.
- [ ] Decide the deterministic signal to assert on: e.g. feed a fixed MIDI sequence to mGB
      and capture the resulting framebuffer (a visualiser) or an APU/register snapshot.
- [ ] The current `sameboy_tester` can't inject serial/MIDI input. Add a minimal hook (e.g. a
      `--midi <file-of-bytes>` option, or a small dedicated `GB_INTERNAL` harness like
      `Tester/main.c`) that calls `GB_midi_input_byte` on a schedule while the ROM runs.
- [ ] Produce a reference output, pin its SHA‑1, and wire it into `sanity_tests.sh` exactly
      like the acid2 / sound ROMs (`--length N test.gb`, then hash‑compare the `.bmp`).
- [ ] Keep the test ROM small and checked in (or document how to regenerate it); reference
      ROMs live under `.github/actions/`.
- [ ] Bonus: a pure‑core C test that connects the MIDI device, pushes known bytes, single‑
      steps the serial clock, and asserts the bytes reach `SB` — no ROM needed, closest thing
      to a "unit test" the codebase allows.

---

## 6. Testing

### How SameBoy tests things (so our tests fit in)
- `Tester/main.c` builds **`sameboy_tester`** (`make tester`), a headless harness that runs a
  `.gb`/`.gbc` **test ROM** for N frames and dumps the final framebuffer to a BMP/TGA.
- `.github/actions/sanity_tests.sh` runs known test ROMs (acid2, sound, oam_bug) through it
  and compares the **SHA‑1 of the output image** against pinned hashes. That *is* the CI test
  suite (`.github/workflows/sanity.yml`).
- There is **no unit‑test framework** and **no serial‑device tests** to copy. `CONTRIBUTING.md`
  bans new languages, so tests stay C11 + ROM‑hash. See Phase 5 for how we add MIDI tests
  within these constraints.

### Manual testing tips
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

### Arduinoboy / mGB / synchronization research
- Arduinoboy (Trash80) — behavioral reference; its source headers specify GPLv2, so do not
  copy firmware implementation into Expat-licensed SameBoy without resolving licensing:
  https://github.com/trash80/Arduinoboy
- mGB (Trash80) — the raw-MIDI synth ROM and current target: https://github.com/trash80/mGB
- LSDJ official site and manuals: https://www.littlesounddj.com/lsd/index.php
- Arduinoboy mode explanations: https://github.com/trash80/Arduinoboy/wiki
- FMS external-sync guide (GBA; research only): https://lo-bit.club/fms/guide
- nanoloop USB-MIDI adapter (separate MIDI and SYNC modes):
  https://www.nanoloop.com/midi/index.html
- nanoloop product/sync documentation: https://www.nanoloop.com/sync/index.html

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

- First open the proposal issue in §1.1 and let the maintainer clarify scope/naming before
  preparing a PR.
- Work on this fork (`origin = github.com/jpalvarezl/SameBoy`) on `feature/midi-input`.
- `39ae967` is the input-only boundary. Preserve the later output experiment separately
  rather than allowing sunk-cost pressure to determine the first PR.
- Read `CONTRIBUTING.md` and match SameBoy's C style (it's specific about braces/naming).
- Keep the core device frontend-agnostic: no CoreMIDI in `Core/`. For the contained mGB
  feature, Cocoa feeds only raw MIDI bytes through a small public API.
- If upstream prefers a true Arduinoboy-compatible peripheral, agree on its mode API and on
  whether protocol state belongs in the portable core before extending the implementation.
- A future behavioral reimplementation should avoid copying GPLv2 Arduinoboy firmware into
  SameBoy's Expat-licensed source without explicit licensing resolution.
