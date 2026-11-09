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

**How notes are made.** The class comment in `GuitarToMidi.h` is the reference. In short: picks come
from the tracker's detector, from the note losing its own period (soft picks over a ringing note,
the same note picked again) and from a lock that comes with a level rise (the tracker locking, or
changing its reading after the level was under the gate), and each is dated to the sample from its
click or the broken period. A picked note goes out once its pitch has settled, its own period is
periodic since the pick, and no lower note can be under it (above A3, that means waiting one period
of the low E). Legato changes must sit on their semitone (12 ms for a change of one semitone, 3 ms
for more); octave corrections need the input to repeat clearly better at the longer period, and a
picked note is taken an octave above what the tracker reads when the input also repeats cleanly at
half that period (a string ringing an octave under it). The same note picked again needs a new
waveform: its periodicity breaks and the input then repeats at its own period again, so pick noise
or a scrape over a sounding note is not a repick. A detector pick replaces a weak pick still waiting
for its level check. A mute breaks the period too: when the level has fallen 10 dB under the note
before a pick seen only that way, and the note heard is one the muted note was a partial of (unison,
octave, twelfth...), it is a string ringing in sympathy, and no note starts. On a string that has
faded 10 dB under its attack, a change of pitch without a pick is the fretting hand moving to the
next note (or lifting off to the open string) before the finger picks it: it ends the note instead
of starting one, and a pick seen there counts only when the level comes back, or rises 3 dB over the
loudest millisecond of the 16 before it (a pick puts energy into the string, a fretting hand does
not).

**Level.** Nothing that decides a note depends on how loud the input is. The levels of the latest 32
picks say how hard this player picks on this input. The velocity scale's top sits 10 dB over the
loud ones (the second loudest; the loudest while there are fewer than four), and its 40 dB span ends
at a floor: once there is a pick to go by, a pick whose attack is under it (a finger touching a
string, the hand on the guitar) starts no note, though it still counts, so a passage played that
much softer comes through after a while. Note-offs come from the level: the note falls 40 dB under
its attack, or under the gate, or a mute is judged against the note's own level from 3 ms after its
pick. The gate (default −75 dB) only has to keep the input's own noise out.

In Steady, a note that would sound under 40 ms while its note-on still waits is dropped, and the
note it interrupted sounds on; a note-on that has gone out is never taken back; a note-off decided
after the next note's pick (a mute, the fretting hand) goes out with that note-on, so one note
sounds at a time. The MIDI layer adds 2 ms to the tracker's lock below A3.

**Accuracy** (synthetic DI, 48 kHz; the tests in engine/tests/midi gate it):

| Material | Notes | Right | Wrong (octave) | Missed | Extra |
|---|---:|---:|---:|---:|---:|
| Bass lines, bridge, neck and piezo pickups, 6 seeds, open strings damped | 864 | 99.4 % | 0 (0) | 5 | 2 |
| A sloppy player's bass lines (below), Fastest | 864 | 99.2 % | 4 (0) | 3 | 8 |
| The same in Steady (the note within 120 ms of its pick) | 864 | 98.4 % | 4 (0) | 10 | 6 |
| Chromatic runs, the fretting hand moving 25-60 ms before each pick (below), both timings | 312 | 100 % | 0 | 0 | 0 |
| A quiet input, every setting at its default (each set with its peak at −30 dBFS): bass lines | 864 | 99.5 % | 0 (0) | 4 | 2 |
| The sloppy lines at −30 dBFS, Fastest / Steady | 864 | 98.8 / 98.0 % | 4 (0) | 6 / 13 | 8 / 6 |
| The fretted-first runs at −30 dBFS, both timings | 312 | 100 % | 0 | 0 | 0 |
| Palm-muted eighths and sixteenths, 120 and 140 bpm, E2 and A2 | 256 | 100 % | 0 | 0 | 0 |
| Hammer-ons, pull-offs, slides | 69 | 100 % | 0 | 0 | 0 |
| Vibrato and slight bends | 8 | 100 % | 0 | 0 | 0 |
| A new note over the previous one ringing | 12 | 100 % | 0 | 0 | 0 |

The sloppy player's lines are the bass lines played the way a guitarist plays them: the previous
note left ringing 6-20 dB under the next (55 % of notes), open strings that share a partial with
the note ringing in sympathy 20-30 dB under it, loud pick noise, picks scraping damped strings
(12 %), and uneven muting (−30 to +60 ms). They gate the rules above: at least 98.5 % right in
Fastest (97.5 % in Steady), at most 2.5 extra notes per 100 (2 in Steady), at most 2 notes
shorter than 40 ms per 100 (1 in Steady), no octave errors. Measured: 10 notes under 40 ms in
Fastest and 3 in Steady (1.2 and 0.3 per 100). Most of their extra notes were strings ringing in
sympathy after a mute (19 to 8 in Fastest, 14 to 6 in Steady, with the sympathy rule above).

The quiet-input rows are the musician's case (his interface records with peaks at −30 dBFS): the
same sets, the noise brought down with them, every setting at its default, against the same bars.
Before the level rules and the −75 dB gate they reached 92.3 % (bass lines), 88.8 % (sloppy) and
46.5 % (fretted first, whose peaks then sat at −40 dBFS), with every velocity between 26 and 52.
A second test plays the bass lines at their usual level and at −30 dBFS: after the first note,
843 of 843 notes get the same velocity within 4 (median 87, 10th to 90th percentile 74-98).

The fretted-first runs model how the musician plays on his own DI (below): chromatic runs up and
down one string, each note fading 14 dB in half a second, the fretting hand reaching the next fret
(or lifting off, so the open string sounds for a moment) 25-60 ms before the finger picks, half the
moves tapping the fret, no pick click, three pickups, four seeds. They gate the fretting-hand rule:
at least 98 % right, at most 2 extra and 2 short notes per 100, no octave errors, and never two
notes sounding at once (the sloppy lines check that too).

**Real piezo DI** (reported, not a gate): fingerstyle chords on a nylon-string piezo, outside the
single-note design; measured below, in [What split the notes](#what-split-the-notes-2026-11-09).

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
Single picks on every string land 37.8 ms after the pick, within the 1 ms bar. On the latency
test's lines, 88.9 % land within 1 ms of the lag, 97.2 % within 3 ms and all within 6 ms (before
the onset search below: 86, 95 and 100 %). By note type, the error from pick + 38 ms:

| Material, note type | Notes | Within 1 ms | p50 | p95 | p99 | Max | > 3 ms early | > 3 ms late |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Bass lines, clean pick | 796 | 88.6 % | 0.2 ms | 2.1 ms | 3.9 ms | 20.1 ms | 10 | 13 |
| Bass lines, soft pick over a ringing note | 63 | 92.1 % | 0.2 | 1.5 | 1.9 | 4.1 | 0 | 1 |
| Sloppy lines, clean pick | 715 | 96.1 % | 0.2 | 0.4 | 20.5 | 25.6 | 17 | 4 |
| Sloppy lines, soft pick over a ringing note | 135 | 82.2 % | 0.2 | 26.3 | 34.5 | 36.0 | 0 | 23 |
| Legato (hammer-ons, pull-offs, slides) | 36 | 22.2 % | 1.8 | 3.6 | 7.2 | 7.2 | 0 | 3 |

The bar was clean picks within 1 ms, the rest p99 within 3 ms, and nothing later than the lag.
**This is a deviation**, for three reasons:
- *Early.* A clean pick right after the previous note's mute or a scrape is dated to that break
  in the period, a few to 26 ms early (10 and 17 notes).
- *Late.* A note that is not sure within the 38 ms goes out when it is sure, not dropped: on the
  sloppy lines a soft pick over a still-ringing string can take up to 74 ms in Fastest, so 20 of
  those 135 notes land 10-36 ms after the lag. A lag that covered them would double every note's
  latency.
- *Legato* is dated where the tracker sees the pitch change, about 2 ms after it.

Tried: once a note's pitch is known (inside the look-ahead), find where its fundamental rises
(complex demodulation at the note's frequency, summed over one period) and date the pick there.
Used directly it made timing worse: one period cannot tell a note from its neighbouring semitone
(that takes about 18 periods), so over a ringing note it found the old note. Kept: the onset only
narrows where to look for the pick's click, and the click dates it, which helps clean picks.

**Velocity** rises with picking strength over five strengths spanning 18 dB, on every string and
with each curve; at 0 % sensitivity every note has velocity 100. It is the same at any input level
(above): the scale follows the player's loud picks. The cost: a long passage played much softer
than what came before drifts up to a usual velocity over about 30 notes, and the very first note
after loading, with no pick to go by yet, uses the scale's default place (top at −10 dB).

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
- Steady's constant-latency bar holds for clearly dated picks only: early after a mute, late for
  a soft pick over a ringing string, and legato about 2 ms after the change (above).
- The sloppy player's lines, harsher than the acceptance material (several strings ringing at
  once, scrapes), now reach 99.2 % right with 0.9 extra notes per 100 in Fastest (the bass lines'
  bars); Steady stays at 98.4 % right.
- The gate is the one absolute level left, and its default moved from −54 to −75 dB so that an
  interface set low (peaks at −30 dBFS, the musician's) needs nothing changed: his softest notes
  are 17 dB over it, and his noise brought up to −6 dBFS peaks stays 7 dB under it. Before the
  first pick of a session, the floor under the player's level does not exist yet, so on a loud
  input handling noise above −75 dB can start one note (two on his chords at −6 dBFS); one wrong
  velocity can follow.
- Under the gate's off point (−83 dB by default) a note ends before it has fallen 40 dB: notes
  quieter than about −43 dB at their attack end 25-40 dB down.
- On fingerstyle playing, a fretting move on a string still within 10 dB of its attack can start
  a short note (5 or 6 of the 78 notes on his DI in Fastest, 3 in Steady), and Steady dates a
  pluck from the finger's touch, up to 26 ms early.
- The standalone app sends only through its own port, never through JUCE's MIDI output setting,
  which would send from the audio thread.

### What split the notes (2026-11-09)

The flicker reported on real DI (short wrong notes between the right ones) was taken apart note by
note with `sedef-midi-render why`, which lists why each note started and ended and how its pick
was seen. On the sloppy lines the extra and short notes came from four rules in the MIDI layer,
not from level note-offs:

| Cause | Layer | Fix |
|---|---|---|
| "The same note picked again" fired on any 2 dB level rise: pick noise and scrapes over a sounding note re-triggered it (most of the extras) | MIDI | A repick needs a new waveform and the input repeating at the note's own period again |
| A string ringing an octave under the played note (left ringing, or in sympathy) made the tracker read the lower octave | tracker, kept by MIDI | The octave-up check (under "How notes are made"), also as a later correction |
| A note was judged muted against the previous, louder string's level | MIDI | The mute is judged against the note's own level |
| A break of periodicity at the previous note's mute dated the next pick | MIDI | A detector pick replaces a weak pick still waiting |

| Sloppy lines, 864 notes | Fastest before | Fastest after | Steady before | Steady after |
|---|---:|---:|---:|---:|
| Right | 97.7 % | 98.8 % | 97.9 % | 98.0 % |
| Octave errors | 6 | 0 | 6 | 0 |
| Extra notes per 100 | 8.7 | 2.3 | 8.4 | 1.7 |
| Notes under 40 ms per 100 | 5.0 | 1.5 | 5.1 | 0.8 |
| Missed | 10 | 6 | 8 | 13 |

Steady misses more because its glitch drop also drops a right note that a wrong correction
replaced within 40 ms. Bridging dropouts was not needed: in the real excerpts no note starts from
silence without a pick. MonoTracker gained only a pick counter that nothing in Trench reads;
Trench renders stay byte-identical (57 renders: every factory preset, and Trench Mono and Poly in
four settings on the synthetic DI and seven real piezo DIs, against main).

**Real piezo DI** (reported, not a gate). The only real DI available is fingerstyle playing on a
nylon-string piezo, with chords and strings ringing into each other, outside the single-note
design. Five excerpts, octave 0, before and after these fixes:

| Real piezo DI, 5 excerpts | Fastest before | Fastest after | Steady before | Steady after |
|---|---:|---:|---:|---:|
| Notes out | 561 | 523 | 560 | 494 |
| Notes under 40 ms | 141 (25.1 %) | 125 (23.9 %) | 134 (23.9 %) | 93 (18.8 %) |
| Hand-checked scale, 8 notes: right within 80 ms of the pick | 6 | 8 | 6 | 8 |
| The scale's 1.2 s: other notes | 10 | 9 | 10 | 8 |

Every split in the scale (D3 E3 F3 G3 A3 B♭3 C4 D4, picked at 0.122, 0.325, 0.452, 0.588, 0.749,
0.876, 1.024 and 1.160 s), after, in Fastest:

| Time (s) | Note out | Length | Cause | Layer |
|---|---|---:|---|---|
| 0.336 | D3 again | 10 ms | E3 is plucked while the tracker still reads D3; the break in D3's period reads as a repick | MIDI, on the tracker's lag |
| 0.346 | D♯3 | 47 ms | The tracker's period blends D3 and E3 (151 to 165 Hz) and is taken as a correction | tracker |
| 0.394 | E3 | right | 69 ms after its pick | |
| 0.568 | D♯3 | 54 ms | A weak pick as F3 is released, with another string ringing | MIDI weak pick, tracker |
| 0.869 | C5 | 42 ms | B♭3's attack partials (506 and 557 Hz) read as the note | tracker |
| 0.911, 0.923 | D♯2, A♯2 | 12, 19 ms | The tracker locks an octave low (116 Hz, the body's resonance); the octave-up check corrects it to B♭3 at 0.942 (before: A♯2 for 108 ms, never corrected) | tracker |
| 1.149-1.232 | C♯3, D4, D3, D4 | 15, 15, 53 ms, then right | The change from C4 to D4 reads an octave low (139-147 Hz) twice before it settles | tracker |

What is left is a mono tracker reading two strings at once. Of the 124 notes under 40 ms across the
excerpts, 107 end because another note replaces them (not the level); 48 are a semitone from a
neighbouring note (two adjacent notes sounding together), 20 an octave or a twelfth from one (the
body resonance near 100 Hz pulls the lock an octave low), 13 go back to the note before. Neither
the MIDI layer nor the tracker can tell which of two strings at similar levels is the note; where
the new note is 6-20 dB above the ringing one, as in the sloppy lines, 98.8 % come out right. This
is inherent to fingerstyle chords on a piezo, and real single-note bass lines are untested until
the musician plays them.

### The musician's own DI (2026-11-09)

Reported, not a gate; the recordings stay off the repository. Two dry DIs from the musician's own
interface, 44.1 kHz: a chromatic exercise (59.7 s, 78 notes: six runs up one string each, open
string to the twelfth fret, strings tuned a whole step down, D G C F A D) and 13 strummed chords
(29.1 s). He plays with his fingers: no pick click (the attack above 2 kHz is about 45 dB under
the note), each string fades about 14 dB in half a second, and the fretting hand reaches the next
fret, or lifts off so the open string sounds, 25-60 ms before the finger plucks. The level is low:
peaks at −30.6 dBFS (chords −27.1), RMS −55 dBFS, noise floor −101 dBFS.

The ground truth was made offline: each onset where the input stops repeating at the ringing
note's period (refined from the level rise), each pitch from a long-window period estimate checked
against a harmonic sum, and every run's string and direction checked on the waveform. A note is
right when one of its pitch starts within 80 ms before and 150 ms after its onset (plus the lag in
Steady).

**At its own level the defaults lose the quietest string.** The gate (−54 dB) misses 11 of the 13
notes on the top string and three more, and the velocity scale is absolute (−50 to −10 dB of
picking level), so velocities sit at the bottom: 26-49 (median 33), with 21 % of notes at the
floor of 26 that 75 % sensitivity leaves. Brought to −6 dBFS peaks (+24.6 dB) the same playing
gives 26-108 (median 88, 10th to 90th percentile 69-98) and every note over the gate; the gate
dragged to −66 dB at the native level gets every note too. Lowering the default instead was
tried and rejected: at −60 or −66 dB the sloppy lines exceed their gate (23 extra notes in
Fastest, 2.7 per 100; 18 in Steady, 2.1). That was the first round's answer (raise the
interface's gain); the [second round](#his-level-is-the-operating-level-2026-11-09) makes his level
work as it is.

**What was wrong in the MIDI layer.** Once the notes were over the gate, almost every note came
out twice: the fretting hand moved on while the string still rang, the ringing string took the
new pitch with no energy added, the move broke the note's periodicity (a weak pick) or tapped the
fret for the detector, and the note started early and quiet; the finger's pluck then started it
again. Fixed by the fretting-hand rule under "How notes are made", gated by the fretted-first
runs (before the fix: 96.5 % right and 37 extra notes in 312). A second fault showed in Steady:
a note-off decided after the next note's pick (the mute, the fretting hand) went out up to 28 ms
after the next note-on, two notes at once; such a note-off now goes out with that note-on, and
the sloppy and fretted-first gates check that one note sounds at a time. Neither change touches
the tracker: Trench renders stay byte-identical to main (105 renders: the 57 above and this DI
in Mono and Poly, two settings, alone and through a clean and a high-gain preset).

| Chromatic DI, 78 notes | Right | Wrong | Missed | Extra per 100 (before) | Under 40 ms per 100 (before) |
|---|---:|---:|---:|---:|---:|
| Native level, defaults, Fastest | 80.8 % | 0 | 15 | 3.8 (11.5) | 2.6 (10.3) |
| Native level, defaults, Steady | 82.1 % | 0 | 14 | 1.3 (5.1) | 0 (0) |
| Native level, gate −66 dB, Fastest | 98.7 % | 0 | 1 | 9.0 (51.3) | 5.1 (19.2) |
| Native level, gate −66 dB, Steady | 100 % | 0 | 0 | 5.1 (39.7) | 1.3 (6.4) |
| Peaks at −6 dBFS, defaults, Fastest | 98.7 % | 0 | 1 | 10.3 (64.1) | 5.1 (20.5) |
| Peaks at −6 dBFS, defaults, Steady | 98.7 % | 1 (no octave) | 0 | 3.8 (52.6) | 0 (7.7) |

No octave errors anywhere. Lock latency, pluck to note-on, Fastest, gate −66 dB (the same at
−6 dBFS):

| Run (string) | D2 | G2 | C3 | F3 | A3 | D4 |
|---|---:|---:|---:|---:|---:|---:|
| Median | 21.1 ms | 18.0 ms | 15.8 ms | 13.3 ms | 15.9 ms | 15.5 ms |
| Max | 26.0 | 22.3 | 33.1 | 24.1 | 32.7 | 33.4 |

Steady, error from the pluck + 38 ms (right notes):

| Chromatic DI | p50 | p95 | p99 | Max | Within 1 ms | > 3 ms early | > 3 ms late |
|---|---:|---:|---:|---:|---:|---:|---:|
| Native, defaults | 0.3 ms | 7.2 ms (before 12.2) | 59.0 ms | 133.6 ms | 79.7 % | 7 | 2 |
| Native, gate −66 dB | 0.4 | 8.9 (37.2) | 48.4 (66.2) | 124.6 | 71.8 % (61.5) | 12 | 1 |
| Peaks at −6 dBFS | 0.3 | 11.1 (43.0) | 48.7 (76.8) | 121.6 | 72.7 % (55.8) | 12 | 1 |

Every error left, from `sedef-midi-render why` (gate −66 dB and −6 dBFS peaks; Fastest unless
said):

| Time (s) | Played | Out | Cause | Layer |
|---|---|---|---|---|
| 1.283 | D2, the first note | D2 160-172 ms late (Steady: 122-134 ms after the lag) | The first note from silence: the tracker locks on the finger's touch (around 830 Hz), then gives no pitch for about 100 ms | tracker |
| 10.901 | G2, first of run 2 | −66 dB: G2 for 36 ms, then G2 again 115 ms on; −6 dBFS: F♯3 for 59 ms, then G2 for 10 ms (Steady: the F♯3 only, the one wrong note) | The same, after a 2 s rest | tracker |
| 14.491 | C♯3 | C♯3 for 53 ms from 34 ms before the pluck, then the note | The fretting hand on a string faded less than 10 dB: the move is a weak pick and passes its level check | MIDI |
| 24.388 | F♯3 | F♯3 for 61 ms from 46 ms before (legato) | The same, as a change of pitch | MIDI |
| 41.490 | C4 | C4 for 22-43 ms from 31 ms before | The same, weak pick | MIDI |
| 49.522 | D♯4 | D♯4 for 8 ms from 51 ms before (−6 dBFS only) | The same, ended by its mute | MIDI |
| 52.648 | A4 | A4 for 6-17 ms from 41 ms before | The same, legato | MIDI |
| 53.702 | B4 | A4 for 33 ms from 80 ms before | The hand shifting position: lifting the finger off A♯4 leaves A4 fretted for 80 ms, a pull-off without a pick | MIDI; the signal is a real pull-off |
| (12 notes in each) | | Steady, 3-26 ms early | The finger touching the string before it plucks damps the old note and breaks its period, and that is dated as the pick (E2 −25.7 and A2 −16.8 ms, at −6 dBFS also C3 −19.8 ms; the rest 3-10 ms) | MIDI, by design |

The residual fretting errors are moves on strings that have faded less than 10 dB, mostly on
the higher strings, where the next note comes sooner; on the signal they look like a legato
change or a soft pick, and a lower threshold would cut real hammer-ons. In Steady the glitch drop
removes those under 40 ms, and three remain.

**Chords** (robustness only; chords are outside the mono design): no stuck notes, never two notes
at once, the last note-off 2.3 s before the end of the file. Notes out per strum, 13 strums:

| Chords DI | Notes out | Per strum (min / median / max) | Under 40 ms | Longest |
|---|---:|---|---:|---:|
| Native level, Fastest | 33 | 1 / 3 / 4 | 4 | 987 ms |
| Native level, Steady | 31 | 1 / 2 / 3 | 1 | 991 ms |
| Peaks at −6 dBFS, Fastest | 44 | 2 / 3 / 5 | 11 | 1126 ms |
| Peaks at −6 dBFS, Steady | 41 | 2 / 3 / 4 | 6 | 1130 ms |

Each strum gives a few notes as the tracker moves between the ringing strings, mostly the low one
first, and settles on one of them; before the fixes each run gave one note more or one fewer.

### His level is the operating level (2026-11-09)

There are no more recordings, and his interface stays as it is: peaks at −30 dBFS are where Sedef
MIDI has to work, with every setting at its default. Three things depended on the input's level:

| Before | Now | Why this value |
|---|---|---|
| Gate default −54 dB: his top string (attacks at −52 to −58 dB) under it | −75 dB, still the player's control | His softest attack is 17 dB over it; his noise floor (the silence after the runs, at most −107 dB) brought to −6 dBFS peaks stays 7 dB under it |
| Velocity: picks from −50 to −10 dB map to 1..127 | The scale's top 10 dB over the player's loud picks (the second loudest of the latest 32) | The same playing gets the same velocities at any level; 10 dB puts the bass lines' median where the old scale did at a usual level (87 against 84) |
| Note-off under the gate's off point (−62 dB): his notes ended after 15 dB of decay | Also 40 dB under the note's attack | The old off point sat 32-47 dB under the attacks of a usual level |

Lowering the gate showed what it had hid at a usual level. Each was reproduced on the synthetic
sets with the gate at −66 to −78 dB before it was fixed:
- A mute breaks the period too. When the mute takes longer than the weak pick's 12 ms level
  check, a string ringing in sympathy with the muted note (20-30 dB under it) started a note from
  that break, with the muted note's velocity: 4 of the 23 extra notes on the sloppy lines at
  −66 dB. The sympathy rule (under "How notes are made") removed them, and 11 more at the old
  gate.
- Handling noise and a finger touching a string, 40-60 dB under the playing, start notes on a
  loud input (on his DI brought to −6 dBFS: F♯4 for 243 ms and D4 for 49 ms, velocity 26, before
  the C3 and A3 runs). The floor under the player's level stops them.
- With the floor in place, two kinds of real notes measured under it, because their pick was
  dated wrong. After a mute, the tracker can miss the next pick and hold a reading through the
  silence: the note started on the mute's stale break, 84 ms earlier, with velocity 26. The
  tracker changing its reading after the level was under the gate now counts as a pick. On his
  DI the detector dates the first pluck of a run 41 ms early (the finger touching the string):
  the attack of a note from silence is now the loudest millisecond from its pick until it
  starts.

| Chromatic DI, 78 notes, defaults | Right | Wrong | Missed | Extra per 100 | Under 40 ms per 100 | Velocity p10 / median / p90 |
|---|---:|---:|---:|---:|---:|---:|
| His level (−30.6 dBFS peaks), Fastest | 98.7 % (80.8) | 0 | 1 (15) | 10.3 | 5.1 | 72 / 82 / 94 (26 / 33 / 40) |
| His level, Steady | 100 % (82.1) | 0 | 0 (14) | 5.1 | 0 | 72 / 82 / 94 |
| Peaks at −6 dBFS, Fastest | 98.7 % | 0 | 1 | 11.5 | 5.1 | 72 / 83 / 94 |
| Peaks at −6 dBFS, Steady | 98.7 % | 1 | 0 | 5.1 | 0 | 72 / 83 / 94 |

In brackets, before this round at his level. Both levels now give nearly the same notes; the extra
one at −6 dBFS is handling noise before the first pick (D4 at 1.03 s, 129 ms), when there is no
level to go by yet. The other errors are those in the table above: the tracker's first note from
silence, the G2 after a rest, and fretting moves within 10 dB of the attack. Steady at his level:
p50 0.4 ms, p95 10.8, p99 48.4, max 124.6, 71.8 % within 1 ms. Right notes now last 0.92 (Fastest)
to 0.96 (Steady) of the time to the next pluck, as at −6 dBFS (0.76 before, at his level).

The chords, at his level: 42 notes in Fastest (2 to 6 per strum, median 3, 9 under 40 ms), 39 in
Steady (2 to 5, 4 under 40 ms); at −6 dBFS 49 and 46, with 2 before the first strum. The old gate
cut them to 33 and 31. Never two notes at once, no stuck note. The tracker is untouched: Trench
renders stay byte-identical to main (the same 105 renders).

**Left open:** the tracker's first note from silence (late by about 160 ms, or split), which needs
a tracker change that Trench would hear; fretting moves on strings still within 10 dB of their
attack; before the first pick of a session there is no player's level yet, so on a loud input
handling noise over the gate can start one note and the first note's velocity uses the scale's
default place; and a long passage played much softer than what came before drifts up to a usual
velocity over about 30 notes.
