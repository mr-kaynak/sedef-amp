# Robustness: re-preparing, damaged values, shared files and missing files

Status: design for 0.10.3, written before the fixes it describes. Owners: core maintainers.

A bug scan before 0.10.3 found seven defects and five suspicions in the same few families: the
audio thread and the message thread touching the engine at once, numbers from hosts and files
that are not numbers, files that several instances share, and files that are not there or lie
about themselves. This page states the rules the code now follows, so the next feature follows
them too. Each rule names the test that pins it.

## 1. Re-preparing while the host plays: the hold

### The defect

Settings > Latency (`oversampling`) changes the oversampling filters, which means preparing the
engine again: its buffers are reallocated. The plug-in did this on the message thread between
`suspendProcessing (true)` and `suspendProcessing (false)`. That call only takes JUCE's callback
lock. The VST3, AU and standalone wrappers hold that lock around `processBlock`, so there it
waited for the running block and kept the next one out. The CLAP wrapper
(`clap-juce-extensions`, `clap-juce-wrapper.cpp`, `process()`) never takes the lock: it reads
`isSuspended()` and then calls `processBlock`. In a CLAP host a block that started just before
the suspension ran on while `Engine::prepare` freed and reallocated what it was using.

### The handshake

`engine/include/sedef/core/ProcessHold.h`: one atomic 32-bit word. The low 16 bits count the
blocks inside `processBlock`, the high 16 bits count the holds raised.

```
audio thread, first thing in processBlock     message thread, to re-prepare
  s = word.fetch_add (1)                         word.fetch_add (1 << 16)        raise
  if s has a hold:                               wait until (word & 0xffff) == 0 (bounded)
      word.fetch_sub (1)                         … prepare …
      play silence, return                       word.fetch_sub (1 << 16)        lower
  … process …
  word.fetch_sub (1)
```

Both sides change the same word with one read-modify-write each, so they are totally ordered:
either the block's `fetch_add` comes first, and the holder's wait sees the block inside until it
leaves, or the hold comes first, and the block sees it and leaves without touching anything.
The audio thread never waits, locks or loops: three atomic operations a block, and a held block
is a `clear()` of the buffer. While held, the block also skips the live layer, so a MIDI message
in a held block is lost (a footswitch pressed in the few milliseconds of a latency change).

**States.** Open (blocks run); held (blocks play silence); a holder waiting for the block inside
to leave. Holds nest: two holders at once each count, and the audio stays out until both lower.

**The wait** spins with a yield for the first 64 rounds, then sleeps 100 µs a round, until the
block is out or 500 ms have passed. A block takes far less than that unless the host is stuck
or an offline render passes a huge block, so:

- **Settings > Latency** (`applyOversampling`, from the 20 Hz timer) gives up after 500 ms: it
  lowers the hold and tries again on the next tick. It never prepares while a block is inside.
- **`prepareToPlay`** also raises the hold. Hosts promise no block runs during it, so the wait
  returns at once; if it does not within 500 ms, it prepares anyway, as it always did.

**What the hold covers.** In both places: the engine's prepare, the capture rebuilt for the new
conversion (it must not play a block built for the old one) and, in `prepareToPlay`, re-amp and
the deck. The hold is raised after the engine lock is taken, so while a loader thread still
holds that lock the guitar keeps playing; the silence lasts only for the work itself. IR kernels
and the pitch tracker are rebuilt after the hold is lowered, as before: a kernel built for
another rate is ignored by the cab until then (`docs/dsp/cab.md`, section 6).

`suspendProcessing` is no longer used: one mechanism, the same in every format.

### Everything else the message thread changes while audio runs

Checked against the same race; each goes through a lock-free handoff the audio thread reads at
a block boundary, so none needs the hold:

| What | How it reaches the audio thread |
|---|---|
| Pedalboard | `RtMailbox<Board>` |
| Custom IRs, captures | `RtHandoff` per slot or chain; the old object is freed off the audio thread |
| Looper memory | one atomic pointer, set once and freed with the engine |
| Metronome pattern, looper device latency, oversampling request, tuner flags | atomics |
| MIDI map, scene bank, learn flag | `RtMailbox` and atomics in `live::LiveControl` |
| Transpose's expected latency | set inside the hold (it writes the shifters' settings) |

Sedef MIDI changes its converter only in `prepareToPlay` and on the audio thread.

### Tests

- `engine/tests/core/ProcessHoldTests.cpp` `[concurrency]`: the protocol step by step on one
  thread (enter, raise, back off, lower, nesting, the timeout); then an audio thread that checks
  a two-field structure is never half-written while a holder rewrites it under the hold, and an
  `Engine` prepared again under the hold while another thread processes it. The TSan CI job runs
  them 20 times; a missing step in the protocol is a data race it reports.
- `plugin/tests/ProcessorTests.cpp`: a thread calling `processBlock` without pause while the
  message thread flips Settings > Latency 200 times; the output stays finite and the latency
  follows the setting.

## 2. Numbers from hosts and files

A value read from a session, a preset, `midi.json`, `update.json` or a release manifest, or set
by a host, is someone else's. The rules:

1. **Present but null, or of the wrong type, is absent**: the default applies.
2. **A number must be finite before it is clamped or converted.** `jlimit` and `std::clamp` let
   NaN through, and converting NaN or a value outside `int`'s range to `int` is undefined
   behaviour in C++. Integers are range-checked as doubles first.
3. **Parameters are sanitised where they enter the engine**: every block, the processor copies
   `kParamSpecs[i].sanitise (raw)` into the snapshot (non-finite becomes the default, then the
   range clamp and the step), as Sedef MIDI already did. A host's NaN automation therefore
   plays the default for as long as it lasts.
4. **A restored session never stores a non-finite value**: each parameter of the session is
   sanitised before APVTS sees it, and the sanitised text is what the next save writes. A preset
   saves sanitised values too (JSON has no NaN; one would make the file unreadable).
5. **The engine recovers**: a smoother given a non-finite target keeps its old one, and one whose
   current value is non-finite takes the next finite target at once, so nothing that once saw a
   NaN stays NaN. Non-finite input samples (a broken upstream plug-in or driver) are read as
   silence, so they cannot fill the filters' state with NaN.

| Source | Field | Rule |
|---|---|---|
| Session `Extra` | `uiScale` | a finite number in 0.5–2.5, else 1 (a NaN gave a 0 × 0 window) |
| | `metronomePattern` | 13 integers 0–3, else the pattern is left as it is |
| | `board`, `scenes`, `presetId`, `presetName`, `uiState` | text, parsed by their own readers |
| Session `SedefParameters` | `value` | rule 4 |
| Session `IRs/Slot` | `mic`, `cab`, `path` | integers in range; an absolute path (section 4) |
| Preset JSON | `parameters.<id>` | a number or a boolean, else the default |
| `midi.json` | `channel`, `controller`, `program`, `scene`, `low`, `high` | finite numbers in range, else the binding or assignment is skipped |
| `update.json`, release manifest | `size`, `lastAttempt`, `lastSuccess` | finite and inside `int64`, else 0 |
| Bridge (the page) | scale, pattern, slot numbers | the same helpers |

The helpers are `plugin/src/SafeVar.h`: `finiteNumber`, `toIntIn`, `toFloatIn`.

Tests: `plugin/tests/ProcessorTests.cpp` (every parameter set to `nan` in a session restores its
default and plays finite; a NaN host value plays the default; a session whose attributes are
replaced by odd text, 1200 times from fixed seeds, restores without crashing, plays finite and
keeps a usable window size), `LiveStateTests.cpp` (a `midi.json` with `1e999` and `null`
fields), `engine/tests/core/SmoothedTests.cpp` (a NaN target and a NaN state recover),
`engine/tests/engine/EngineTests.cpp` (a NaN input block, then the rig plays again), the update
tests (a manifest with `1e999` sizes).

## 3. Files several instances share

`midi.json` (the MIDI setup), the presets' settings (favourites, Recent), `captures.json` (recent
captures) and `update.json` live once per user. Every instance in every host, and the standalone
app, reads and writes them.

**Locking.** JUCE's `InterProcessLock` is a POSIX `fcntl` record lock on macOS and Linux. Those
locks belong to the process, not the object: two instances in one host both "hold" it at once,
and the first to let go releases it for both. Every shared file's read-modify-write now runs
under `SharedFileLock` (`plugin/src/SharedFileLock.h`): a process-wide mutex for the file's name,
then the inter-process lock. Instances in one process wait for each other; processes wait on the
file lock.

**Merging.** A whole-file write made from an instance's own copy loses what another instance
wrote since that copy was read. The MIDI setup re-reads `midi.json` under the lock before every
edit (when its text differs from what this instance last read or wrote), applies the one edit to
what it read, and writes that. Two instances editing within the one-second poll keep both
edits. An edit the panel makes by position names the binding by its target, so it reaches the
same binding in the fresh copy. The recent captures and the preset settings already re-read
before writing; they only needed the lock.

Tests: `plugin/tests/LiveStateTests.cpp` (two processors, each binding a different control
without polling: the file holds both); `plugin/tests/SharedFileLockTests.cpp` (two locks of one
name in one process exclude each other).

## 4. Files that are not there

**Custom IRs** follow captures (`docs/dsp/nam.md`, section 7). A session restored while its IR
file can't be read (a drive not mounted, the project opened on another computer) keeps the
slot's path as a **missing** reference: the slot loads nothing (the mic plays Dyn Classic, as an
empty Custom IR slot does), the next save writes the same path, and the Mic strip says
"Cab.wav is missing" with where it was and a **Locate…** button that loads a file into the slot.
When the file is back, opening the session loads it. Sessions saved before 0.10.3 load as they
did; only the missing case changes.

The path is kept as text and never opened unless it passes the same checks as before (absolute,
a regular file, at most 64 MB), so a session can't make the plug-in read a pipe or a device. A
load the player starts that fails (not audio, or refused below) leaves the slot as it was: the
IR that plays stays, and stays in the session.

## 5. Files that lie about themselves

**IRs.** A WAV header can claim any sample rate up to 4 294 967 295 Hz. The resampler's work per
output sample grows with the rate ratio, and the plug-in read up to one second of the file at
its claimed rate: a hostile 64 MB file kept the loader busy for minutes, and did it holding the
engine lock, which `prepareToPlay` and the 20 Hz timer wait on. Now:

- IR rates from **8 kHz to 384 kHz** load (every rate audio hardware runs at, and the IRs
  published for them); anything else is refused: "Cab.wav says it runs at 4,294,967,295 Hz:
  Sedef reads IRs from 8 to 384 kHz." `CabSim::makeKernel` refuses the same range, so no caller
  can bypass it.
- One second at 384 kHz is 384 000 frames, and the resampler's sum is at most
  `2 · 32 / 0.95 ≈ 68` taps per output sample times the larger rate over the smaller, so a
  kernel costs at most about 26 million multiply-adds (≈ 30 ms) at any host rate up to 384 kHz.
- **Kernels are built outside the engine lock.** Under the lock the loader reads the engine's
  rate and block size; it builds without it; under the lock again it publishes, if the rate
  and block size are still the ones it built for and no newer load or clear for the slot came
  meanwhile. If the engine was prepared again in between, it builds again.

**Re-amp files.** The converted copy is a 32-bit float WAV, which RIFF limits to 4 GB. The
length after conversion, `ceil (frames · device rate / file rate)`, is checked against
`WavFloatWriter::maxFrames` **before** converting, and the panel says why: "Long.wav is
4 h 12 min long; converted to 48.0 kHz it would pass the 4 GB limit of a WAV file. Trim it, or
set the audio device to its rate (44.1 kHz) so it plays without converting." (Before, it
converted for minutes and then blamed disk space.) Files claiming a rate outside 8–384 kHz are
refused like IRs.

Tests: `engine/tests/cab/` (makeKernel refuses rates outside the range at once, and builds at
both ends of it), `plugin/tests/IrLibraryTests.cpp` (the refusal message; a slot's kernel built
while the engine is prepared again ends at the new rate; the engine lock is free while a kernel
builds), `plugin/tests/RecorderTests.cpp` (a file whose converted length passes the limit is
refused before any conversion, with the reason).

## 6. The suspicions

| Suspicion | Verdict | Evidence and fix |
|---|---|---|
| Two instances in one host can lose a recent captures entry | Real | Two `InterProcessLock`s of one name in one process are both entered at once (`SharedFileLockTests.cpp`). Fixed by `SharedFileLock` (section 3). |
| A session saved within 400 ms of a metronome step edit reopens with the older pattern | Real | On connect the page sent its saved UI state's pattern to the plug-in, over the session's own. The handshake now carries the plug-in's pattern and the page takes it from there; nothing is sent back on connect. |
| A dropped `.nam` read with `File.text()` loses a byte-order mark and changes invalid bytes | Real | `text()` decodes UTF-8: it drops a BOM and turns invalid bytes into U+FFFD, so the kept copy's hash differed from the file's. The page now sends the bytes (base64, `sedef_capture_loadBytes`). |
| With the tuner open and muted, a device restart lets audio through until the async notice | Real | The mute followed "analysis enabled", which `prepareToPlay` clears until the message thread restarts the tracker. The mute now follows "tuner open", which a prepare does not change. |
| Two instances editing MIDI bindings within the one-second poll: the later write wins | Real | Whole-map writes from each instance's copy. Fixed by re-reading under the lock before each edit (section 3). |
