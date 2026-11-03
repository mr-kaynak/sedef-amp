# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **Doubler controls:** Variation (how loose the second take is), Width and Mix, next to Spread,
  and a sketch of the two takes showing how far behind the second one lands.
- **Doubler switch** in the header, under the output meter, so double tracking is one click away
  on every page.
- **Captures:** Neural Amp Modeler (`.nam`) files play in place of the amp. Pick Capture next to
  Lagoon, Reef and Abyss, then load a file or drop one on the panel; the last twelve are a click
  away. WaveNet (including the 0.6/0.7 additions), LSTM, ConvNet and Linear captures load, at any
  host rate (Sedef converts to the capture's own rate). Every capture is levelled to the built-in
  amps; input and output trims, "Through cab" (off by default for full-rig captures) and, for
  captures that record it, the interface level (a rig setting, not stored in presets). Switching
  and replacing crossfade. Sessions store the capture itself, so they play the same anywhere;
  presets keep a reference, and a missing file is named with a way to find it. A capture too
  heavy to play in real time is refused with a message saying why
  ([docs/dsp/nam.md](docs/dsp/nam.md)).
- New parameters: `ampSource`, `captureInput`, `captureOutput`, `captureCab`,
  `captureInterfaceLevel`.
- `sedef-render --capture FILE.nam` renders through a capture.

### Changed

- **Amp gain:** Abyss at full gain is now a modern high-gain wall: light picking and single notes
  are fully saturated and a note sustains about 5 s within 6 dB of its peak even from a quiet
  interface (was 1.6 s), while the low end stays as tight as before. Reef at full gain is a hot
  British crunch bordering on lead. Both gain knobs do something over their whole travel, from
  edge of breakup to saturation, and the level stays put as you turn them. Lagoon is unchanged.
  Factory presets were re-balanced so each keeps its character and level.
- The hard-driven stages of Reef and Abyss use antiderivative anti-aliasing: with 13-24 dB more
  gain, they alias no more than v0.2.0 did at full master, and Abyss less.
- The amps' input level is documented
  ([docs/dsp/amps.md](docs/dsp/amps.md#4-gain-targets-and-input-level)): strum hard and set
  Input Gain so the input meter peaks around -4 to -2 dBFS, or set it to your interface's maximum
  input level in dBu plus 0.6 dB.
- **Doubler:** the second take now differs from note to note, as a real one does. At every pick
  attack it draws its own timing (around Spread, with a memory of the phrase), a detune of a few
  cents, and a slightly different level and tone, then moves there between notes, so it never
  sounds like a fixed delay or a chorus. The side you play is passed through untouched, with no
  delay. With a mono output it stays off, since there it could only comb-filter your part
  ([docs/dsp/doubler.md](docs/dsp/doubler.md)).
- **Tuner:** a calm, steady needle. It reads the long, precise analysis window whenever the pitch
  holds and the quick one only while a peg turns, so hum, a string still ringing and the pick's
  sharp attack no longer make it swing (with hum 28 dB under a low E it now wanders 0.002 cents
  instead of 1.4). It keeps up with a peg turned from far off, also right after a pick or a
  re-pluck, and lands and settles on the pitch. In tune is confirmed after a moment and kept with
  hysteresis, the note name stays put from the pick to the end of the note and is the nearest one
  even at the edge of a note, and the last reading holds, then fades, when the note dies; a new
  reference renames it at once. The numbers change at most four times a second and show tenths
  only when steady, a hint under them says "Tune up", "Tune down" or "In tune", and screen readers
  hear each state once ([docs/dsp/tuner.md](docs/dsp/tuner.md#3-display)).
- The plug-in's latency rises by 2 samples at 48 kHz and by 0.46–0.81 ms at 88.2 kHz and above,
  so captures made at 44.1 to 192 kHz and the built-in amps share one latency, each capture with
  its full-quality rate conversion.

### Removed

- **Transpose.** It could not shift chords cleanly or keep a steady latency, and no design we
  measured could do both within a playable latency ([docs/dsp/transpose.md](docs/dsp/transpose.md)).
  The parameter stays so sessions still load; sessions that used it now play at the original
  pitch.

### Fixed

- **Custom IRs from sessions:** an IR path in a session or preset is only opened when it is an
  absolute path to a regular file of at most 64 MB, so a session can no longer point the plug-in
  at a pipe or device and hang it.
- **Stereo input:** a guitar plugged into one input of a stereo track no longer reaches the amp
  6 dB down in "Sum to mono". The live side now passes at unity when the other carries nothing,
  and a mono source copied to both sides is averaged as before.

## [0.2.0] - 2026-11-01

A preset browser you can audition from, an intonation workbench that works with a real guitar,
one-click installers, and updates that come to you.

### Added

- **Preset browser:** a three-pane panel over the rig: a library rail (all, favourites, recent,
  factory, user) with categories; a list with tag chips, search across names, notes, tags, amps
  and blocks, and sorting by name, recency or category, whose rows show the amp, the switched-on
  chain, a one-line note and tags; and a detail pane with the chain by stage and the preset's
  actions.
- **Audition:** a click or the arrow keys play a preset while the browser stays open; Enter or a
  double-click keeps it, and Esc restores exactly the sound you had when it opened, unsaved edits
  included.
- **Undo:** loading over unsaved edits (from the browser or prev/next) and deleting a user preset
  offer Undo in a toast (also ⌘/Ctrl+Z).
- **A/B compare** next to the preset bar, with copy between the slots and an edit marker per slot,
  and a Save button beside it; the bar shows the preset's category.
- **Shortcuts:** `P` opens the browser, `[` and `]` step presets, `A` and `B` pick the compare
  slot (never while typing, and ⌘/Ctrl+S and ⌘/Ctrl+F only in the preset search among text
  fields); the browser lists every shortcut.
- **Saving:** ⌘/Ctrl+S saves (Save as for factory presets), ⌘/Ctrl+Shift+S is Save as. Save as
  takes a category (or a new one), tags with suggestions and an optional note, and warns inline
  before replacing a preset.
- Presets may carry a one-line `description`; every factory preset has one, and the factory
  categories and tags follow one vocabulary (`presets/README.md`).
- **Updates:** a daily check for new releases (one HTTPS request, version and OS only), an
  Updates panel with release notes, and one-click download that verifies the installer's SHA-256
  before opening it and marks it as downloaded, so the OS checks it as well. Off in Settings,
  with `SEDEF_DISABLE_UPDATE_CHECK=1` or
  `-DSEDEF_UPDATE_CHECK=OFF` ([docs/updates.md](docs/updates.md)).

### Changed

- **Installers:** one download per OS with a stable name: a macOS disk image with an installer
  (no more copying bundles or clearing quarantine flags), a Windows setup program and a Linux
  archive with an install script. Each lets you pick the formats; the app now has an icon.
- Bridge protocol 2: presets report their amp, cab, active blocks and description; Recent, A/B,
  snapshots and undoing a delete are new native functions. Each preset state carries a rising
  `revision`, so the editor ignores an answer that arrives after a newer broadcast.
- Factory presets: Mariana and Coral Fuzz moved to a new High Gain category, Morning Ritual to
  Crunch. Sounds and levels are unchanged.

### Fixed

- **Presets:** favourites survive a settings file that can't be read, and plugin instances in
  different hosts no longer overwrite each other's changes. Renaming a preset to the same name in
  other capitals works on macOS and Windows, and a rename that fails keeps the old file. A
  category of ".." or a name made of dots can no longer save outside the presets folder.
- **Host automation:** preset loads, tap tempo and the preset footswitches springing back reach
  the host as gestures, so touch, latch and write automation record them.
- **State:** a session the host restores while the editor works on presets no longer mixes in the
  editor's values or names the wrong preset, and a saved session names the current preset as soon
  as it changes.
- **Intonation workbench:** the open-string step no longer waits for a reading held within one
  cent for a second, which a real plucked string never gives. Within 5 cents it moves on by
  itself; further off on the right note it says how far and offers **Use this tuning**. Both
  notes are now captured the same way from one pluck (one second from 0.6 s after it), which also
  fixes 12th-fret captures that restarted late on a wobbling note and read up to 1.5 cents off.

## [0.1.0] - 2026-11-01

The first public build: a complete guitar rig with a tuner and an intonation workbench.

### Added

- **Pedals:** noise gate, Tide (compressor with tilt EQ and parallel blend), Pearl (transparent
  overdrive with mid boost), Coral (distortion), Ripple (tremolo, chorus, vibrato) and polyphonic
  transpose (±12 semitones). Drive stages run oversampled with antiderivative anti-aliasing.
- **Amps:** Lagoon (glassy clean), Reef (British crunch, three voicings) and Abyss (tight lead),
  each with its own 9-band graphic EQ, low cut and high cut.
- **Cabs:** 1x12 open, 2x12 open and 4x12 closed, two mics per cab from ten mic models placed
  freely on the cone, room mics, and your own impulse responses (zero-latency convolution).
- **Time effects:** Doubler, Drift (multi-head tape echo with wow, flutter, tape drive and
  tempo sync) and Fathom (plate and hall reverbs).
- **Tools:** precision tuner, the intonation workbench (open string, 12th fret, saddle advice in
  millimetres for any scale length) and a metronome with odd meters (standalone).
- **Formats:** VST3, AU, CLAP and a standalone app for macOS (universal), Windows and Linux.
- **UI:** resizable web UI with keyboard control, screen reader labels and WCAG AA text contrast;
  preset browser with 13 factory presets balanced to -16 LUFS, user presets and favourites.
- **Mono, stereo and dual-mono** rigs, latency-aligned bypasses and host tempo sync.
- `sedef-render`, an offline renderer for presets, and loudness tooling for preset authors.

### Quality

- 202 engine tests on macOS, Windows and Linux, including a test that the audio thread never
  allocates, plus ASan, UBSan and ThreadSanitizer runs.
- Every CI build is checked with pluginval (strictness 8), clap-validator and auval, and a smoke
  test opens the editor in a real host and WebView on every OS.

[Unreleased]: https://github.com/mr-kaynak/sedef-amp/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/mr-kaynak/sedef-amp/releases/tag/v0.1.0
