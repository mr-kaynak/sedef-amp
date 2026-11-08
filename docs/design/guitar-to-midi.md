# Guitar to MIDI: play bass lines on a guitar (design)

Status: approved 2026-11-08; built in Phase 1 (engine layer, plug-ins, standalone app, test build).
What was measured, and where the build differs from this design, is in [As built](#as-built)
at the end. The user page is [docs/midi.md](../midi.md).

## Why

A guitarist who writes songs with Sedef has no bass guitar. He programs bass lines by hand, as
MIDI, into a sampled bass instrument, and finds that slow and hard. He wants to play the bass
line on his guitar and have it come out as MIDI in real time:
- plugged into his audio interface;
- driving a bass instrument (and sometimes a piano) that he hears while he plays;
- recorded as MIDI in his DAW, so he can edit it afterwards.

What the user told us:
- **Single notes only.** Bass lines are single notes; chords are not needed.
- **He uses Logic, on a Mac.**
- **A standalone version is required**, and so is a plug-in. The standalone app must show up as a
  MIDI input in other programs:
  - Logic;
  - the standalone versions of sampler and bass-instrument apps.
- **Real time.** He plays and hears the instrument as he plays.

## What it is

A new, small product in the Sedef repo, provisionally named **Sedef MIDI** (the builder may propose
another name in the marine style). It takes a mono guitar DI and sends MIDI notes.

1. **Standalone app (the primary path).**
   - It opens the audio interface's input.
   - It creates a virtual MIDI output named "Sedef MIDI": a CoreMIDI virtual source on macOS, an
     ALSA sequencer port on Linux.
   - Any program picks it as a MIDI input, as it would a keyboard.
   - It can also send to an existing MIDI output instead, such as an IAC bus or a hardware port.
2. **Plug-in** (AU, VST3, CLAP; mono audio in, MIDI out). It goes on the guitar's audio track:
   - The host routes its MIDI output to an instrument track, where the host supports that.
   - It can also send to the same kind of virtual port as the standalone, so it works in hosts that
     can't route plug-in MIDI. This is off by default.

**Logic needs extra care:**
- Logic loads only AU. It does not pass MIDI from a plug-in on an audio track to other tracks the
  way other DAWs do. Logic 11 added internal MIDI routing ("MIDI From" on an instrument track);
  whether that accepts the MIDI output of an audio-effect AU is **unverified**.
- So for Logic, the standalone app plus the virtual port is the path that must work.
- Inside Logic, the plug-in's virtual-port option is the fallback.
- The musician has to confirm both on his machine. Nobody can run Logic here.

## Goals

- **Correct notes.** On single-note lines, every note played comes out as one MIDI note at the
  right pitch. Wrong or extra notes are worse than a little latency for this use: each one has to
  be found and deleted by hand.
- **Real-time feel.** Latency as low as the tracker allows, and measured per string.
- **Bass register.** An octave transpose, with −1 octave as the default because a bass sounds an
  octave below a guitar. The choices are −2, −1, 0 and +1 octaves.
- **Hammer-ons, pull-offs and slides give new notes.** The tracker follows a whole-step hammer-on
  like a bend, so the MIDI layer quantises the tracked pitch to semitones itself. It uses
  hysteresis, so vibrato and slightly sharp fretting don't flip notes. A slide gives the notes it
  passes through.
- **Velocity from picking strength,** with a sensitivity and a curve. Hard picks give high
  velocities and soft ones low, monotonically.
- **Clean note-offs.**
  - Muting the string ends the note.
  - Letting it decay ends the note below a threshold.
  - No stuck notes ever: stopping, bypassing, changing the port or quitting sends note-off for
    the sounding note and All Notes Off.
  - There is a Panic button.
- **Optional pitch bend.**
  - In Notes mode (the default), only notes are sent.
  - In Notes + Bend mode, bends and vibrato are also sent as pitch bend, with a settable bend range
    (default ±2 semitones) that must match the instrument's.
- **MIDI channel** setting, 1–16.
- **Standalone and plug-in behave the same:** one shared engine.

## Non-goals (for this version)

- Chords or polyphony.
- Built-in instrument sounds. He uses his own bass and piano instruments.
- Converting recorded audio to MIDI offline. DAWs already do this.
- MPE, per-string channels, hex pickups.
- Mobile.

## Anti-goals: these would make it a failure even if it "works"

- Glitch notes:
  - pick noise, fret squeaks or a ringing open string coming out as short extra notes;
  - octave jumps on low notes.
- Stuck notes.
- Latency that changes from note to note so much that the line feels loose. If a "steady" timing
  option exists (below), it must really be constant.
- Breaking anything in Sedef Amp: its sound, parameters, presets, release asset names, or the
  updater.
- Naming commercial products, companies or artists in the repo (ADR-0006). That includes the
  instruments the musician uses.

## Constraints

- **Clean room** (ADR-0006), AGPL (ADR-0007), the C++20 engine without JUCE (ADR-0001), and the
  JUCE plug-in shell (ADR-0002).
- **Real time** (ADR-0005): no allocation or locks on the audio thread.
  - MIDI to a virtual or hardware port leaves the audio thread through a lock-free FIFO to a MIDI
    thread, or through a JUCE call shown to be real-time safe.
  - Plug-in MIDI output uses the host's block, with sample-accurate offsets.
- **Pitch tracking reuses `sedef::analysis::MonoTracker`** (the tracker behind Trench's Mono
  mode). It already exposes:
  - note-on, note-change and note-off events;
  - how many samples ago the pick was, and the pick level (the velocity source);
  - continuous pitch and confidence.

  Its measured lock time is E2 28 ms, A2 22, E3 16, A3 13, and E4–E5 11 ms. Improve it there if
  needed, without hurting Trench, whose tests must stay green.
- **The release contract stays:**
  - the stable asset names (`Sedef-Amp-macOS.dmg`, `Sedef-Amp-Windows-Setup.exe`,
    `Sedef-Amp-Linux-x86_64.tar.gz`, `SHA256SUMS.txt`);
  - the updater's format detection.

  Sedef MIDI ships inside these installers as extra components (a pkg choice, an Inno component,
  install.sh files). The updater's component logic must treat it like another format: update it
  when it is installed, and never install it when it is not.
- **Testing:**
  - The repo's DI helpers and a public-domain set of real piezo DI recordings (kept out of the
    repo) are the test material.
  - No local GUI launches: the standalone app and the UI are smoke-tested in CI, as Sedef Amp's
    are.
  - Logic and third-party instruments are verified by the musician, from a checklist.

## Decided

- Single notes only; −1 octave by default; Notes mode by default.
- Standalone with its own virtual MIDI port, plus a plug-in, from one engine.
- Mac first: the musician's platform must fully work. Windows and Linux build and pass CI.
  - On Windows the standalone sends to a MIDI output the user picks (a loopback driver, or a
    virtual port where Windows MIDI Services supports it).
  - The docs say which.
- Lives in the Sedef repo, sharing its engine, CI, packaging and release.

## Calls I made (the user can overrule)

- **Separate product.** A separate small product, not a mode inside Sedef Amp. Its job (MIDI out,
  instrument tracks) and its window are different, and Sedef Amp's chain and parameters stay
  untouched.
- **Name.** "Sedef MIDI" is a working name.
- **"Steady" timing option**, off by default. The note-on is delayed to a fixed lag after the pick
  (e.g. 30 ms at E2's worst case). Every note then has the same latency, and a recording can be
  shifted earlier by one fixed amount (Logic's track delay) to sit exactly on the pick. The
  default "Fastest" sends each note as soon as it is sure, so low strings lag more than high ones.
- **Tip for the docs.** Playing the line higher on the neck with −2 octaves locks faster than
  playing it on the low strings with −1.

## Settled after the builder check (my calls; the user can overrule)

1. **Accuracy before latency, within a budget.**
   - The MIDI layer may wait to confirm a note before sending its note-on, at most 5 ms after the
     tracker locks. The latency bar becomes the tracker's lock time + 5 ms + one buffer.
   - Most of the accuracy work goes into the MIDI layer, which is free to be cautious.
   - Changes to MonoTracker are allowed only behind options whose defaults leave Trench's behaviour
     bit-identical, and Trench's tests must stay green.
   - A note-on that has been sent is never taken back. Glitches are filtered before the note-on, by
     confirmation and a level gate, not by a minimum note length.
2. **Installing and updating.**
   - Sedef MIDI ships as an extra component in the existing installers, **not selected by default**:
     a pkg choice that is off by default, an Inno component that is off by default, and an
     install.sh option. The musician ticks it once, under Customize; the docs say so.
   - The updater updates it only when it finds it installed, as for any other format.
   - Check that an updater with no knowledge of the new choice leaves it uninstalled:
     - macOS: the choice's default is off;
     - Windows: `/COMPONENTS=` lists only known components;
     - Linux: install.sh installs it only when asked.
   - Sedef MIDI shows a simple "a new version is available" notice from the same release feed,
     with its own page, or with a pointer to the download. It doesn't need the full one-click flow
     in this version.
3. **What becomes a new note, and what becomes pitch bend.**
   - Every semitone crossing, with hysteresis, is a new note, in both modes. A slide therefore
     gives the notes it passes through, and so does a whole-step string bend. Bends are rare in
     bass lines, so this is acceptable.
   - Notes + Bend adds pitch bend only for the deviation under a semitone (vibrato, slight bends),
     relative to the sounding note.
   - No glide-versus-fretted classifier in this version.
4. **Steady mode and late notes.**
   - The fixed lag is chosen so that at least 99 % of notes lock within it on the test material.
   - A note that locks later is still sent, as soon as it is sure, and counted. The "constant
     within 1 ms" bar applies to the notes sent on time; the late fraction is reported.
5. **The AU in Logic.**
   - Logic can't be tested here, so the first build that goes to the musician is a **test build**,
     not a release. It has:
     - the audio-effect AU;
     - an instrument AU variant that takes the guitar through Logic's side-chain input (Logic 11's
       "MIDI From" routing is more likely to accept an instrument's MIDI);
     - the standalone.
   - Both AU variants can also send to a MIDI output the user picks, such as the IAC bus, or to a
     virtual port of their own, as a fallback that doesn't depend on Logic's routing.
   - The musician tests the checklist, and the AU types and codes are fixed before the first
     release on what worked. A variant that doesn't work in Logic is dropped.
6. **Note-off comes from the level, not the tracker.** MonoTracker's own `noteOff` fires 0.70–0.78 s
   after a mute, because it rides out poor estimates and leaves level gating to the caller. The
   MIDI layer ends a note from its own level envelope instead: a palm mute or a staccato note has
   to end within tens of milliseconds. Its tests:
   - a muted note ends within 60 ms;
   - palm-muted eighths at 140 bpm give separate notes, never one long note;
   - a long, decaying note is not cut off early while it is still clearly audible.
7. **Real DI.** The public-domain piezo DI has no reference note list. The builder annotates a few
   single-note excerpts, using an offline pitch analysis checked by hand. The score on them is
   reported, not a gate. The gates are the synthetic tests.

## Left to the builder

- UI layout and stack. Keep Sedef's visual identity; reuse the WebView UI stack if it fits a small
  window, or argue for plain JUCE components. It needs:
  - an input meter;
  - a large display of the detected note;
  - the sensitivity (gate) control;
  - the octave, velocity sensitivity and curve, channel, mode and bend range settings;
  - the output port picker (standalone), and the virtual-port switch (plug-in);
  - Panic.
- Thresholds and hysteresis values, chosen from measurements.
- How the MIDI layer turns the tracker's events into notes: the semitone hysteresis, the minimum
  note length, and legato order (note-off before note-on, at the same timestamp).
- The plug-in's parameter list and IDs. They are append-only from the first release (ADR-0004);
  the product has its own schema, not Sedef Amp's.
- The packaging details and the updater changes needed for the new component.
- The product's own docs page, the README section, the CHANGELOG entry and screenshots.

## Acceptance (measured)

**Single-note lines**
- On synthetic DI (bridge pickup and piezo, with pick noise and the previous note ringing), with
  palm-muted eighths and sixteenths at 120–140 bpm on E2/A2, a legato run, slides and vibrato:
  - ≥ 99 % of notes correct: right pitch, one note each;
  - ≤ 1 extra note per 100 notes;
  - no octave errors.
- Real piezo DI (the public-domain recordings, kept out of the repo) is reported the same way.

**Latency**
- Per string, from the pick to the MIDI note-on, at 48 kHz with 64- and 256-sample buffers.
- At most the tracker's lock time plus one buffer. In Steady mode, constant within 1 ms.

**Velocity**
- Monotonic with picking strength across five strengths.

**No stuck notes**
- An automated test covers stop, bypass, port change and quit mid-note.

**Platforms**
- **macOS:** the virtual port appears in the system MIDI list (CI can check that with CoreMIDI).
- **Plug-in:** its MIDI output is sample-accurate in a test host.

**CPU**
- Reported in ns/sample at 48 kHz.

**Checklist for the musician**, in the README or docs:
- the Logic setup with the standalone;
- the Logic setup with the plug-in (Logic 11 "MIDI From", or the virtual-port fallback);
- a standalone instrument app taking "Sedef MIDI" as its input.

## As built

Phase 1 (2026-11-09): the engine layer (`sedef::midi::GuitarToMidi`, engine/src/midi), the plug-ins
and the standalone app (plugin/midi), the UI (ui/src/midi), tests, and a macOS test build from CI.
Installers, the updater and the final AU codes are Phase 2, after the musician's test.

**How notes are made.** The class comment in `GuitarToMidi.h` is the reference. In short: picks
come from the tracker's detector, from the note losing its own period (soft picks over a ringing
note, the same note picked again) and from a lock that comes with a level rise, and each is dated
to the sample from its click or the broken period. A picked note goes out once its pitch has
settled, its own period is periodic since the pick, and no lower note can be under it (above A3,
that means waiting one period of the low E). Legato changes must sit on their semitone; octave
corrections need the input to repeat clearly better at the longer period. Note-offs come from the
level (gate, mute). The MIDI layer adds 2 ms to the tracker's lock below A3.

**Accuracy** (synthetic DI, 48 kHz; the tests in engine/tests/midi gate it):

| Material | Notes | Right | Wrong (octave) | Missed | Extra |
|---|---:|---:|---:|---:|---:|
| Bass lines, bridge, neck and piezo pickups, 6 seeds, open strings damped | 864 | 99.4 % | 0 (0) | 5 | 2 |
| The same with open strings left ringing (reported) | 864 | 99.1 % | 3 (3) | 5 | 2 |
| Palm-muted eighths and sixteenths, 120 and 140 bpm, E2 and A2 | 256 | 100 % | 0 | 0 | 0 |
| Hammer-ons, pull-offs, slides | 69 | 100 % | 0 | 0 | 0 |
| Vibrato and slight bends | 8 | 100 % | 0 | 0 | 0 |
| A new note over the previous one ringing | 12 | 100 % | 0 | 0 | 0 |

The three octave errors with ringing open strings are an open string an octave below the played
note: the input then really repeats at the lower period.

**Real piezo DI** (reported, not a gate). The only real DI available is fingerstyle playing with
strings ringing into each other, outside the single-note design. It fragments: in a hand-checked
eight-note scale, three notes come out right within 80 ms of their picks, three right but 60-120 ms
late, two an octave low, with six short wrong or repeated notes besides. Real single-note bass
lines are untested until the musician plays them.

**Latency**, pick to note-on, 48 kHz (identical at 64 and 256-sample blocks: events carry their
sample offsets; a live instrument adds one audio buffer):

| String | Tracker lock | Fastest | Steady |
|---|---:|---:|---:|
| E2 | 25.5 ms | 27.5 ms (max 30.0) | 37.8 ms |
| A2 | 19.5 | 21.5 (max 22.5) | 37.8 |
| D3 | 15.0 | 17.0 | 37.8 |
| G3, B3, E4, E5 | 11.5, 9.5, 8.5, 8.5 | 15.9 | 37.8 |

On one synthetic piezo E2 the tracker itself locks at 47 ms (its body swell reads as a second pick
25 ms in); the MIDI layer follows 2 ms later.

**Steady.** The lag is 38 ms: 99.83 % of picked notes in the test material are sure within it.
Single picks on every string land 37.8 ms after the pick, within the 1 ms bar. Across bass lines,
86 % land within 1 ms of the lag, 95 % within 3 ms and all within 6 ms: a soft pick over a ringing
note shows only as the note losing its period, which dates it to a few milliseconds, and a mute
just before a pick can be taken for the pick. This misses the design's "constant within 1 ms" for
those notes.

**Velocity** rises with picking strength over five strengths spanning 18 dB, on every string and
with each curve; at 0 % sensitivity every note has velocity 100.

**No stuck notes.** Tests end a note mid-line by stopping, bypassing, resetting, changing the
channel, octave or settings at random, and Panic, in the engine; and by bypass, transport stop and
reset in the built VST3. The port router ends the sounding note when the port changes and when it
closes, through a real virtual port.

**CPU.** About 140 ns per sample at 48 kHz (0.7 % of one core) on an idle development machine,
of which the tracker is 80 ns; `process()` never allocates (tested).

**Where the build departs from this design.**
- The plug-ins' fallback port is either an existing output (an IAC bus) or a virtual port of their
  own, "Sedef MIDI (plug-in)", shared by every instance in a host. They cannot send to the
  standalone app's port: a virtual source is an output only its owner can send to.
- MIDI ports are opened on the message thread. JUCE 9 creates a virtual port that no other app
  can see when this happens on another thread.
- Steady's constant-latency bar holds for clearly dated picks only (above).
- The standalone app sends only through its own port, never through JUCE's MIDI output setting,
  which would send from the audio thread.
