# AGENTS.md

Context for AI agents working in this fork. **Read this first, then read
`MIDI_INPUT_FEATURE.md`.**

This is a personal fork of [SameBoy](https://github.com/LIJI32/SameBoy) (a Game Boy /
Game Boy Color emulator). We are adding a **MIDI Input serial accessory** so a host MIDI
source can drive ROMs like **mGB** and **LSDJ** over the emulated Link Cable — essentially a
built‑in software **Arduinoboy**.

- Fork remote: `origin = github.com/jpalvarezl/SameBoy`
- Working branch: `feature/midi-input`
- Feature design & learning guide: `MIDI_INPUT_FEATURE.md` (root of repo)

---

## ⚠️ Working agreement — READ THIS

**This is a learning exercise. The human writes the feature code themselves.**

Your job is to **explain, guide, review, and help debug — not to implement.** Concretely:

- **Do NOT write or edit the feature implementation** (`Core/midi.c`, `Core/midi.h`, the
  `Core/timing.c` hook, and the Cocoa MIDI/CoreMIDI code) unless the human *explicitly* asks
  you to write that specific code.
- Prefer giving **specs, hints, and explanations** over code. When you must show code, keep
  it to small illustrative snippets, not drop‑in implementations.
- Explain C / Objective‑C idioms as they come up — the human is a senior dev but new to C/ObjC.
- It's fine (and encouraged) to run builds/tests, investigate the codebase, edit **docs**
  (`MIDI_INPUT_FEATURE.md`, this file), and diagnose crashes.
- **The human manages git.** Don't commit. You may `git add`/stage when asked, but leave
  committing to them. When staging, exclude `MIDI_INPUT_FEATURE.md` unless told otherwise,
  and never stage `.DS_Store`.

(History note: an earlier agent implemented all of Phase 1 unprompted and had to revert it.
Don't repeat that.)

---

## Build & run (macOS) — non‑obvious setup

Full **Xcode** is required (not just Command Line Tools) because the Cocoa app compiles
`.xib` files with `ibtool`. Two gotchas on this machine:

1. `xcode-select` points at Command Line Tools, so `ibtool` fails. Work around it **without
   sudo** by setting `DEVELOPER_DIR` for the build.
2. Xcode 26 / clang 21 is newer than SameBoy expects: a harmless warning in bundled HexFiend
   becomes an error under `-Werror`. Already fixed on this branch by adding
   `-Wno-implicit-const-int-float-conversion` to the Makefile `WARNINGS` (commit `0eea2a5`).

Build + launch:

```bash
cd ~/code/forks/SameBoy   # (path on this machine)
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer make -j$(sysctl -n hw.ncpu) \
  && open build/bin/SameBoy.app
```

- First build ~2 min (also assembles boot ROMs); incremental rebuilds are seconds.
- `make clean` for a fresh build. The Makefile auto‑globs `Core/*.c`, so new core files need
  no Makefile edit. Default `CONF` is **debug** (asserts live via `GB_CONTEXT_SAFETY`).
- Requires `rgbds` (`brew install rgbds`) and `sdl2` (already installed) for other targets.

### Known non‑issue
In **debug** builds, playing a battery‑backed game (mGB, LSDJ) can `SIGABRT` in
`GB_save_battery` via the main‑thread `batteryTimerExpired` racing the emulation thread's
`GB_ASSERT_NOT_RUNNING_OTHER_THREAD`. It's a pre‑existing debug‑only issue, **unrelated to the
MIDI work** — we deliberately are not fixing it. (It's compiled out in release builds.)

---

## Progress

- ✅ **Phase 0 — scaffolding** (committed `102d0f8`): `GB_ACCESSORY_MIDI` enum, `GB_midi_t` in
  the `accessory` `GB_SECTION` union, `Core/midi.{c,h}` stubs, `connectMIDI:` +
  `validateUserInterfaceItem:` in `Cocoa/Document.m`, "MIDI" item in `Cocoa/MainMenu.xib`.
  The menu item appears and toggles; the device is a no‑op.
- 🚧 **Phase 1 — mGB feed logic** (in progress, human implementing): make
  `GB_midi_input_byte` enqueue; add a feed function that clocks queued bytes into the GB when
  it's an armed external‑clock slave (`SC & 0x81 == 0x80`) via `GB_serial_set_data_bit`,
  MSB‑first; call it from `GB_serial_master_edge` in `Core/timing.c`. See `MIDI_INPUT_FEATURE.md`
  §5 Phase 1 for the spec.
- ⬜ Phase 2 — macOS UI source picker + CoreMIDI input → `GB_midi_input_byte`.
- ⬜ Phase 3 — LSDJ modes (sync/keyboard, GB→MIDI out). Phase 4 — SDL. Phase 5 — tests.

---

## Key facts so you don't re‑derive them

- **Direction of clocking matters.** mGB is a serial *slave* (external clock); the adapter is
  the *master*. The `serial_transfer_bit_start/end` callbacks fire **only when the GB is the
  master**, so they don't apply to mGB. Feeding a slave uses `GB_serial_set_data_bit()`.
- `GB_serial_master_edge()` (`Core/timing.c`) runs at the serial base rate regardless of
  master/slave — the natural periodic hook for feeding a slave.
- `Core/workboy.c` is the simplest complete serial peripheral — the reference to study.
- **Testing model:** SameBoy has *no* unit‑test framework and *no* serial‑device tests. CI
  runs ROM‑based image‑hash checks (`Tester/main.c` → `sameboy_tester`, hashes pinned in
  `.github/actions/sanity_tests.sh`). `CONTRIBUTING.md` bans new languages — keep tests in C11.

---

## Upstreaming notes

If this is eventually PR'd to `LIJI32/SameBoy`: keep the core device frontend‑agnostic (no
CoreMIDI in `Core/`); follow `CONTRIBUTING.md` C style (it's strict — read it).

`CONTRIBUTING.md` has **no policy on AI‑assisted contributions** (it's silent). The plan:

- These scaffolding files (`AGENTS.md`, `MIDI_INPUT_FEATURE.md`) are **intentionally committed
  during development** and **removed when opening the PR**.
- Transparency is the chosen approach: the PR description will **disclose that AI assistance
  was used**, alongside the human's understanding of the change. (Solo maintainer who values
  code quality — owning the code in review matters more than hiding provenance.)
- Also drop the local‑only HexFiend toolchain fix (`0eea2a5`) from the upstream PR.
- Curate a clean PR branch (rebase/squash) rather than pushing this working branch as‑is.
