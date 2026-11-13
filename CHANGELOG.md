# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- **A looper in the standalone app**, next to the metronome. One button, or the `L` key so a
  hand stays on the guitar, records, closes the loop and plays it, then lays parts on top; press
  it again to go back to playing. `Shift+L` stops. Up to two minutes at 48 kHz (one at 96).
- **Loops that keep time with the click**: while the click runs, a loop starts exactly on the
  next "1" (a press just after the "1" still catches it), the closing press is rounded to the
  nearest bar line, and an optional one-bar count-in gives you a bar of clicks first. Once it
  plays, the click stays locked to the loop, pass after pass. Without the click the loop is as
  long as you played.
- **Undo and Redo of the last overdub, and Clear with Undo.** Feedback sets how much of the old
  take stays under a new part (100 %: all of it, 0 %: you replace it); Level sets how loud the
  loop plays.
- The looper records what you hear, effects included and the click left out, so you can change
  presets and solo over your rhythm part. It allows for the interface's delay and Transpose's,
  so what you play in time with the click plays back in time with it, layer after layer, and
  every start, stop, punch-in and the seam where the loop wraps is a 10 ms crossfade: no clicks.
- **Pedals after the amp.** A modulation pedal (Ripple, Beacon, Gyre, Jetty) can stand after the
  amp and the cab, where a rig's effects loop puts it: a phaser or a chorus that moves the
  finished amp sound instead of feeding the amp. On the board the amp is now a block in the
  chain; drag a modulation pedal across it, or use the arrow keys or the move buttons, or add it
  there from its card's **After the amp** button. Other pedals stop in front of the amp, and the
  board says why. Crossing the amp fades the pedal out on one side and in on the other: no gap.
- **Stereo modulation after the amp.** There the phaser and the flanger sweep the two sides a
  quarter of a cycle apart, and Ripple's chorus keeps its own spread. Summed to mono they never
  cancel and never get louder or deeper than the pedal on its own.
- **Any pedal twice.** Two drives stacked, a boost before and after, a gate on either side of the
  drives, a phaser before the amp and another after it: the shelf's **Add another** puts a
  second copy right after the first. It is *Kelp 2*, with its own knobs and switch, in scenes
  and in the host's automation list. The board holds up to 25 pedals, with at most seven drives
  and fuzzes, so the rig's latency never changes.

### Changed

- Sessions and presets with pedals after the amp or second copies open in earlier versions with
  those pedals in front of the amp and without the second copies; everything saved before opens
  exactly as it did.

## [0.9.0] - 2026-11-13

Play Sedef Amp live: learn any control to a foot controller, load presets with program changes and
switch between eight scenes per preset without a gap. Transpose is back, to play the whole rig up to
an octave lower or higher without retuning.

### Added

- **Transpose is back**, in the section tabs row, on every page: play the whole rig
  up to an octave lower or higher without retuning, chords included. It steps a semitone at a
  time and goes back to 0 in one click. Shifting down adds 3 ms of latency and shifting up
  10 ms; at 0 it is off and leaves the sound exactly as it was. Every attack comes out exactly
  that much later, not anywhere in a 3–50 ms window as in 0.2.0, so a drop tuning plays as
  tight as the strings. It is a new design: a model of the ringing partials is shifted on its
  own, and only the rest is spliced ([docs/dsp/transpose.md](docs/dsp/transpose.md)). Chords
  are cleaner than 0.2.0's, though not yet as clean as the quality bar asks; the page lists
  what it measures and what it misses.
- **MIDI learn: play Sedef Amp from a foot controller.** Right-click any knob, switch, scene or
  preset arrow (or press Shift+F10 on it), choose Learn MIDI, then press a footswitch or move a
  pedal. Sedef tells a momentary footswitch from a latching one by itself, and an expression
  pedal can sweep a knob's whole travel or a part of it, either way round. The MIDI panel, at the
  right of the section tabs, lists what is learned and forgets one or all. What you learn belongs
  to your controller, so it is the same in every session and host.
- **Program changes load presets**: program 1 the first preset in the library, 2 the second, and
  so on, or the preset or scene you assign in the MIDI panel, on any channel or the one you pick.
  The VST3 version receives controllers but no program changes (VST3 has none): step presets and
  scenes with footswitches there.
- **Scenes: 8 per preset.** A scene keeps which pedals, EQ banks, Doubler, Drift and Fathom are on,
  plus up to 16 knobs you choose (Remember in scenes). Switching is instant and gapless, through
  the same fades as turning the controls by hand, from the scene bar, a footswitch, a program
  change or host automation (the new Scene parameter; in Logic Pro, which sends no MIDI to
  effects, assign a controller to it). A scene remembers what you change while it plays, and one
  you have not used starts as a copy of the scene you left. Scenes are saved with the preset and
  the session; older presets and sessions open as before, and a preset with scenes opens in
  earlier versions without them.

### Changed

- **The reported latency includes Transpose's while it shifts.** Turning Transpose on or off,
  or changing its direction, changes it, and the host is told; steps in the same direction
  don't. The status bar shows Transpose's share.
- **The transpose parameter is called "Transpose" again and can be automated.** Sessions that
  saved a setting (0.2.0) play it again; sessions and automation saved since load unchanged.
- **The update pill is in the status bar**, at the bottom right: the section tabs line now holds
  Transpose and the scene bar, and the pill sat on Transpose's stepper there.

## [0.8.1] - 2026-11-13

Sedef MIDI reads each note from the moment you pick it, so notes come sooner, with the maths and the
tests that keep them right; and the input meter now tells you when your level is right.

### Added

- **Sedef MIDI is tested to play the same whatever the host does**: the same notes at the same
  samples at any buffer size; the same notes within a millisecond at any input level and
  whenever the playing starts, and within 1.5 ms at 44.1, 48, 88.2 and 96 kHz; every semitone
  from F♯1 to E6 within the time the design works out for it; and `sedef-midi-render` writes the
  same file, byte for byte, run twice or at another buffer size.
- **Input level guide.** The input meter now marks where your level should land, and a chip
  under it says in words whether it is right: Level good, Too quiet, Too hot or No signal. Click
  it for a panel that says what to turn (your interface's gain or Input), with the loudest peak
  of the last few seconds and the Input knob at hand. Before, the meter had no target and turned
  amber, a warning, right at the level the presets were made for.
- **Audio settings in the standalone app.** Settings, and the input level panel, open the app's
  audio device window: pick your interface and its input without hunting for the title bar's
  Options button.
- **T opens and closes the tuner.** The tuner says when nothing is coming in, with a link to the
  input level panel.

### Changed

- **Sedef MIDI: notes come sooner in Fastest.** A string repeats itself from the moment you
  pick it, and Sedef MIDI now reads the note from that instead of waiting for two full periods
  of it and a safety margin. From the pick to the note: a low E 22 ms (was 28), an A 17 ms
  (22), a G 13 ms and the notes above it 9 to 12 ms (all 16 before); on seven and eight
  strings a low B 29 ms (37) and a low F♯ 37 ms (47). The same note picked again and again
  comes within 15 to 25 ms, where it took 33 ms on every string. It plays the same notes as
  before: when the signal alone does not settle a note (a soft pick under a string that still
  rings, a low string plucked near the bridge with a finger), it waits as it did. Steady is
  unchanged.
- The amp cards' descriptions fit; they were cut off at every window size.
- The Metronome and Settings panels open under the header instead of over the Doubler switch.
- The metronome says what a click on a beat does.

### Fixed

- The clip lights say when they have caught a clip, for screen readers too.
- Sedef MIDI: the gate's label no longer sits on the input meter.

## [0.8.0] - 2026-11-11

Sedef MIDI after its first session with a player: a held note is steady, low strings down to F♯1
are tracked, bends are smooth, and notes come sooner.

### Added

- **Sedef MIDI: Lowest note** (Auto, E2, B1, F♯1). Seven- and eight-string guitars and low
  tunings are tracked down to F♯1 (and to E1 in a dropped tuning); before, nothing under a low B
  was played. Auto is a six-string guitar, exactly as before, until you play a lower string, and
  then keeps the wider range (a host saves it with the session). A low note takes two of its
  periods to tell: 47 ms for a low F♯, 36 for a low B.
- **Sedef MIDI shows what the audio buffer costs**: the buffer in milliseconds and the
  interface's input latency at the bottom of the standalone app's window, with a way to a
  smaller buffer when it is long.

### Changed

- **Sedef MIDI: a held note sends no pitch bend.** In Notes + Bend the pitch wheel now stays at
  exactly its centre while you only hold a note, and a centred bend goes out with every
  note-on. Before, every small move of the string (its tuning, its drift as it fades) went out
  as pitch bend, about 190 messages a second, which made some instruments waver on every note.
  A bend starts once the string has moved a quarter of a semitone, follows it smoothly, and
  returns to exactly the centre. Notes mode is unchanged: it never sent pitch bend.
- **Sedef MIDI: a string bent across a semitone stays one note** in Notes + Bend, up to the bend
  range; past it the next note starts without a gap.
- **Sedef MIDI: a slide is not a burst of notes.** A fast slide gives the first fret it reaches
  and the fret it stops on; a slow one still gives every fret.
- **Sedef MIDI's standalone app starts with a 3 ms audio buffer** (128 samples at 44.1 or 48 kHz)
  instead of the system's usual 512: every note comes about 9 ms sooner. It does this once, also
  after an update; a size you pick afterwards in Audio input… stays.
- **Sedef MIDI's notes leave the MIDI port sooner with a long buffer**: at most 3 ms after their
  buffer arrives, not a whole buffer after their sample.
- Sedef MIDI's window is a little taller, for the new control.

## [0.7.0] - 2026-11-10

The Pedals page is redrawn: every pedal has its own enclosure and a full board fits on one
screen. There are 49 factory presets, named for their sound, in a preset browser you can scan.

### Added

- **32 new factory presets**, 49 in all: Clean 10, Crunch 11, High Gain 10, Lead 10, Ambient 8.
  Every pedal is now in at least two (Remora, Sargasso, Breaker, Anemone, Conch, Gyre and Jetty
  were in none), and each amp is covered from clean to its highest gain. A few use the EQ and
  the Doubler.
- `sedef-render --check` also plays every factory preset a quiet guitar (a DI peaking at
  -30 dBFS that dies away) and fails one whose gate cuts the note less than 24 dB under its
  peak. `sedef-render --di-gain DB` renders from a quieter or hotter DI.

### Changed

- **Factory presets are named for their sound**: "Glassy Chorus", "Tight Metal Boost", "Surf
  Tremolo" instead of Nacre, Holdfast, Tidewater. The 17 presets of 0.5.0 sound as they did and
  keep their ids, so favourites, Recent and saved sessions still find them and show the new
  name.
- **The preset browser is one dense list.** One line per preset (its name and its chain with the
  amp), in two columns under category headers from clean to ambient, with about 30 presets in
  view where there were 7. The description, tags, author and cab show in a strip under the list
  for the row under the pointer or the highlighted one. Search, favourites, Recent and keyboard
  audition stay; the rail, the tag chips, the sort control and the detail pane are gone (search
  finds a tag, an amp or a pedal).
- **Previous and next step in a new order**, the one the browser lists: Clean, Crunch, High
  Gain, Lead, Ambient, then your own folders, each by name with the factory presets first.
  Until now they went through the factory categories alphabetically (Ambient first) and then
  your presets. The header buttons, `[` and `]`, and the automatable Preset Previous and Preset
  Next all follow it.
- Tight Metal Boost (was Holdfast) is 0.6 dB quieter: it was the one preset above the -16 LUFS
  the library is balanced to.

- **The Pedals page is redrawn as one board you can see whole.** Each of the sixteen pedals is
  its own enclosure: three widths and several heights by what it carries, its own knob layout
  and knob sizes, and a line figure painted on it, so a pedal can be told apart without reading
  its name. The board is drawn as large as its length allows, and every pedal there is fits in
  two rows without scrolling.
- **The signal path is drawn on the board:** a cable from the Guitar jack through the pedals,
  row by row, into the amp in use, which opens the Amp page.
- **A pedal's tools are on the pedal:** the handle, Move left, Move right and Remove sit on its
  top edge and show when the pointer or the keyboard is on it. A pedal also drags by its
  enclosure.
- **Add pedal is a shelf under the board** with a picture of every pedal, by category, and the
  ones on the board marked. Pointing at a pedal shows where on the board it will land and what
  it does. The shelf stays open after you add a pedal.

## [0.6.0] - 2026-11-10

Sedef MIDI comes with Sedef Amp: play MIDI instruments from your guitar, one note at a time. The
Doubler now hears quiet playing.

### Added

- **Sedef MIDI:** a companion that plays MIDI instruments from your guitar. It turns the guitar's
  DI into MIDI notes in real time, one note at a time, so you can play a bass line on the guitar,
  hear it on a bass instrument and record it as MIDI. A standalone app with its own MIDI port
  ("Sedef MIDI"; on Windows it sends to an output you pick), a plug-in (VST3, CLAP, AU) for the
  guitar track, and on macOS an instrument AU that takes the guitar on its side chain, so Logic's
  instrument tracks can record its notes. Octave −2 to +1, Notes or Notes + Bend, Fastest or
  Steady timing (every note 38 ms after you play it, for one track delay), velocity sensitivity
  and curve, MIDI channel, Panic; no stuck notes on stop, bypass, port change or quit. It works
  the same with the interface set low or high (peaks from about −40 to −6 dBFS) with every
  setting at its default: the gate follows the input's own noise (Auto; Manual sets it by hand),
  and velocity follows how hard you play, over about your last 16 notes. Made for fingers as much
  as for a pick: nothing waits for a pick's click, a finger touching the string before it plucks
  it is not a note, and a note after a rest comes as quickly as any other. Its own parameter
  list, separate from Sedef Amp's ([docs/midi.md](docs/midi.md),
  [design](docs/design/guitar-to-midi.md)).
- **Installers:** Sedef MIDI comes with Sedef Amp on every OS, selected by default: its own choice
  in the macOS installer, a component in the Windows setup (with a Start menu entry), and part of
  the Linux archive (`./install.sh --no-midi` leaves it out). The macOS uninstaller removes it
  too, or only it with `--midi` ([docs/install.md](docs/install.md)).
- **Updates** keep Sedef MIDI when it is installed and leave it out when it isn't
  ([docs/updates.md](docs/updates.md)).
- Updating from 0.5.0 or earlier with **Update now** adds Sedef MIDI on macOS and Linux. On
  Windows it doesn't: run the new setup once to add it.

### Fixed

- **The Doubler hears quiet playing.** Its onset detector judged attacks against a fixed level,
  so a player peaking around −30 dBFS with soft attacks (fingerstyle, a neck pickup) got few or no
  new takes per note. It now judges them against the input's own hiss, hum and buzz, the way
  Trench's gate does: the same playing 36 dB quieter gives the same onsets, and hiss, hum and buzz
  alone still give none. At normal levels nothing changes; the factory presets render the same
  ([docs/dsp/doubler.md](docs/dsp/doubler.md)).

## [0.5.0] - 2026-11-09

The Pedals page is a pedalboard you build yourself, with five new filter and modulation pedals;
latency is down to 0.85 ms; updates install themselves; and Trench divides chords and works at
any input level.

### Added

- **Settings > Latency:** Low latency, the default, or **Linear phase**, the 0.4.0 filters, which
  keep every frequency in time for mixing and re-amping and add about 5 ms at 48 kHz. Switching
  restarts the audio for a moment and tells the host the new latency; the status bar and About
  follow it. Sessions saved with 0.4.0 open in Low latency.
- New parameter: `oversampling` (global, not automatable, not saved in presets).
- **Five filter and modulation pedals**, each with its own enclosure on the pedalboard:
  - **Anemone**, an envelope filter: low-, band- or high-pass that opens (or closes) with your
    picking, with a fast attack and a slower release for funk
    ([docs/dsp/anemone.md](docs/dsp/anemone.md)).
  - **Conch**, an inductor wah: Position is the rocker (automatable), Resonance sets the peak, and
    Auto rocks the pedal from your picking ([docs/dsp/conch.md](docs/dsp/conch.md)).
  - **Beacon**, a photocell vibe: four staggered stages and a lamp that throbs rather than sways,
    in Chorus or Vibrato ([docs/dsp/beacon.md](docs/dsp/beacon.md)).
  - **Gyre**, a phaser with 4 or 6 stages and Feedback ([docs/dsp/gyre.md](docs/dsp/gyre.md)).
  - **Jetty**, a bucket-brigade flanger with Feedback in either polarity
    ([docs/dsp/jetty.md](docs/dsp/jetty.md)).

  By default Anemone and Conch go between Trench and Remora, before the drives; Beacon, Gyre and
  Jetty after the drives, before Ripple. All five are off by default and add no latency, so
  existing sessions and presets sound the same.

- New parameters (177 → 202): `anemoneEnabled`, `anemoneSensitivity`, `anemoneRange`,
  `anemoneResonance`, `anemoneMode`, `anemoneDirection`, `conchEnabled`, `conchMode`,
  `conchPosition`, `conchResonance`, `conchSensitivity`, `beaconEnabled`, `beaconSpeed`,
  `beaconDepth`, `beaconMode`, `gyreEnabled`, `gyreSpeed`, `gyreDepth`, `gyreFeedback`,
  `gyreStages`, `jettyEnabled`, `jettySpeed`, `jettyDepth`, `jettyDelay`, `jettyFeedback`.
- **Sea Lantern** factory preset: Lagoon pushed by Coral into the Beacon vibe, with a plate.
- **Pedalboard:** the Pedals page is a board. Add pedals from a picker grouped by category, take
  them off (with Undo), and drag them into any order by their handle, or move them with the
  arrow keys or the move buttons. The board is saved with the session and with presets, and
  A/B, preset Undo and the browser's audition carry it. Pedals off the board cost no CPU, and
  the plug-in's latency stays the same whatever is on it. A reorder is a short duck (about
  12 ms) rather than a click; adding and removing use each pedal's bypass fade
  ([docs/architecture.md](docs/architecture.md#the-pedalboard)).
- The Gate is a board pedal, first by default; it still listens to the guitar wherever it sits.
- `sedef-render --board gate,tide,…` renders through a given board (without it, a preset's own
  board), and `--switch 2.5=ripple,gate,…` changes the board mid-render.
- **Trench Poly:** a Mono/Poly switch on Trench. Poly divides every string of a chord, so open
  chords, power chords and barres get their octave down: each note's sub lands within 1.3 cents,
  what is neither a note nor one of its partials stays 24 dB under the rest, and a sub speaks
  7–12 ms after the pick. Mono, the default, is the analog-style divider for single notes, so
  sessions and presets made before keep their sound
  ([docs/dsp/trench.md](docs/dsp/trench.md)).
- New parameter (202 → 203): `trenchMode` (automatable; Mono by default).

### Changed

- **Latency is down from 6.2 ms to 0.85 ms at 48 kHz** (298 to 41 samples; 296 to 40 at
  44.1 kHz, 295 to 32 at 96 kHz). Every pedal and the amp now oversample through minimum-phase
  filters, about 5 samples each instead of 37, quick enough to play through live. The latency
  the host compensates still never changes when you switch a pedal or the amp. The tone is the
  same: responses and harmonics match the previous filters to 0.05 dB, and the new filters reject
  aliasing harder (what they let fold back into the top of the band at 44.1 kHz is down from
  −25 dB to −94 dB). Captures convert their sample rate the same way, with no look-ahead
  ([docs/dsp/oversampling.md](docs/dsp/oversampling.md),
  [ADR-0009](docs/adr/0009-minimum-phase-oversampling.md)).
- **Updates install themselves:** **Update now** downloads the new version into a private folder,
  verifies it and installs it through your OS's own installer, so there is no disk image to open,
  no installer to click through and nothing to delete by hand. You answer your OS's password or
  permission prompt; the installer replaces the formats you have. On macOS Apple's installer runs
  on the verified package; on Windows the setup waits until your DAW has closed (the plug-in is
  locked until then); on Linux the archive's `install.sh` runs, through `pkexec` for a system-wide
  install. The panel then says "Sedef Amp <version> is installed. Restart <your DAW> to use it.", in
  every DAW and window that still runs the old version, and the standalone app offers **Restart
  Sedef**. Where that can't run (a copy outside the installer's folders, a sandboxed host, no
  `pkexec`) or anything fails, **Install manually** saves the verified installer to Downloads and
  opens it as before ([docs/updates.md](docs/updates.md)).
- This takes effect from the update after this one: 0.2.0, 0.3.0 and 0.4.0 install this release
  the old way (the installer in Downloads), and from this release on updates take one click.
- **Windows setup:** `/WAITPID=<pid> /WAITPIDTIME=<its creation time>` waits for that process to
  exit before installing (not for a later process that got its id), holding the setup file so
  nothing can change it meanwhile, and `/RELAUNCH` opens the standalone app afterwards.
- **Releases** are installed over the previous release on every OS, through the updater's own
  code, before they are published ([docs/releasing.md](docs/releasing.md#install-test)).
- Sessions and user presets saved before the board (0.4.0 and earlier) open with the whole fixed
  chain it replaces (Gate, Trench, Anemone, Conch, Remora, Tide, Urchin, Sargasso, Pearl, Kelp,
  Coral, Beacon, Gyre, Jetty, Ripple, Breaker; 0.3.0 had Gate, Tide, Pearl, Coral and Ripple, in
  that order), so every pedal whose settings they saved is in view, and they sound exactly as
  they did.
- Every factory preset carries a board with just the pedals it uses, and sounds exactly as it did
  (a test renders each one against the fixed chain). A new instance and Init open with the Gate
  alone, the only pedal on by default; add the rest from the picker.
- The Pedals tab shows a lamp for each pedal on the board, in its order.
- **Trench tracks real playing.** On real guitar DI its sub chopped on and off 12–20 times a
  second: a bright pickup's 2nd and 3rd partials, the pick and the last note still ringing threw
  its period detector off. A new note tracker holds the note through all of them: on a recorded
  fingerstyle piece the sub is on 97 % of the time instead of 69 %, with 1.3 flips a second
  instead of 19.6. It locks in about two periods of the note (28 ms on the low E, 11 ms from E4
  up), follows vibrato, bends and hammer-ons, and keeps palm-muted sixteenths at an even level.
- **Trench works at any input level.** Its gate, which keeps hiss and hum from being divided,
  was a fixed level, and with the interface gain set low it stayed shut: on a fingerstyle DI
  peaking at −30 dBFS the sub spoke on a fifth of the notes. The gate now follows the input's
  own noise floor, so that DI gets a sub on every note in Mono and Poly, as it did at −6 dBFS.
  Silence, hiss and hum still get no sub ([docs/dsp/trench.md](docs/dsp/trench.md#the-level-gate)).

## [0.4.0] - 2026-11-04

Abyss is tight instead of fuzzy, and six new pedals: a mid-hump overdrive, an octave-up fuzz, an
analog-style octave down, a two-transistor fuzz, a sustain fuzz and a treble booster.

### Added

- **Three new pedals** on the pedals page, in signal order ([architecture](docs/architecture.md)):
  - **Trench**, an analog-style octave down: one and two octaves below the note you play, from
    a divider that tracks single notes and fades its subs out on chords and silence instead of
    sputtering. Sub −1, Sub −2, Direct and Tone. It sits first, right after the gate, where it
    hears the clean guitar ([docs/dsp/trench.md](docs/dsp/trench.md)).
  - **Urchin**, an octave-up fuzz modelled on the transformer-and-diodes circuit: Fuzz, Octave,
    Tone and Level. Like the original, its octave rings clearest on single notes on the neck
    pickup, above the 12th fret ([docs/dsp/urchin.md](docs/dsp/urchin.md)).
  - **Kelp**, the classic mid-hump overdrive: Drive, Tone and Level. Drive down and Level up, it
    is the mid-pushed boost that tightens a high-gain amp ([docs/dsp/kelp.md](docs/dsp/kelp.md)).
- New factory presets: **Holdfast** (Kelp boosting Abyss), **Thermocline** (an octave-up fuzz
  lead) and **Benthic** (riffs doubled an octave below). New preset tags: `fuzz`, `octave`.
- New parameters: `kelpEnabled`, `kelpDrive`, `kelpTone`, `kelpLevel`, `urchinEnabled`,
  `urchinFuzz`, `urchinOctave`, `urchinTone`, `urchinLevel`, `trenchEnabled`, `trenchDirect`,
  `trenchSub1`, `trenchSub2`, `trenchTone`.
- **Remora**, a two-transistor feedback fuzz, right behind Trench at the front of the board.
  Germanium is soft and warm, silicon harder, brighter and louder; Bias moves from a gated,
  sputtering starve through symmetric at noon to hot; Guitar volume stands in for the guitar's own
  knob, and rolling it back cleans the fuzz up (from 42 % to 7 % distortion between 10 and 5, only
  3 dB quieter) the way the real circuit does ([docs/dsp/remora.md](docs/dsp/remora.md)).
- **Sargasso**, a four-stage sustain fuzz after the compressor and Urchin. Sustain holds a plucked
  note above −20 dB for 1.7 s at minimum and 3.5 s at full; Tone blends a low-pass and a
  high-pass, with a 9 dB mid scoop near 1 kHz at noon ([docs/dsp/sargasso.md](docs/dsp/sargasso.md)).
- **Breaker**, a germanium treble booster last on the board, straight into the amp. Treble peaks
  at 1.5 kHz and keeps the lows out; Full peaks at 460 Hz for a broad boost; Boost goes from
  +3 to +31 dB at the peak and clips softly when pushed ([docs/dsp/breaker.md](docs/dsp/breaker.md)).
- Both fuzzes leave at their input level with Volume at noon, every pedal switches click-free, and
  aliasing on high notes stays below −69 dB.
- New parameters: `remoraEnabled`, `remoraFuzz`, `remoraVolume`, `remoraBias`,
  `remoraGuitarVolume`, `remoraTransistor`, `sargassoEnabled`, `sargassoSustain`, `sargassoTone`,
  `sargassoVolume`, `breakerEnabled`, `breakerBoost`, `breakerRange` (163 → 176).

### Changed

- The rig's latency is 1.5 ms longer at 48 kHz (187 samples): Kelp and Urchin are oversampled,
  and like Pearl and Coral they keep their latency when switched off, so turning pedals on and
  off never changes what the host compensates.
- **Abyss is tight again:** palm mutes stop instead of droning, power chords and triads keep their
  notes apart, and soft picking cleans up, without giving up the lead gain at the top of the knob.
  In 0.3.0 two fixed stages after the gain knob squared every note even at low gain, so Abyss
  behaved like a fuzz pedal. Now the gain knob does the work, the low strings hit the clippers
  about 18 dB later than the mids, and the knob turns like an audio-taper pot: crunch to a tight
  rhythm sound in the lower half, the lead wall at the top. At the default gain, eighth-note chugs
  at 150 bpm now fall 20 dB between hits (was 10), a power chord's sum and difference tones sit
  18 dB below its notes (was 11), and 24 dB of picking strength moves the output 7 dB (was 3).
  At full gain a single note still sustains about 5 s, even from a quiet interface. The tone
  colour is unchanged ([docs/dsp/amps.md](docs/dsp/amps.md#5-tight-not-fuzzy)).
- Abyss factory presets were re-voiced for the new knob: Mariana (gain 0.6) is a tight
  palm-mute sound, and the lead presets Riptide and Bioluminescence sustain as long as before.
- Remora, Sargasso and Breaker add another 111 samples at 48 kHz (298 for the whole rig, 6.2 ms):
  each is oversampled and, like the other drives, keeps its latency when switched off, so
  switching them never moves the timeline.

## [0.3.0] - 2026-11-03

More gain, mics you can place anywhere over the speaker, Neural Amp Modeler captures, a doubler
that sounds like a second take, and a calm tuner.

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
- **Free mic placement:** drag each mic anywhere over the speaker, not just along a line, or click
  the cone to bring the selected mic there. Scroll over a mic, or Option/Alt-drag it, to back it
  off; arrow keys move it, Page Up and Down change its distance, Home and End take it to the dust
  cap or the edge. New `Mic A/B Azimuth` parameters hold where round the cone it sits; old
  sessions open with the mics where they were.
- **Off-axis angle:** a new `Off-axis` knob (0–60°) per mic turns the capsule towards the dust
  cap. It trades treble, presence and a little proximity for a smoother sound, as a real mic
  does, and depends on the mic: large diaphragms lose more treble than small ones, a ribbon keeps
  its low end. The marker shows the angle. Custom IRs get it too.
- **The other speakers:** on the 2x12 and 4x12 a mic now also hears the cab's other cones. Backed
  off, it picks up their low end and the comb filtering of a real multi-speaker cab (its notches no
  deeper than a room would leave them), and moving it towards or away from them changes the
  sound; close up they stay in the background. A small map
  on the cab page shows the mics against the cab's speakers. The floor reflection follows how high
  the mic sits on the cone.

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
- The 2x12 and 4x12 sound slightly different with a close mic than before, because the other
  cones are now heard (up to 2 dB around 600 Hz with the default Dyn Classic on the 4x12); backed
  off, the difference is large. The 1x12 is unchanged on the grille, and factory presets keep
  their level.
- With a custom IR, the Position and Distance knobs are disabled: the IR holds its own placement.

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
- A host's parameter text now reads back as the same text. A level a hair above 0 dB used to show
  "+0.0 dB" and come back as "0.0 dB", and values next to a unit switch (999.6 Hz, 99.96 ms) did
  the same. Durations from 100 ms up show whole milliseconds.

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

[Unreleased]: https://github.com/mr-kaynak/sedef-amp/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.8.1...v0.9.0
[0.8.1]: https://github.com/mr-kaynak/sedef-amp/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/mr-kaynak/sedef-amp/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/mr-kaynak/sedef-amp/releases/tag/v0.1.0
