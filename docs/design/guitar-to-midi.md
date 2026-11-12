# Guitar to MIDI: play bass lines on a guitar (design)

Status: approved 2026-11-08; built in Phase 1 (engine layer, plug-ins, standalone app, test build);
shipped in Phase 2 with Sedef Amp's installers from 0.6.0, its identity frozen
([docs/midi.md](../midi.md#identity)).
What was measured, and where the build differs from this design, is in [As built](#as-built)
at the end, round by round; the latest is
[Sooner than two periods](#sooner-than-two-periods-2026-11-12). The user page is
[docs/midi.md](../midi.md).

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
Installers, the updater and the final AU codes were Phase 2: Sedef MIDI ships in Sedef Amp's
installers from 0.6.0, without waiting for the musician's test, with the codes and AU types of the
test build frozen (`aufx` `Sdm1` and `aumu` `Sdm2`, manufacturer `Sedf`).

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

**Level.** Nothing that decides a note depends on how loud the input is: every level a decision
compares is the input's own noise floor, the note's own level, or the player's. The gate is Auto by
default, 20 dB over the noise floor Sedef MIDI tracks (Manual keeps a fixed gate). The player's
level is the average attack, in dB, of about the latest 16 notes; the velocity scale's top sits
13.5 dB over it, so a note at the player's level gets velocity 88 at the defaults, and its 40 dB
span ends at a floor 26.5 dB under the player's level: once there is a note to go by, a pick whose
attack is under it (the hand on the guitar, a string ringing in sympathy) starts no note, though it
still counts, so a passage played that much softer comes through after a while. Before the first
note, a note must stand 30 dB out of the noise floor. Note-offs come from the level: the note falls
40 dB under its attack, or under the gate, or a mute is judged against the note's own level from
3 ms after its pick. See
[Any level, fingers, and the first note](#any-level-fingers-and-the-first-note-from-silence-2026-11-09).

In Steady, a note that would sound under 40 ms while its note-on still waits is dropped, and the
note it interrupted sounds on; a note-on that has gone out is never taken back; a note-off decided
after the next note's pick (a mute, the fretting hand) goes out with that note-on, so one note
sounds at a time. The MIDI layer adds 2 ms to the tracker's lock below A3.

**Accuracy** (synthetic DI, 48 kHz; the tests in engine/tests/midi gate it):

| Material | Notes | Right | Wrong (octave) | Missed | Extra |
|---|---:|---:|---:|---:|---:|
| Bass lines, bridge, neck and piezo pickups, 6 seeds, open strings damped | 864 | 99.4 % | 0 (0) | 5 | 1 |
| A sloppy player's bass lines (below), Fastest | 864 | 99.2 % | 4 (0) | 3 | 9 |
| The same in Steady (the note within 120 ms of its pick) | 864 | 98.4 % | 4 (0) | 10 | 7 |
| Chromatic runs, the fretting hand moving 25-60 ms before each pick (below), both timings | 312 | 100 % | 0 | 0 | 0 |
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

The bass lines, the sloppy lines, the fretted-first runs, the palm mutes, 45 of the legato notes
and the strummed chords are also measured with every setting at its default and each set's peaks
at −40, −30, −18 and −6 dBFS, the noise brought down with it, against the same bars at each level:
the results are the same at all four
([below](#any-level-fingers-and-the-first-note-from-silence-2026-11-09)). The first round's quiet-input
rows (peaks at −30 dBFS, before the level rules) reached 92.3 % (bass lines), 88.8 % (sloppy) and
46.5 % (fretted first), with every velocity between 26 and 52.

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
25 ms in); the MIDI layer follows 2 ms later. (Fastest has since become quicker: most notes go out
on an early reading, before the tracker locks. See
[Sooner than two periods](#sooner-than-two-periods-2026-11-12).)

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

### Any level, fingers, and the first note from silence (2026-11-09)

His interface stays as it is and he plays with his fingers, so no default may depend on the
input's level or on a pick's click; and a bass player starts nearly every phrase from silence, so
the first note after a rest must come as quickly as any other.

**Level-independent defaults.** Every level a decision compares is now the input's own noise
floor, the note's own level, or the player's:

| What | Before | Now |
|---|---|---|
| Gate | −75 dB, fixed | **Auto** (default): 20 dB over the input's noise floor, never under −100 dB. The floor is the quietest the level has been (the loudest 3 ms level of each millisecond): it falls at once, rises 6 dB/s while no note sounds, and is the level itself for the first 50 ms. **Manual** keeps the fixed gate: the new MIDI parameter Gate Mode (`gateMode`, Auto or Manual), appended after Audio |
| The first note | over the gate | also 30 dB out of the noise floor (the hand on the guitar, a string ringing in sympathy and handling noise stay under it) |
| A note after it | over the gate and the velocity scale's floor | the same; the floor sits 26.5 dB under the player's level |
| The player's level | the second loudest of the latest 32 picks | the average attack, in dB, of about the latest 16 notes (each new one weighs 1/16): slow enough to keep the dynamics of a phrase |
| Velocity | the scale's top 10 dB over the loud picks | the top 13.5 dB over the player's level: a note at the player's level gets 88 at the defaults, and the first note is its own player's level |
| The tracker's detector | picks over −70 dBFS | picks from 6 dB under the gate in effect |

The UI puts Auto and Manual under the input meter; in Auto the meter draws the gate in effect, sent
with the telemetry, as a dashed line that takes no input.

**Why the first note from silence was late** (his DI at his level, measured before any change).
D2 at 1.283 s, the first note, came 160-172 ms late; G2 at 10.901 s, after a 2 s rest, sounded for
36 ms and again 115 ms on.
- His low strings are plucked near the bridge and heard by a bridge pickup: for the first 100 ms
  or so the fundamental carries only −30 to −40 dB of the energy at the note's period. The tracker
  takes a period whose fundamental carries under −30 dB (a share of 0.001) for a virtual
  fundamental (two notes repeating together at a period neither has, as an open fifth does) and
  rejects it: D2 waited until its fundamental grew, and G2's held-note check dropped it as its
  share dipped, until the next lock started it again.
- A finger touches the string 10-50 ms before it plucks it, 20-22 dB under the pluck on his DI. The
  detector took the touch for the pick, so the pluck fell in its 25 ms refractory time and was
  none, and the tracker first locked on the touch's squeak (about 830 Hz, a high harmonic of the
  string).
- The attack of a low string plucked that way is pulse-like: the 2 ms power swings 10-20 dB with
  every period while the 40 ms power catches up, so the detector fired again every 25-34 ms, and
  each pick restarted the two periods the tracker waits before a pitch counts as settled.

**The fix**: two MonoTracker options, off by default. Sedef MIDI turns them on; Trench does not
set them, and renders byte-identical.
- `weakFundamentals`. A virtual fundamental has nothing at its 1st and 7th harmonics (shares near
  10⁻⁵ on two-note signals); his low notes keep 0.10-0.19 of their energy at the 7th. A period
  whose fundamental carries at least 3 × 10⁻⁵ counts when its 7th harmonic carries at least 0.02
  (notes up to about 210 Hz, a seventh of the anti-alias band). A period with under 1 % at its
  fundamental is judged 2.5 ms later, once the attack no longer leaks into it: without that, the
  attack of an open fifth named its virtual fundamental for a moment.
- `fingerPicks`. For 60 ms after a pick, only a pick 6 dB over that pick's peak counts, even in
  the refractory time, and it is dated from where its climb began: the pluck after a touch is a
  pick, the swings of a pulse-like attack are not.

In the MIDI layer: a detector pick replaces a pick that started no note; from silence, a pick
10 dB under the player's level, or the first pick of all, waits 60 ms for a louder one before it
starts a note; and no note from silence starts while a louder attack is under way (the touch's
squeak just as the pluck begins). The price is the very first note after Sedef MIDI starts, about
60 ms after its pluck, since nothing says yet how loud the player is; a note from silence after
that is not held. Letting a pick that replaced a touch 10 dB quieter skip the wait was tried and
dropped: when a touch's contact and its squeak were two picks, it started the squeak (8 extra
notes on one set).

**The suite at any level** (every setting at its default; the same at peaks of −40, −30, −18 and
−6 dBFS):

| Set | Notes | Right | Wrong (octave) | Missed | Extra | Bar at each level |
|---|---:|---:|---:|---:|---:|---|
| Bass lines | 864 | 99.42 % | 0 (0) | 5 | 1 | ≥ 99 %, ≤ 1 extra per 100 |
| Sloppy lines, Fastest | 864 | 99.19 % | 4 (0) | 3 | 9 | ≥ 98.5 %, ≤ 2.5 per 100 |
| Sloppy lines, Steady | 864 | 98.38 % | 4 (0) | 10 | 7 | ≥ 97.5 %, ≤ 2 per 100 |
| Fretted first, Fastest and Steady | 312 | 100 % | 0 | 0 | 0 | ≥ 98 %, ≤ 2 per 100 |
| Palm mutes | 256 | 100 % | 0 | 0 | 0 | ≥ 99 %, ≤ 1 per 100 |
| Legato | 45 | 100 % | 0 | 0 | 0 | ≥ 99 %, ≤ 1 per 100 |
| Strummed chords, both timings | 144 chords | 98 notes | | | none outside a chord | ≤ 4 per chord (3 at most), one at a time |

Velocity on the bass lines at each level: 10th percentile 78, median 88, 90th 99, and 3440 of 3440
notes within 4 of their velocity at the recorded level. Digital silence, 50 and 60 Hz hum and buzz
at −70 dBFS, and hiss at −100, −90, −80, −70, −60 and −50 dBFS give no note, in either timing; bass
lines over a hum at −80 or −60 dBFS are all right, with nothing extra and nothing after the last
note.

**Notes from silence** (the gate for the fix): fingerstyle notes after rests of 0.5-3 s, each with
a finger's touch 10-50 ms before the pluck and no pick click, on every string of three tunings
(standard, a whole step down, drop D), open and at frets 2 and 5, on a bridge pickup that hears
little of the low strings' fundamental and on a neck pickup: 54 of 54 right in each of the six
sets, none extra. Each string's median latency from silence (the first note of a run aside)
against the same notes picked over a ringing string, Fastest:

| String (low to high) | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---:|---:|---:|---:|---:|---:|
| Standard, bridge | 28.6 / 28.3 ms | 23.3 / 22.9 | 19.4 / 18.8 | 22.1 / 21.3 | 16.3 / 16.4 | 16.5 / 16.9 |
| Standard, neck | 26.6 / 25.8 | 20.3 / 20.5 | 16.7 / 16.4 | 16.5 / 16.2 | 16.3 / 16.3 | 16.3 / 16.5 |
| Whole step down, bridge | 31.7 / 31.1 | 25.0 / 24.9 | 21.2 / 20.3 | 16.3 / 20.4 | 16.6 / 16.3 | 16.4 / 16.6 |
| Whole step down, neck | 29.2 / 28.6 | 23.3 / 22.3 | 18.2 / 18.0 | 16.4 / 16.3 | 16.4 / 16.2 | 16.3 / 16.4 |
| Drop D, bridge | 31.4 / 30.9 | 23.1 / 23.0 | 19.1 / 18.9 | 17.0 / 17.2 | 16.4 / 16.4 | 16.3 / 16.8 |
| Drop D, neck | 28.8 / 28.4 | 20.7 / 20.5 | 18.1 / 16.3 | 16.8 / 16.4 | 16.3 / 16.3 | 16.2 / 16.3 |

From silence minus from a ringing string: −4.1 to +1.8 ms (the gate is +3 ms). The first note of
each set comes within 65 ms of its pluck (the 60 ms wait). Notes from a ringing string keep
their latency: pick to note-on, Fastest, at 48 kHz, E2 27.5 ms, A2 21.5, D3 17.0, G3 and up 15.9,
the tracker's lock 25.5, 19.5, 15.0, 11.5, 9.5, 8.5 and 8.5 ms, all as before. The tracker's own
tests: with the options a touch 12, 25 or 45 ms ahead gives one pick within 3 ms of the pluck and
none in the attack after it; D2, E2, G2 and A2 plucked near the bridge are named at once; and an
open fifth over a ringing neighbour still gives no virtual fundamental.

**His DI, at his level, every setting at its default** (reported, not a gate; in brackets the
same with the gate dragged to −66 dB in the round before, the target):

| Chromatic DI, 78 notes | Right | Wrong | Missed | Extra per 100 | Under 40 ms per 100 | Velocity p10 / median / p90 |
|---|---:|---:|---:|---:|---:|---:|
| Fastest | 100 % (98.7) | 0 | 0 (1) | 7.7 (9.0) | 3.8 (5.1) | 78 / 87 / 93 |
| Steady | 100 % (100) | 0 | 0 (0) | 3.8 (5.1) | 0 (1.3) | 78 / 87 / 93 |
| Peaks at −6 dBFS, Fastest | 100 % | 0 | 0 | 7.7 | 3.8 | 79 / 88 / 94 |
| Peaks at −6 dBFS, Steady | 100 % | 0 | 0 | 3.8 | 0 | 79 / 88 / 94 |

- D2 at 1.283 s, the first note: one note, 49.6 ms after the onset in Fastest (was 160-172 ms);
  in Steady 11.6 ms after the lag (was 122-134 ms). It is the first note of all, held by the
  60 ms wait.
- G2 at 10.901 s, after the 2 s rest: one note, 27.1 ms after the onset (was 36 ms of G2, then G2
  again 115 ms on).
- The other runs from silence now start on the pluck rather than the touch: C3 +1.0, F3 +4.0 and
  D4 +6.0 ms (A3 the same), each now within a few ms of its string's notes from a ringing string
  (D4 14.7 ms against a run median of 14.4).
- Notes from a ringing string: 64 of 72 have the latency they had, to 0.1 ms; A2 +1.0, G♯2 +2.0,
  F♯3 +3.0, A♯2 and G3 −1.0, E3 −2.0 ms. E2 at 2.530 s now starts at its pluck (25.3 ms) instead
  of 0.7 ms before it, on the fretting hand's move, and D♯4 at 49.522 s now has an 8 ms note on
  the move before it (counted below). Run medians, D2 to D4: 22.4, 18.0, 16.2, 13.6, 15.9, 14.5 ms
  (before: 21.1, 17.5, 15.8, 13.3, 15.9, 14.5).
- Steady: p50 0.4 ms, p95 7.5, p99 12.6, max 15.8; 71.8 % within 1 ms; 11 notes more than 3 ms
  early (the finger touching the ringing string damps it and breaks its period, which dates the
  pick: A2 −15.8 ms, E3 −9.7, F♯4 −8.8, the rest 3-7 ms) and one more than 3 ms late (the first
  note).
- The extra notes are fretting moves on strings within 10 dB of their attack, as in the round
  before: C♯3, F♯3 and C4 (both timings), D♯4, A4, and A4 before B4 (Fastest only).

The chords, at his level (the same at −6 dBFS): 28 notes for 13 strums in either timing (1 to 4 per
strum, median 2; one under 40 ms), none before the first strum, never two at once, no stuck note
(before: 42 and 39 notes, 2 to 6 and 2 to 5 per strum).

**Left open:** the first note after Sedef MIDI starts (60 ms, once per session); fretting moves on
strings still within 10 dB of their attack; and in Steady, a note whose ringing string the finger
touches before plucking lands up to 16 ms early.

### The first session: a held note, low strings, bends and the delay (2026-11-11)

The musician played 0.6.0's standalone app into a bass instrument and reported, in his order of
importance: too much delay; no note under a low B; a held note wavers in pitch, like a chorus;
bends only half work and slides don't. He had sent a screen recording with the instrument's
sound. In it Sedef MIDI was on Notes + Bend, octave 0, Fastest (read from which segments are
lit: the labels are too small to read). His recordings stay off the repository.

#### A held note

**What the recording shows.** The instrument's pitch on ten held notes (31 to 83 Hz, the
instantaneous frequency of their first four partials): a modulation of 3.65 to 3.66 Hz, ±12 to
14 cents, the same rate at every pitch and the same depth on every partial (a pitch modulation,
not two detuned voices), undamped for three seconds, and starting again with each note (its
first peak 70 to 140 ms after the note starts, on about 40 notes). Every note also sits 15 to
35 cents sharp, and a few start in tune for 50 to 90 ms and then jump there.

**What Sedef MIDI sent.** Notes + Bend sent the tracked pitch's distance from the equal-tempered
note, every 2 ms, whenever it moved 1.5 cents. On his chromatic DI that is 9629 pitch bends for
84 notes, 194 a second: each note starts with its own offset (median 7 cents, up to 38: tuning
and fretting), drifts as it decays, steps by more than 5 cents six times a note (the tracker
changes the source of its reading once a note holds still), and swings 50 to 75 cents at its end
when the fretting hand moves on. On his three long notes (2.0 to 2.6 s) the bend, after the
first 100 ms, has a standard deviation of 1.7 to 3.9 cents and nothing near 3.66 Hz; a perfectly
steady synthetic string gets one step of 6 to 10 cents about 100 ms in. Short notes started
again inside a held note, the other suspect, do not occur: Notes mode gives 84 notes for 78, the
five short ones all a fretting move before a pluck.

**The cause.** A rate that ignores the note and restarts with each note is an oscillator in the
instrument, not anything in the pitch stream (nothing in Sedef MIDI has a 273 ms clock). What
fits all of it is an instrument that takes a pitch wheel off centre as a played bend and adds a
bend and vibrato of its own; Sedef MIDI never left the wheel centred. That cannot be proved
without the instrument, and does not need to be: a held note has no business sending pitch
bend, and in Notes mode, the default, none was ever sent.

**The fix** (the class comment in `GuitarToMidi.h` has the rules). A centred bend goes out with
every note-on, and the wheel stays at exactly the centre while a note is only held. The bend is
counted from where the note sits, not from equal temperament: the note's centre is its reading
over its first 30 ms, then follows the string's slow drift at 20 cents a second, within 35 cents
of the semitone. A bend starts when the pitch has been 25 cents from the centre for 40 ms (or 60
cents, followed all the way), then follows the string at up to 3000 cents a second, ignoring
moves of the reading under 3 cents; it ends, exactly centred, back within 10 cents for 60 ms or
at rest within a fret's tuning of the semitone. The pitch the bend follows is the tracker's
estimator reading (`MonoTracker::followedHz()`), which has no step when a note holds still.

Why 25 cents: on his chromatic DI, 74 held notes, the pitch is within 1.6 cents of the note's
centre half of the time, 5.8 cents 90 % and 16.4 cents 99 % of it; 4 % of the notes pass 25
cents in their first 100 ms and 4 % while held, all of them fretting moves, which the 40 ms and
the falling level rule out. The drift from the start of a hold to its end is −2.3 cents in the
median and −10.9 at the 10th percentile, which is what the moving centre is for.

| Notes + Bend, pitch bends off centre | 0.7.0 | Now |
|---|---:|---:|
| His chromatic DI (84 notes) | 9629 | 0 |
| The same, a fourth and a minor sixth lower | | 0 and 6 |
| His strummed chords (28 notes) | 829 | 0 |
| Seven piezo DIs, fingerstyle chords (about 400 notes) | 13 169 | 355 |
| Held synthetic strings, 3 s: drift of 0 and −5 cents/s, tuned −12 and +18 cents, four pitches, three pickups | every one | 0 |

In Notes mode his chromatic DI gives the same events as 0.7.0, byte for byte.

#### Bends and slides

What each does, and why slides are not pitch bend:

| | Notes | Notes + Bend |
|---|---|---|
| Held note, narrow vibrato (±15 cents) | note-on, note-off | the same, and a centred bend before the note-on |
| String bend inside the bend range | the notes it passes | one note; the bend follows, and returns to exactly the centre |
| String bend past the range | the notes it passes | the next note starts, without a gap |
| Wide vibrato (±50 cents) | one note | followed |
| Hammer-on, pull-off | a note | a note |
| Slide | its frets: the first reached, then only frets held 30 ms, so a fast slide is two notes and a slow one every fret | the same |

Measured on synthetic strings (E2, A2, G3): bends of a semitone in 80 to 600 ms and of a whole
step in 150 to 600 ms, held and released, are one note each; what sounds is within 27 cents of
the string (allowing 60 ms), 5.8 cents rms at worst; no message moves the bend more than 6
cents; a held bend is exact to 3 cents and a released one ends at exactly 0. A whole step in
80 ms moves too fast to tell from a fret taken at once and comes out as notes. Slides over 2, 5
and 12 frets at 15 to 120 ms a fret: never two note-ons closer than 25 ms after the first fret,
the last note always the fret the slide stopped on, no gap.

A fret taken at once is a note in both modes. Turning it into a bend was built and measured: on
the bass lines 13 % of the notes (later 3 %, with every cue that could be found: the level's dip
before a pluck, the pick's click, the level's continuity) lost their attack, because a softly
picked note on the same string looks the same. That is worse than a slide played as notes.

#### Down to F♯1

"A low B gives B, a low A gives nothing" was one constant: `kMinNoteHz`, 60 Hz. Under it, the
tracker looked no lower than 50 Hz, and the waits that rule out a lower note assumed a
six-string.

`Settings::lowNote` (the parameter Lowest Note, appended) has three ranges and Automatic:

| Range | Lowest string | Plays down to | The tracker looks down to | Its rumble filter |
|---|---|---|---|---|
| E2 | 82.4 Hz | 60 Hz | 50 Hz | 40 Hz |
| B1 | 61.7 Hz | 50 Hz | 46 Hz | 30 Hz |
| F♯1 | 46.2 Hz | 39.5 Hz | 38 Hz | 22 Hz |

The E2 range is 0.7.0, exactly. A lower range costs the notes above something, so it is not
always on: a first reading can name any partial of a low string, so a note above the lowest
string's octave must wait one period of that string, unless it alone accounts for the signal
since the pick (99 % of it repeats at the note's period), which a cleanly played note does.
Three more things were needed, each found on the lowered sets below: a note above the lowest
string's octave must account for the signal since the pick, not only for its own last period (a
low string picked again made the tracker read one partial after another for 45 ms); the longer
period is compared with the note's own on the residual each leaves, the note's period taken
exactly (a low string whose 4th partial is its strongest repeats 97 % at that partial's
period); and a low string's 3 ms level swings 10 to 20 dB with every period, which read as a
mute (his lowered DI: a low F♯ of one second came out as two notes of 14 ms), so a string under
the low E is as loud as the loudest millisecond of its last period.

**Automatic** (the default) is the E2 range, with the tracker looking down to 38 Hz, until a note
under the range has sounded for 150 ms; then it is the B1 or the F♯1 range, kept across
`reset()` and `prepare()` and saved with a host's session. Before it has learned, a note under
60 Hz needs a third period (two strings ringing together repeat at such a period for a moment:
two notes of 15 ms on the sloppy lines without it). On a six-string it sends the notes the E2
range sends, 493 of 494 within 3 ms of where that range puts them, and never widens, on the
lines, the sloppy lines, the fretted-first runs and the strummed chords.

There is no DI of his low strings, so the material is the suite's lines on strings a fourth lower
(B1, E2, A2 where a guitar has E2, A2, D3) and a minor seventh lower (F♯1, B1, E2), and his own
chromatic DI slowed down by a fourth and by a minor sixth (its lowest run then starts on A1 and
on F♯1), against the six-string bars:

| Set | Range | Notes | Right | Wrong (octave) | Missed | Extra | Under 40 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| Bass lines, a fourth lower | B1 | 864 | 100 % | 0 | 0 | 1 | 1 |
| Sloppy lines, a fourth lower | B1 | 864 | 98.84 % | 3 (0) | 7 | 11 | 11 |
| Fretted first, a fourth lower | B1 | 312 | 100 % | 0 | 0 | 0 | 0 |
| From silence, a fourth lower | B1 | 108 | 98.15 % | 0 | 2 | 0 | 0 |
| Bass lines, a seventh lower | F♯1 | 864 | 99.88 % | 0 | 1 | 0 | 0 |
| Sloppy lines, a seventh lower | F♯1 | 864 | 98.61 % | 4 (0) | 8 | 17 | 12 |
| Fretted first, a seventh lower | F♯1 | 312 | 100 % | 0 | 0 | 0 | 0 |
| From silence, a seventh lower | F♯1 | 108 | 100 % | 0 | 0 | 0 | 0 |
| Bass lines, a seventh lower | Automatic, from a six-string each time | 864 | 99.77 % | 0 | 2 | 3 | 2 |
| Sloppy lines, a seventh lower | Automatic, the same | 864 | 98.50 % | 4 (0) | 9 | 18 | 13 |
| His chromatic DI, a fourth lower, Fastest / Steady | Automatic | 78 | 100 % / 100 % | 0 | 0 | 6 / 2 | 4 / 0 |
| His chromatic DI, a minor sixth lower, Fastest / Steady | Automatic | 78 | 100 % / 100 % | 0 | 0 | 7 / 3 | 4 / 0 |

(The two notes missed from silence a fourth lower are one note on two pickups: the finger's
touch squeaks 9 ms before the pluck, and the pick seen there is judged before the pluck's level
has come. The extra notes on his lowered DI are the fretting moves of the original.) Before:
nothing under B1 was played, in any setting.

Pick to note-on, a note picked a second time, with a pick and with the fingers, on a bridge and
a neck pickup, median (and the most), 48 kHz:

| Note | Two periods | E2 range | B1 range | F♯1 range |
|---|---:|---:|---:|---:|
| F♯1 | 43.2 ms | | | 47.0 (49.5) |
| G1 | 40.8 | | | 44.5 (48.0) |
| G♯1 | 38.5 | | 42.5 (45.0) | 42.5 (45.0) |
| A1 | 36.4 | | 39.5 (43.0) | 40.0 (43.0) |
| A♯1 | 34.3 | | 38.0 (41.0) | 37.5 (41.0) |
| B1 | 32.4 | 35.5 (38.0) | 36.5 (39.0) | 35.5 (39.0) |
| C2 | 30.6 | 33.5 (37.0) | 34.5 (37.0) | 33.5 (37.0) |
| C♯2 | 28.9 | 33.0 (35.5) | 33.0 (35.5) | 32.0 (35.5) |
| D2 | 27.2 | 31.0 (34.0) | 31.0 (34.0) | 30.5 (34.0) |
| D♯2 | 25.7 | 29.5 (32.5) | 29.0 (31.5) | 29.0 (31.5) |
| E2 | 24.3 | 27.5 (31.0) | 27.5 (30.0) | 27.5 (30.0) |
| F♯2 | 21.6 | 25.5 (28.0) | 24.5 (28.0) | 27.0 (27.0) |
| A2 | 18.2 | 21.5 (24.0) | 21.5 (24.0) | 27.0 (27.0) |
| B2 | 16.2 | 19.5 (22.0) | 21.5 (22.0) | 22.0 (27.0) |
| C♯3 | 14.4 | 17.5 (20.0) | 20.0 (21.5) | 20.0 (22.5) |
| D3 | 13.6 | 17.0 (19.5) | 17.5 (21.0) | 17.5 (21.0) |
| E3 | 12.1 | 16.0 (18.0) | 16.5 (18.0) | 16.5 (18.0) |
| G3 to E4 | 10.2 to 6.1 | 16.0 | 16.0 | 16.0 |

So the low ranges cost E2 nothing, the notes from F♯2 to C♯3 up to 5.5 ms (A2 in the F♯1 range),
and nothing above E3. In the E2 range and in Automatic on a six-string, every figure is 0.7.0's.

Trench shares the tracker. Its new options (`lowestHz`, `rumbleHz`) are off by default and the
`followedHz()` accessor changes nothing: 137 renders (every factory preset, Trench Mono and Poly
in four settings on the synthetic DI and seven piezo DIs, and his two DIs in Mono and Poly, two
settings, alone and through two presets) are byte-identical to main.

#### The delay

His setup, a low E, Fastest, as he ran 0.6.0:

| Part | Before | Now | Whose |
|---|---:|---:|---|
| The note's pitch: two periods of the low E, and 3 ms to be sure | 27.5 ms | 27.5 ms | physics, and ours |
| Sedef MIDI's audio buffer | 11.6 ms (512 samples at 44.1 kHz, the framework's default, each event sent one buffer after its sample) | 2.9 ms (128 samples) | ours |
| The interface's input latency | 1 to 4 ms | the same, now shown in the app | the interface |
| The instrument's output buffer and the interface's output | about 12 to 15 ms if it runs at 512 samples too | the same until he lowers it | the instrument |
| **Pick to MIDI out** | **about 41 ms** | **about 32 ms** | |
| **Pick to sound** | **about 55 ms** | **about 46 ms; about 37 with the instrument at 128 samples** | |

Per string, pick to MIDI out with a 2 ms interface, before and now (before, the buffer added
11.6 ms; now 2.9): low F♯ not played, now 52 ms; low B 49 and 41; low E 41 and 32; A2 35 and 26
(32 in the F♯1 range); D3 31 and 22; G3 and up 30 and 21.

What changed:
- **The buffer.** The standalone app asks the device for the smallest buffer of at least 2.5 ms
  (128 samples at 44.1 and 48 kHz) the first time it starts, also after an update from a version
  that left the default; a size chosen afterwards in Audio input… stays. The window's footer shows
  the buffer in ms and the device's input latency, and offers the way to a smaller one when the
  buffer is over 6 ms.
- **The port router.** A block's events can only go out once the block is in, and MIDI through a
  port carries no time stamp another app would honour (the framework's `sendMessageNow` has none;
  the block's last sample is "now"), so an event at offset o of N has already waited N − o. The
  router used to send it o later (every event one block late, exact spacing). It still does while
  the block is no longer than 3 ms; a longer block's events are spread over 3 ms, so a 512-sample
  buffer costs 5.8 + 1.5 ms on average instead of 11.6 (and at most the block). The floor is the
  rest of the block.
- **A provisional note-on** (the note sent at the tracker's first reading after the pick, and
  corrected if wrong) was measured, not built: on the bass lines that reading is right for 56 %
  of the picks, wrong for 13.2 % (114 of 864), and for the rest there is none before the note is
  sure anyway; when right it comes 2.0 ms sooner in the median (5.2 at the 90th percentile). On
  the sloppy lines the same 13.2 % and 2.0 ms; on his DI 3.2 ms sooner on the 23 notes of 78 that
  have such a reading. Thirteen wrong notes in 100 for 2 ms is not a timing mode worth offering.

**The floor that is left** is two periods of the note (43 ms for a low F♯, 32 for a low B, 24 for
a low E, 18 for an A, 14 for a D), the 3 to 4 ms the MIDI layer takes to be sure, 16 ms for any
note above E3 (one period of the low E, to rule out a lower note), and the rest of the audio
buffer. Playing the line an octave higher with the octave at −2 halves the first part. (The next
section takes most of this floor away.)

**CPU.** The tracker inside Sedef MIDI now keeps periods down to 38 Hz up to date in every
range: 229 ns per sample (1.1 % of one core at 48 kHz) against 197 before, on the same machine in
the same hour; `process()` still never allocates.

**Left open:** a hammer-on, a pull-off and a slide cannot be pitch bend (above); a whole-step
bend faster than about 120 ms comes out as notes; strummed chords give more notes in Automatic
than in the E2 range (110 against 98 for 144 chords: two strings together repeat at a long
period), as they are outside the design; a finger's squeak 9 ms before its pluck after a rest
can lose the note; and Steady's 38 ms cannot hold a note under about B1.

### Sooner than two periods (2026-11-12)

After 0.8.0 the musician's one complaint left is the delay. The buffers are his to lower (above);
what is left is ours: how long Sedef MIDI takes to be sure of a note. This round finds each wait,
says what it is for, and removes what a better reading makes unnecessary, in Fastest only. Steady
plays exactly as before: its events are identical on every recording and every set here.

#### Where the time went

Pick to note-on in 0.8.0's Fastest was the sum of these, each put there by an earlier round:

| Wait | How long | What it protects against |
|---|---|---|
| The tracker judges a period on a window one period long | two periods: 24.3 ms for a low E, 18.2 for A2, 13.6 for D3 | a period shows once a stretch as long as itself has come round |
| ...and on nothing inside the attack | 8 ms | the attack's short periods can look periodic |
| The tracker's lock | 1 ms from silence, 2 ms to change note | two readings in a row must agree |
| The MIDI layer: the reading holds | 2 ms | strings still ringing pull the tracker around |
| A note above E3: one period of the low E, after the attack | 16 ms, whatever the note | a first reading can name a partial of a low string |
| The same note again, picked no louder than it rang | 33 ms, whatever the note | a pick that drowns nothing may be a lower note on its way: the lowest note's two periods |
| A pick on a faded string, or seen only as a lost period: its level | 12 ms | the fretting hand breaks a period too, and adds no energy |
| The first note of all, or a pick 10 dB under the player's | 60 ms | a finger's touch before its pluck |

So a low E took 27.5 ms, every note from G3 up 16 ms, and the staple of a bass line, one note
picked again and again, 33 ms on any string.

#### Why the early reading can be trusted, and when

This is the argument the rule and its constants come from, and what it does not cover. Each
result is marked **proved** (it holds for every signal of the model), **computed** (from the
model in closed form, or by a search or a simulation over it, the method given) or **measured**
(on the test sets and the musician's DI). The tests named check the proved and computed results on
the converter itself; the scripts that computed the rest are numpy over the model below, outside
the repository.

**The signal.** For a time `u` after a pick at `u = 0`:

    x(u) = sum over n of  a_n sin(2 pi f_n u + p_n) exp(-u / T_n)     the string
         + c(u)                                                       the pick's transient
         + r(u)                                                       what rang before the pick
         + w(u)                                                       noise

with `f_n = n f0 sqrt((1 + B n^2) / (1 + B))` (a stiff string: `B` is about 1e-4 for a wound
string) and `a_n` the pluck's and the pickup's comb, `sin(n pi beta) / n x sin(n pi gamma)` for a
pluck at `beta` of the string heard at `gamma`. `e_n` is partial `n`'s share of the energy after
the early band (40 Hz to 1.5 kHz). The phases `p_n` matter. A pluck releases the string from
rest, so every partial starts at once: in sine phase for a magnetic pickup, which hears the
string's velocity (plucked and heard near the bridge, that is one narrow pulse a period), and in
cosine phase for a piezo, which hears the force at the bridge, which the release changes at once
(a step). The synthetic sets draw the phases at random, which no plucked string does; 7 and 9 are
where that mattered.

**The measure.** The stretch `s_k` is the band at 8 kHz from `T_a` = 2.5 ms after the pick. At
`n` samples of it, for a lag `L`, every pair of samples a lag apart is compared:

    d_n(L) = sum (s_k - s_(k-L))^2 / sum (s_k^2 + s_(k-L)^2),      k from L to n - 1.

`W = n - L` is the window: the part of the stretch that has come round. `d` is 0 for a stretch
that repeats exactly at `L`, about 1 for one that does not repeat, 2 at most.

**1. The true period, over any window (proved).** Write `s = p + e`: `p` periodic at `P`, `e`
everything else, `eps` the energy of `e` in the two windows over that of `p`. At `L = P` the
string cancels sample for sample, `sum (s_k - s_(k-P))^2 = sum (e_k - e_(k-P))^2`, whatever `W`
is, so `d_n(P) <= 2 eps / (1 - sqrt eps)^2` (`(a - b)^2 <= 2 a^2 + 2 b^2` above, the triangle
inequality below). Noise does not repeat, and leaves `1 / (1 + SNR)` on average, `SNR = 1 / eps`:
the limit 0.05 needs the note 12.8 dB over the noise in the band, 0.02 needs 16.9 dB, 0.4 needs
1.8 dB. The rest of `e` (computed from the model):

| What does not repeat | Its share at the note's period |
|---|---|
| The partials drifting apart (`B` = 1e-4) | 0.038 at F♯1, 0.020 at B1, 0.007 at E2, 0.002 at A2 on the bright bridge model; 0.027, 0.015, 0.007, 0.003 on the piezo model; under 0.001 from G3 up |
| The same at `B` = 4e-4, four times a wound string's stiffness | 0.034 to 0.058 at F♯1, 0.014 to 0.032 at E2 |
| The pick's noise burst (the model's: 0.2 of the peak, decaying in 1.5 ms) after `T_a` | under 4e-4 of the windows for every note |
| A note 20 dB under the pick still ringing | 0.01 of the energy, 0.02 at most |

So the limit 0.05 for a note with nothing under it is the drift of the lowest note of the lowest
range with a margin, and 0.02 is the drift of the notes it applies to (from about A2 up) with a
margin. `T_a` is not set by the pick's noise, which has gone by then, but by the finger or the
pick letting go, which takes 1 to 2 ms: read from the pick itself, 3 of 234 notes of his DI came
out a semitone flat (measured).

**2. The octave above, exactly, over any window (proved).** Split the string into its even
partials `E` and its odd ones `O`. Half a period on, `E` is the same and `O` has changed sign:
`s_(k-P/2) = E_k - O_k`, sample for sample. So

    d_n(P/2) = sum 4 O_k^2 / sum ((E_k + O_k)^2 + (E_k - O_k)^2) = 2 x (the odd partials' share of the windows),

with no cross term, at any `W`. On every model the odd partials hold 41 % of the energy or more
(`2 x` that is 0.82 to 1.0, the table of 4): the octave above could be named only if they held
under 2.5 %, and could hold the note back only under 20 %. With the fundamental 30 dB down, the
3rd, 5th and 7th partials still hold the share (a test).

**3. A longer period cannot win (proved).** The rule reads the dips shortest first, and any dip
that leaves 0.4 or less refuses every longer one. A longer period has its window no sooner than
the true one (the window it needs grows with it). So the octave below, a lower semitone and the
period two ringing strings share can be named only while the true dip leaves more than 0.4: by 1,
while the noise is within 1.8 dB of the note.

**4. A whole period of window refuses every wrong period (proved).** If `W >= P`, the difference
`s_k - s_(k-L)` of the string is periodic at `P`, so a window holds at least `floor(W/P)` whole
periods of it, and each of the two windows of `s^2` at most `ceil(W/P)`. Over whole periods the
partials are orthogonal (Parseval), so

    d_n(L) >= D(L) floor(W/P) / ceil(W/P),     D(L) = sum over n of e_n (1 - cos(2 pi f_n L)).      (1)

`D` on the three models (computed from (1), `B` = 1e-4):

| | F♯1 | E2 | A2 | D3 | G3 | E4 | E5 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Bridge (pluck 0.10, pickup 0.07): partials of comparable energy | 6.6 | 6.3 | 6.1 | 5.9 | 5.4 | 4.2 | 2.6 |
| ...the deepest wrong dip of `D` (a semitone above the note and higher) | 0.97 | 0.98 | 0.98 | 0.98 | 0.97 | 0.96 | 0.96 |
| ...the octave above, `2 x` the odd share | 0.99 | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 | 0.96 |
| Neck (pluck 0.15, pickup 0.25): deepest wrong dip | 0.90 | 0.98 | 0.99 | 0.99 | 0.99 | 0.97 | 1.21 |
| Piezo (pluck 0.20, half the fundamental): deepest wrong dip (the octave above) | 0.82 | 0.87 | 0.87 | 0.87 | 0.86 | 0.84 | 0.99 |
| A semitone above, neck / piezo | 0.41 / 0.52 | 0.35 / 0.46 | 0.32 / 0.43 | 0.29 / 0.39 | 0.26 / 0.34 | 0.22 / 0.27 | 0.14 / 0.17 |

(The first row is `1 / sum e_n^2`, the number of partials of comparable energy.) So over a whole
period a wrong period leaves 0.41 or more on every model: more than 0.4, which is why a wrong
period seen whole never even holds a note back, and eight times the limit. A semitone off is not a
dip of its own but the slope of the true one; 8 is about its place.

**5. Under a whole period nothing can be proved (proved).** Fine and Wilf (1965): a sequence with
periods `p` and `q` that is at least `p + q - gcd(p, q)` long has period `gcd(p, q)`, and for every
shorter length there are sequences with both periods and not the gcd. Here: a stretch that repeats
exactly at a wrong `L` and comes from a string of period `P > L` is excluded only once
`n >= L + P`, a window of a whole period of the *true* note. In energy terms, the least share of
its mean energy that a sum of `K` partials keeps over a window of `alpha` periods (the least
eigenvalue of their Gram matrix over the window; computed):

| K \ alpha | 0.33 | 0.5 | 0.6 | 0.8 | 0.9 | 1 | 1.5 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2 | 0.013 | 0.15 | 0.39 | 0.51 | 0.70 | 1 | 0.72 |
| 4 | 1e-6 | 5e-4 | 0.008 | 0.26 | 0.40 | 1 | 0.67 |
| 6 | 5e-11 | 8e-7 | 7e-5 | 0.094 | 0.23 | 1 | 0.67 |
| 12 | 0 | 0 | 2e-11 | 2e-4 | 0.094 | 1 | 0.67 |

and the model's own strings, the amplitudes of their partials fixed and only the phases chosen
worst (coordinate descent from 8 starts on a 96-point circle per partial), at the window the rule
gives each wrong period:

| Worst phase: least `d` at a wrong period / the limit | F♯1 | B1 | E2 | A2 | D3 | G3 and up |
|---|---:|---:|---:|---:|---:|---:|
| Bridge | 0.00 | 0.01 | 0.00 (at 0.855 of the period) | 0.02 | 7.4 | 9 or more |
| Neck | 0.00 | 0.04 | 0.05 | 2.1 | 8.5 | 9 or more |
| Piezo | 0.00 | 0.01 | 0.01 | 0.95 | 9 or more | 9 or more |

So a guarantee for any string needs a window of a whole period of the true note: a note at
`T_a + 2/f0`, the tracker's two periods. That is where the mathematics stops; everything sooner is
either covered by 6 or carries a risk, which 7 measures.

**6. The floor makes every note from 200 Hz up safe (proved).** Take a true note whose period `P`
is 5 ms or less. A wrong period shorter than `P` is under 5 ms, so a note an octave under it is in
every range, and the rule needs a window of a period and 5 ms (`kEarlyAloneSeconds`) for it: at
least `P`, and 4 refuses it. A wrong period longer than `P` gets its window only after the true
period has a longer one, and 3 refuses it. So in every range **every note of 200 Hz and up (from
G♯3) is never named wrong** by the early reading, given 1's signal to noise; the worst phases of 5
agree (G3 and up: no wrong period under 9 times the limit). The other floor, 4 ms for a period
with nothing an octave under it (`kEarlyWindowSeconds`), never binds: such a period is longer than
half the range's lowest one, 7.1 ms or more, and 0.6 of it is 4.3 ms. Lowering the 5 ms to 4 ms
would give 0.2 to 1 ms on every note from G♯3 up and lose this proof for G♯3 to B3, whose
periods are longer than 4 ms: not taken.

**7. Under the floor: the risk, and the phases (computed and measured).** Below 200 Hz the rule
names a note after 0.6 of its period has come round (`T_a + 1.6 P`), where 5 asks for
`T_a + 2 P`. Simulated on the model's strings with random phases (seed 7: the rule as in
`earlyEstimate()`, noise 60 dB under the note, the pick's noise; wrong notes per pick):

| Stretch that must repeat | Wrong | Picks | Time for E2 |
|---|---:|---:|---:|
| a third of the period | 32 | 36 000 | 19.0 ms |
| half the period | 52 | 240 000 | 21.0 ms |
| 0.6 of the period (the rule) | 8 (F♯1, B1 and E2 only; none from A2 up) | 360 000 | 22.0 ms |
| a whole period | 0 | 48 000 | 27.0 ms |

Measured on the test sets, which have random phases too: 5 wrong in 3749 early notes at a third,
1 in 1613 at a half, 0 in 5089 at 0.6. That is the trade 0.6 makes: 2 in 100 000 on random
phases, 5 ms sooner on a low E than a whole period.

But a pluck's phases are not random (the signal, above), and 5 showed that the worst phases
exist. Strings whose partials all start together, F♯1 to C4, three plucks and five pickups, sine
and cosine phase (930 strings, two picks each, rendered as WAV files and read by
`sedef-midi-render`): 0.8.0 named every one right; the early reading as first written named 27
wrong, and two mechanisms explain all of them.

- *One pulse a period.* Plucked and heard near the bridge, in sine phase, the string is a narrow
  pulse each period. The first pulse is inside `T_a`; until the second has come round to the
  third (9), the windows hold only the quiet between pulses, which repeats at many periods (an A2
  read as B2 at 15 ms, `d` = 0.035). Its windows then hold 2 to 6 % of the energy their length's
  share of the stretch would; a string's windows hold 0.21 or more of it at the moment a right
  note is named (0.56 or more in 99 of 100; computed over random-phase and in-phase strings, E2 to
  G3, six plucks and pickups). So the windows must hold a quarter of their share
  (`kEarlyDensity`): at no measurable cost on random phases (mean +0.005 ms), and 18 of the 27
  gone.
- *A piezo's step.* In cosine phase the release is a step, and the band's high-pass answers it
  with a ring of its own (28 Hz, decaying in 5.6 ms). On a low string (F♯1 to D2) that makes the
  stretch's first 1.8 periods repeat at a period 3.5 % too long, to `d` = 0.001: a note read 0.6
  semitone flat and named a semitone low. Without the high-pass the same strings read within 0.12
  semitone; but the high-pass cannot go (10). What the ring does not do is move the later part of
  the stretch the same way: read from the later half of the two windows only, the dip names the
  right note or none. So the note named must be the note the later half names. That removed the
  other 9; on the sets it changed nothing (below).

Now: 930 of 930, at a median 18.3 ms after the pick where 0.8.0 took 23.0. A test runs 186 such
strings from F♯1 to C4 on the tests' own string model, and the 18 on which a guard was needed on
the recordings' model.

**8. A semitone off: the dip's place (proved, then computed).** For the string alone, `d(L) >= 0`
with equality only at `P` and its multiples, so over any window the dip is exactly at the period:
a semitone neighbour is never a dip of its own. What does not repeat moves it. Over whole
periods, near `P`, the string leaves `c g^2` of `p`'s energy for a period off by a fraction `g`,
with `c = 2 pi^2 sum e_n (f_n / f0)^2` the dip's curvature. The dip is where the difference is
least, so it is no larger there than at `P`; by the triangle inequality the string's part of it
is then at most twice the most `e` can give, and `|g| <= sqrt(8 eps / c)`. So the dip stays within
half a semitone (`g` = 0.0293), whatever the shape of `e`, while `eps < c x 1.07e-4`: 0.086 to
0.044 at F♯1, 0.054 to 0.023 at E2, 0.038 to 0.010 at G3, 0.016 to 0.007 at A4, 0.009 to 0.005
at E5 (bridge to neck model), 0.002 for a pure tone; under a whole period the curvature is the
window's (5), and can be less. Noise is not the worst shape: its cross term has no sign, and it
moves the dip by nothing measurable. A residual with a shape can move it far, as
the step of 7 did, and that is what the later half guards, for periods of 5 ms or more (under
5 ms the in-phase strings of 7, G♯3 to C4, read right without it).

The lag grid: the parabola through three whole lags places the dip to a fraction of a lag, but
for a short period the value at a whole lag is too coarse. A period half a lag off leaves
`c / (4 L^2)` for a period of `L` lags: at 440 Hz (18 lags) 0.015 for a pure tone and 0.05 to 0.11
on the models, at 200 Hz (40 lags) 0.015 to 0.056, over the 0.02 allowed. So a period under 5 ms
(`kEarlyFineSeconds`, 40 lags) is measured again a quarter of a lag apart, the stretch read
between its samples by a cubic through four of them. Only a dip that leaves 0.3 or less at its
whole lag is measured again (`kEarlyFine`): half a lag adds at most 0.19 (at the shortest period
read, 1.4 kHz), so a dip over 0.3 cannot come down to 0.05. What the decimation folds down is not
periodic between the samples: the early band is cut a second time at 1.5 kHz (34 dB down at the
fold, 51 dB where it lands in the band).

**9. One pulse a period needs two periods (the measured limit, predicted).** Take 7's pulse
string. At lag `P` the pairs compared are `u` and `u - P` for `u` from `T_a + P` to now; the first
pulse (at 0) is before `T_a`, the second (at `P`) is before `T_a + P`, so neither window holds a
pulse until the third pulse (at `2P`) arrives and is paired with the second. Until then the
windows hold what does not repeat between pulses, and 1 says nothing; at `2P` and a pulse's width
the true period appears at once. So the model predicts: **a string that gives one pulse a period
is named no sooner than two periods after its pick, by any reading that starts after the
attack**. That is the measured limit on his D and G strings (0.06 to 0.20 left at one period and
4 ms; "on his D string a note still takes about two periods", below), and the converter does it:
on 7's in-phase pulse strings from E2 to B2 (plucked at 0.06 and 0.10, heard at 0.05) the note
goes out 1.96 to 2.04 periods after the pick (one at 3.6), every one right, and a test checks
that it is never before `2/f0 - 1 ms` and always by the tracker's time.

**10. Something slow repeats at every short period (proved).** A component at `f` leaves
`2 pi^2 (f L)^2` at a lag `L` while `f L` is small: under 0.02 for anything below 45 Hz at the
shortest period read (0.71 ms). That is why the early band is high-passed at 40 Hz, second order
(24 dB down at 10 Hz): with a first-order high-pass at 10 Hz instead (tried, to take the step's
ring of 7 away), what a pluck's onset puts under 40 Hz made 93 of the 930 strings of 7 come out as
a note around C♯6, the shortest period. And at half the period a slow component leaves a quarter
of what it leaves at the period, where the string's own octave above leaves twice its odd share
(2: 0.82 or more). So half the period must leave more than 0.4 (`kEarlyOther`): no cost on a
string, and no note read from the band's own ring.

**The window each mistake needs, W(f).** Collected, for a note `f` read from `T_a` after its pick,
with `eps` the energy in the windows of what is not the string, over the string's (1):

| The note against | The window that tells them apart | Given | From |
|---|---|---|---|
| `2f`, the octave above | any | the odd partials hold more than 2.5 % (every model: 41 % or more) | 2 |
| `f/2`, the octave below, and every longer period | any: refused by the true dip | the true period leaving 0.4 or less: the note 1.8 dB over the noise, or any `e` with `eps` under 0.095 | 3, 1 |
| a semitone either side | any: the dip is at the period | `eps < c x 1.07e-4` over whole periods (8), or the later half naming the same note | 8 |
| every shorter wrong period: a partial, a lower string's partial, the quiet between pulses | `W >= 1/f`, one whole period of the note | any string; under it no bound exists | 4, 5 |
| the true note itself, to be named | its period leaving 0.05 (or 0.02) | 12.8 dB (16.9 dB) over the noise, `T_a` past the release | 1 |

So `W(f) = 1/f`, after `T_a`: a note proved right at `T_a + 2/f`. The rule gives that window for
every note of 200 Hz and up (6), and names a lower note sooner, after 0.6 of its period, on the
risk 7 measures.

**The rule, and its time.** A note of `f0` in a range whose lowest note is `f_low` (70, 52 and
41 Hz for the E2, B1 and F♯1 ranges) goes out at

    t(f0) = T_a + 1/f0 + max(4 ms, 0.6 / f0)       if 2/f0 > 1/f_low  (nothing an octave under it)
    t(f0) = T_a + 1/f0 + max(1/f0, 5 ms)           otherwise          (a lower string could be)

if by then: what the period leaves is at most 0.05, or 0.02 (1); no shorter dip leaves 0.4 or
less (3); half the period leaves more than 0.4 (2, 10); the two windows hold a quarter of their
share of the stretch's energy (7); and, for a period of 5 ms or more, the later half of the
windows names the same note (7, 8). Otherwise the note waits for the tracker. Added to `t`: one
hop of the analysis (0.5 ms), the date of the pick (within 1 ms), the band's delay (0.5 ms): 2 ms.

| Note (range) | Kind | `t(f0)` | Bound: `t(f0)` + 2 ms | Proved right (5, 6) | Achieved, median (most) | Gap to `t(f0)`, median (most) |
|---|---|---:|---:|---|---:|---:|
| F♯1 (F♯1 range) | nothing under | 37.1 ms | 39.1 | no: from 45.7 (`T_a + 2/f0`), risk measured (7) | 37.0 (47.0) | -0.1 (+9.9) |
| B1 (B1 range) | nothing under | 28.4 | 30.4 | no: from 34.9, risk measured | 28.5 (33.0) | +0.1 (+4.6) |
| E2 | nothing under | 21.9 | 23.9 | no: from 26.8, risk measured | 21.5 (23.5) | -0.4 (+1.6) |
| A2 | nothing under | 17.0 | 19.0 | no: from 20.7, risk measured | 17.0 (17.0) | 0.0 (0.0) |
| D3 | a lower string could be | 16.1 | 18.1 | its own period whole by `t`; a shorter one may go sooner, risk measured | 16.0 (16.0) | -0.1 (-0.1) |
| G3 | a lower string could be | 12.7 | 14.7 | as D3 | 12.5 (13.0) | -0.2 (+0.3) |
| B3 | a lower string could be | 11.5 | 13.5 | yes, at `t` (6) | 11.5 (11.5) | 0.0 (0.0) |
| E4 | a lower string could be | 10.5 | 12.5 | yes | 10.5 (10.5) | 0.0 (0.0) |
| A4 | a lower string could be | 9.8 | 11.8 | yes | 9.5 (10.0) | -0.3 (+0.2) |
| E5 | a lower string could be | 9.0 | 11.0 | yes | 9.0 (9.0) | 0.0 (0.0) |

The six-string range, unless named. Achieved is the converter's latency report: the second of two
picks, with a pick and with the fingers, on a bridge and a neck pickup, three phases each (12
strings a note). The most is a note the signal did not settle (a fingered low string), which went
out at the tracker's time; every median is within half a millisecond of `t(f0)`, which is the
analysis grid. Over every semitone from F♯1 to E6 (236 strings: two pickups, two phases, noise 60
and 35 dB under the note), 194 are settled by the signal alone, by 1's measure computed in the test
on the signal itself, and the latest of those goes out 0.13 ms after `t(f0)`, inside the 2 ms.

**What each constant comes from.** (`GuitarToMidi.h`, and the four early ones at the top of
`GuitarToMidi.cpp`.)

| Constant | Value | From |
|---|---|---|
| `kAttackSkipSeconds` | 2.5 ms | 1: the release (1 to 2 ms) is not the string's period, and the pick's noise holds under 4e-4 of the windows after it. Measured: read from the pick itself, 3 of 234 of his notes came out a semitone flat |
| `kEarlyClean` | 0.05 | 1: the partials' drift at F♯1 (0.038) with a margin, and 12.8 dB over the noise; 4: a wrong period seen whole leaves eight times it |
| `kEarlyShare` | 0.6 | 5, 7: there is no bound under a whole period; 0.6 is the measured trade (8 wrong in 360 000 random-phase picks, 0 in 5089 early notes, 5 ms sooner than a whole period on E2) |
| `kEarlyWindowSeconds` | 4 ms | 6: the floor for a period with nothing an octave under it. It never binds in the three ranges (0.6 of such a period is 4.3 ms or more); it is there for a range that would |
| `kEarlyAloneSeconds` | 5 ms | 6: every period the rule may name with a note an octave under it gets a window of at least every period up to 5 ms, so every note of 200 Hz and up is proved |
| `kEarlyAlone` | 0.02 | 1: the drift from A2 up (0.007 at E2 and less) with a margin, and 16.9 dB over the noise; 4: a lower string's partial seen whole leaves 0.41 or more (his DI, slowed down: 0.15 at best) |
| `kEarlyOther` | 0.4 | 1, 3: the true period leaves at most 0.4 down to 1.8 dB over the noise, and then refuses every longer one; 4: a wrong period seen whole leaves 0.41 or more, so it never holds a note back; 2, 10: half the period must leave more, as a string's octave above does (0.82 or more) |
| `kEarlyDensity` | 0.25 | 7, 9: the quiet between two pulses holds 2 to 6 % of its share of the stretch, a string's windows 21 % or more when the right note is named |
| `kEarlyFineSeconds` | 5 ms | 8: under it, a period half a lag off leaves more than the 0.02 allowed, so those periods are read between the lags; from it up, the later half is read instead |
| `kEarlyFine` | 0.3 | 8: half a lag adds at most 0.19, so a dip over 0.3 at its whole lag cannot come down to 0.05 |
| `kEarlyRingDb` | 20 dB | 1: a note 20 dB under the pick adds 0.02 at most. A louder one that could not be taken out holds the same note, and the notes it is a partial of, for the tracker |
| `kEarlyOldClean` | 0.1 | measured ("The note before, still ringing"): the note before is steady enough that its last period, repeated, predicts what it would have gone on to do |
| `kEarlyRingsOn` | 0.7 | measured: it rings on only if taking it out removes 30 % of the stretch's energy; taking it out always gave 28 wrong in 1017 early notes |
| `kEarlyBrought` | 0.25 | measured: what is left without it must be a quarter of the stretch; noise over a ringing note brings less |
| `kEarlyHeld` | 0.1 | the same note again: the tracker's reading from before the pick leaves one semitone to decide, so twice `kEarlyClean`. Measured: 0.2 gave 2 wrong in 78 on his DI |
| `kEarlyHeldCents` | 45 cents | 8: the dip within half a semitone (50 cents) of that reading, less a margin for the reading's own error |
| `kEarlyHeldApart` | 0.2 | 4, 8: a semitone either side must leave 0.2 or more (a semitone above leaves 0.14 to 0.52 over a whole period on the models); a dip that does not tell the semitones apart is not that note's |
| `kEarlyHeldPart` | 0.25 | 2: a string picked again leaves 0.82 or more at half its period; a note an octave or a twelfth over a ringing one leaves much less there, without the old note (measured: 2 in 864 without the check) |
| `kEarlyHeldSeconds` | 12 ms | measured: a fretting finger that lands just after the pick must have had time to show |
| `kEarlyHeldSkipSeconds` | 5 ms | measured: read from 2.5 ms, the old note's last milliseconds are in the stretch (3 of 10 repeated picks qualified) |
| `kEarlyStill` | 0.25 | 4: by the tracker's time the stretch holds a whole period: the right note's period leaves the drift and the noise (0.05 or less), a wrong one 0.41 or more; 0.25 is between them |
| `kEarlyHighestHz` | 1400 Hz | the tracker's highest note: the early reading looks no higher |
| `kEarlyGraceSeconds`, `kEarlyMinGraceSeconds` | 7 ms, 15 ms | 5: the tracker needs its two periods (`T_a + 2/f0`, where a reading can be proved) and a margin before its reading may count against an early note |
| `kEarlyLimitSeconds` | 150 ms | the longest the tracker is waited for |

**Each limit pushed past what the argument allows.** One at a time, in a copy of the tree: the
early and invariance tests run, and a change none of them catches runs every MIDI test but the
long ones. The test named is the first that fails.

| Pushed | To | Caught by |
|---|---|---|
| `kAttackSkipSeconds` | 2.5 → 0.5 ms | "plays the same note picked again once each time, and sooner" (and four more) |
| `kEarlyWindowSeconds` | 4 → 1 ms | none, and none can: it never binds (6). Raised past 4.3 ms it would, and the notes with nothing under them would wait longer |
| `kEarlyShare` | 0.6 → 0.3 | "early reading on the low strings of seven and eight strings" (a wrong note), the pinned streams |
| | 0.6 → 1.0 | "names a clean note from one period of it" (too late) |
| `kEarlyClean` | 0.05 → 0.15 | "early reading on the low strings of seven and eight strings", "notes move with the playing when it starts later", the pinned streams |
| | 0.05 → 0.01 | "names every semitone from F#1 to E6 within the rule's time" |
| `kEarlyAloneSeconds` | 5 → 1 ms | "early reading on the low strings of seven and eight strings" |
| `kEarlyAlone` | 0.02 → 0.1 | "waits out a fret that lands just after the pick" |
| | 0.02 → 0.004 | "names a clean note from one period of it", the semitone bound, the four rates, the pinned streams |
| `kEarlyOther` | 0.4 → 0.05 | "early reading is never a wrong note" |
| | 0.4 → 0.9 | "early reading is never a wrong note", "the same note picked again", and two more |
| `kEarlyDensity` | 0.25 → 0 | "the model's hardest signals", partials that start together as the recordings were made (every pulse from E2 to C3; added for this) |
| | 0.25 → 0.8 | the pinned streams (notes later) |
| Half the period must leave more than `kEarlyOther` | off | none. A knock under the pick (added) does not reach it: the early reading then waits for its level check, and the in-phase strings of 7 never needed it. Kept for 10: it costs a string nothing (2) |
| The later half names the same note | off | "the model's hardest signals", partials that start together as the recordings were made (every piezo step from F♯1 to D2; added for this) |
| `kEarlyFineSeconds` | 5 → 0 ms | the semitone bound |
| `kEarlyFine` | 0.3 → 0 | the semitone bound (the second run) |
| `kEarlyRingDb` | 20 → -40 dB | "early reading on the low strings of seven and eight strings" |
| `kEarlyOldClean` | 0.1 → 2 | "a slide gives its frets, never a burst of notes" (the second run) |
| `kEarlyRingsOn` | 0.7 → 0 | "early reading is never a wrong note" |
| | 0.7 → 10 | "early reading is never a wrong note", "notes move with the playing when it starts later", the pinned streams |
| `kEarlyBrought` | 0.25 → 0 | none. On the sets it changes no note (two more go out early, both right): what it guards, noise over a ringing note taken for a note, is not in them |
| `kEarlyHeld` | 0.1 → 0.5 | the semitone bound |
| `kEarlyHeldCents` | 45 → 150 cents | "the same note picked again once each time, and sooner" |
| `kEarlyHeldApart` | 0.2 → 0 | none. One more early note on the sets, right: what it guards (a string gliding to the next fret, read before the pick) is only in his DI, which stays out of the repository |
| `kEarlyHeldPart` | 0.25 → 0 | none. The sets give the same notes (one to three more go out early, all right): the case it was added for, a note an octave over a ringing one (2 in 864 on the sloppy lines then), no longer reaches it |
| `kEarlyHeldSeconds` | 12 → 3 ms | none. The sets give the same events and the fret test still passes: here another check of the same note again covers the fret that lands after the pick |
| `kEarlyHeldSkipSeconds` | 5 → 2.5 ms | "waits out a fret that lands just after the pick", the pinned streams |
| `kEarlyStill` | 0.25 → 10 | none. It acts only on an early note the tracker does not confirm, and no early note here is wrong: there is nothing for it to correct |
| `kEarlyHighestHz` | 1400 → 3000 Hz | none. A string over the tracker's reach starts no early note either way; a test (added) checks that Fastest names what Steady names there |

#### The same events, whatever the host does

`GuitarToMidiInvarianceTests.cpp` holds the converter to what must not change its notes, and to
the derivation above. Every signal is built from `sedef::Random` (integer arithmetic, the same
numbers everywhere; no standard-library distribution), with no clock and no thread.

| What changes | What must stay | Why that much |
|---|---|---|
| The block size: 1, 32, 64, 97, 128, 441, 512, 4096 samples | every event at the same sample, in Fastest, Steady and Notes + Bend | every clock runs on samples, and every step is taken per sample |
| The level by a power of two (-18 and +12 dB) | every event at the same sample | it scales every value exactly |
| Any other level (-20 dB, +12 dB less a hair) or a DC offset | the same notes, each within 48 samples, velocity within 1 | each sample rounds anew; the level is kept as one value a millisecond (48 samples) and a pick is dated on that grid |
| The start, by a multiple of 48 samples | every event exactly that many samples later | the playing meets the same analysis grid |
| The start, by any other number of samples | the same notes, within 48 samples | it meets the grid at another phase |
| 44.1, 48, 88.2 and 96 kHz | the same notes, within the rule's time, within 1.5 ms of 48 kHz | the analysis runs at 7350 to 8000 Hz: one hop (0.5 ms) and one millisecond of the level apart at most |
| Every semitone F♯1 to E6, two pickups, two phases, two noise levels | the note right, by `t(f0)` + 2 ms when the signal settles it, by the tracker's time when it does not | the derivation |

`sedef-midi-render` gives the same listing and the same MIDI file, byte for byte, run twice and
at blocks of 64 and 512 samples (the test `midi-render-is-deterministic`).

**What is exact where.** On one build, every event is at the same sample run after run and at
every block size, on every platform. Across platforms the pinned event streams keep their notes,
channels, velocities and order exactly; their samples are exact on macOS on Apple silicon, where
they were printed, and within 48 samples elsewhere. The last bits of the arithmetic differ between
compilers and maths libraries (MSVC builds with `/fp:fast`, compilers fuse multiply-adds
differently, `exp2` and `log2` round differently), and a reading that meets its threshold one step
of the analysis later moves its event by one hop (24 samples) or one millisecond of the level
(48). Strict floating point for the MIDI layer alone would not remove that: the tracker it reads
is shared with Trench, whose arithmetic, and so the amp's renders, must not change.

#### The early reading

The tracker's window slides: it holds the last period and compares it with the one before, so it
needs two periods of the new note inside it. But a picked string repeats from its pick on. The
MIDI layer now keeps, for every period, how well the signal *since the pick's attack* (the
stretch: from 2.5 ms after the pick) repeats one period on: the sums of a difference function as
in YIN (de Cheveigne and Kawahara, 2002), over a stretch that grows from the pick. What a period
leaves unexplained of the stretch is 0 when the stretch repeats exactly and about 1 when it does
not repeat at all. The shortest period that qualifies starts the note, before the tracker has
read it:

| A note... | goes out when | Why that is enough |
|---|---|---|
| with nothing an octave under it in the range (on a six-string, up to C♯3) | 0.6 of its period (4 ms at least) has come round again, leaving at most 0.05: 1.6 periods and the attack's 2.5 ms | nothing lower can be on its way, and a shorter period would have shown first |
| that a lower string could be under | it alone accounts for the stretch (at most 0.02 left), over two of its periods and 5 ms at least | a string's partial never does: on the musician's DI, as recorded and slowed down, the best a low string's partial reaches is 0.15, seven times that |
| either way | no shorter period leaves less than 0.4 | two strings together repeat at a period neither has; an octave under the played note is a multiple of its period |
| either way | half its period leaves more than 0.4, the two windows hold a quarter of their share of the stretch's energy, and, for a period of 5 ms or more, the later half of the windows names the same note | something slow (the band's answer to a step) repeats at every short period; a string that gives one pulse a period has only the quiet between its pulses in the windows until the pulse has come round twice; the attack can move a low note's dip by more than half a semitone, where the string moves it nowhere |
| the same as the string already had | the stretch from 5 ms after the pick repeats at that note's period (at most 0.1 left, over 0.6 of a period, a semitone to either side leaving 0.2 or more), not before 12 ms | the pick ended what the string did, and the tracker's reading from before the pick names the note |

**The note before, still ringing.** A note left ringing under the next makes the input repeat at
its own period too. When that note was steady before the pick, the stretch is kept a second time,
less what the note would have gone on to do (its last period before the pick, repeated). If that
leaves clearly less (under 0.7 of the energy), the old note rings on, and what is left is what
the pick brought: that is what is read. If not, the pick ended it, as a pick on the same string
does, and the stretch as it is is the new note. While a note that could not be taken out still
rings within 20 dB of the pick, the same note and every note it is a partial of wait for the
tracker.

**After the note-on.** The tracker confirms. Until it has had its own two periods, what it still
holds (nothing, the note before, a partial on its way) says nothing; once it names the note, the
note is like any other. If it never does, and the stretch, twice as long by then, no longer
repeats at the note's period, the early reading was wrong, and whatever the tracker names next
replaces the note. On all the material here that correction never had to act on a right note,
and no early note is wrong (below).

The early reading starts a note only for a pick the tracker's detector saw (or, for the same note
again, one seen as the note losing its period), as loud as this player's picks, its level
checked. A pick's level is its loudest millisecond 3 to 12 ms on, which only grows: in Fastest it
now counts as soon as it has shown its energy, where it waited the 12 ms out.

#### What it gives

Pick to note-on, the second of two picks, with a pick and with the fingers, bridge and neck
pickups, 48 kHz, median (and the most), before and now:

| Note | Two periods | E2 range and Automatic | B1 range | F♯1 range |
|---|---:|---|---|---|
| F♯1 | 43.2 ms | | | 47.0 (49.5) → 37.0 (47.0) |
| B1 | 32.4 | | 36.5 (39.0) → 28.5 (33.0) | 35.5 (39.0) → 28.5 (33.0) |
| E2 | 24.3 | 27.5 (31.0) → 21.5 (23.5) | 27.5 (30.0) → 21.5 (23.5) | 27.5 (30.0) → 26.5 (28.5) |
| A2 | 18.2 | 21.5 (24.0) → 17.0 (17.0) | 21.5 (24.0) → 20.5 (21.5) | 27.0 (27.0) → 20.5 (22.0) |
| D3 | 13.6 | 17.0 (19.5) → 16.0 (16.0) | 17.5 (21.0) → 16.0 (16.0) | 17.5 (21.0) → 16.0 (16.0) |
| G3 | 10.2 | 16.0 (32.5) → 12.5 (13.0) | 16.0 → 12.5 (13.0) | 16.0 → 12.5 (13.0) |
| B3 | 8.1 | 16.0 → 11.5 (11.5) | the same | the same |
| E4 | 6.1 | 16.0 → 10.5 (10.5) | the same | the same |
| A4 | 4.5 | 16.0 → 9.5 (10.0) | the same | the same |
| E5 | 3.0 | 16.0 → 9.0 (9.0) | the same | the same |

A lower range moves the line between the two kinds of note: in the F♯1 range a low E has a note
an octave under it, so it needs two of its periods and the attack. Automatic on a six-string is the E2
range; once it has learned a lower string it is that string's range.

The same note picked again, six picks 250 ms apart, each taking the string over at the level it
rang at, three pickups, median: E2 33.0 → 25.0 ms, A2 33.0 → 20.0, D3 33.0 → 16.5, G3 33.0 →
14.5.

The gates, Fastest, before and now (Steady: identical, event for event):

| Set | Notes | Right | Wrong | Missed | Extra | Under 40 ms | Latency p50 / p95 |
|---|---:|---|---|---|---|---|---|
| Bass lines | 864 | 99.42 % → 99.42 % | 0 → 0 | 5 → 5 | 1 → 0 | 1 → 0 | 21.8 / 33.6 → 17.9 / 28.5 ms |
| Sloppy lines | 864 | 99.19 → 99.19 | 4 → 4 | 3 → 3 | 9 → 6 | 10 → 9 | 25.0 / 54.6 → 20.3 / 34.8 |
| Fretted first | 312 | 100 → 100 | 0 | 0 | 0 | 0 | 16.2 / 33.0 → 12.1 / 20.6 |
| From silence | 324 | 100 → 100 | 0 | 0 | 0 | 0 | 18.4 → 13.2 (p95: the first note's 60 ms) |
| Bass lines, a fourth lower, B1 | 864 | 100 → 100 | 0 | 0 | 1 → 1 | 1 → 1 | 28.1 / 43.3 → 22.7 / 36.6 |
| Sloppy lines, a fourth lower, B1 | 864 | 98.84 → 98.84 | 3 → 3 | 7 → 7 | 11 → 9 | 11 → 10 | 32.5 / 59.8 → 26.5 / 44.8 |
| Bass lines, a seventh lower, F♯1 | 864 | 99.88 → 99.88 | 0 | 1 → 1 | 0 → 0 | 0 → 0 | 35.7 / 53.8 → 28.3 / 44.7 |
| Sloppy lines, a seventh lower, F♯1 | 864 | 98.61 → 98.84 | 4 → 4 | 8 → 6 | 17 → 13 | 12 → 10 | 42.3 / 59.8 → 34.6 / 53.3 |
| Bass lines, a seventh lower, Automatic | 864 | 99.77 → 99.77 | 0 | 2 → 2 | 3 → 3 | 2 → 2 | 35.7 / 54.0 → 28.3 / 46.7 |
| Sloppy lines, a seventh lower, Automatic | 864 | 98.50 → 98.73 | 4 → 4 | 9 → 7 | 18 → 14 | 13 → 11 | 42.1 → 34.5 |

Fretted first and from silence in the low ranges: unchanged (100 %, and 99.38 % from silence a
fourth lower), sooner by 4 to 5 ms on the median. Notes + Bend sends the same notes as Notes on the lines. Strummed chords,
outside the design, give a few more notes in two settings (113 against 110 for 144 chords in
Automatic, 39 against 34 a fourth lower in B1, 16 against 15 in F♯1), still never two at once.

Of the 2364 notes of the four sets, 1658 go out on the early reading (1699 a fourth lower, 1732
a seventh lower), and every one of those 5089 note-ons is the note played.

**His DI** (reported, not a gate; every setting at its default, Fastest): the same notes as
before, 78 of 78 right, none wrong, the same six fretting moves as extra notes and the same three
under 40 ms; the strummed chords give the same 29 notes. Onset to note-on, by run:

| Run (string) | D2 | G2 | C3 | F3 | A3 | D4 |
|---|---:|---:|---:|---:|---:|---:|
| Median, before → now | 22.4 → 20.8 ms | 19.0 → 16.5 | 16.2 → 14.0 | 13.6 → 12.6 | 15.9 → 10.3 | 15.5 → 10.2 |
| Most, before → now | 49.6 → 49.7 (the first note) | 27.1 → 26.8 | 33.1 → 16.2 | 24.1 → 17.7 | 32.7 → 17.2 | 33.4 → 12.4 |

Slowed down by a fourth and by a minor sixth, the same notes and the same extras as before;
medians 29.5, 22.4, 21.0, 16.4, 18.5, 16.4 → 27.8, 20.2, 18.1, 14.9, 13.7, 10.7 ms and 34.6,
27.3, 23.8, 20.6, 21.6, 16.3 → 31.8, 23.2, 20.2, 15.5, 17.0, 11.4 ms. Velocities move by at most
4 (one note; the rest by 0 or 1). The seven piezo DIs (fingerstyle chords) give the same number
of notes in Fastest, a few of them a little sooner.

**His lowest strings gain least**, and that is the signal, not the reading. Plucked near the
bridge with a finger and heard by a bridge pickup, his D and G strings are a pulse once a
period with little between: the first pulse is the pluck itself, inside the attack, and the
stretch after it holds nothing that repeats until the second pulse has come round to the third,
which is two periods. At one period and 4 ms his low notes leave 0.06 to 0.20 unexplained, most
over 0.10 (his high strings 0.002 to 0.007), and the pitch read there is up to 0.4 semitone off: not a reading
to send a note on. Reading from the pick itself, to catch the first pulse, was tried: the first
period after a pluck is not the string's (the finger lets go over a millisecond or two), and 3 of
234 notes came out a semitone flat (a C3 as B2, 12 ms after its pluck). So on his D string a
note still takes about two periods; with a pick, or on a neck pickup, a low E takes 1.6.

#### Tried and rejected

| What | Result | Kept instead |
|---|---|---|
| A third of a period (4 ms at least) as the stretch that must repeat | 5 wrong among 3749 early notes, all in the low ranges: a few milliseconds of a low string look like themselves 0.6 of a period on (a low B read as G2, leaving 0.03 to 0.05, and worse at once as the stretch grew) | 0.6 of a period: none in 5089 early notes. Half a period still let one through |
| A stricter 0.04 over half a period, instead of a longer stretch | no wrong note either, but a low B at 31.5 ms where 0.05 gave 26.5 | 0.05 over 0.6 of a period: a low B at 28.5 ms |
| Taking the old note out always | when the pick ended the old note (the same string, the usual case), taking it out adds a ghost that repeats perfectly at the old period: 28 wrong in 1017 early notes | both stretches, chosen by which holds less |
| The same note again at 0.2, named by the tracker's reading alone | his DI: 2 wrong notes in 78, 3 and 4 on the slowed-down versions (a G3 for 18 ms before the G♯3: the reading from before the pick was of a string gliding to the next fret, 200.5 Hz) | at most 0.1, the stretch's own dip on the same semitone as the reading, a semitone to either side clearly worse |
| ...read from 2.5 ms after the pick | the old note's last milliseconds are in the stretch: 3 of 10 repeated picks qualified | from 5 ms |
| ...while another note may ring an octave under it | a note an octave over a ringing one, taken for the ringing one picked again (2 in 864 on the sloppy lines) | what is left without the old note must not repeat at a half or a third of the period |
| ...from silence, for any pick | the note just ended, still all there is to hear, started again on the next note's soft pick (1 to 4 per range) | from silence only for a pick 6 dB over what rang |
| An early note ending when the tracker lets go | the tracker let go of the *old* note 50 ms after the pick: two notes cut after 31 and 34 ms | a note the tracker has not named yet is not one it has lost |
| The player's level from the early note's level at its note-on | the attack is not complete yet: velocities off by up to 20 | the level counts once the tracker has read the note |
| The early reading for a soft pick from silence | a finger's touch pushes the string, which sounds its own note: a squeak came out as a note for 37 ms | only for a pick within 10 dB of the player's level; the rest wait as before |
| The grace counted from the MIDI layer's pick | the tracker restarts its two periods at each pick it sees itself (a low string's attack swings): 16 early notes ended after 6 to 17 ms | counted from the tracker's own pick |
| The early band high-passed at 10 Hz, first order, instead of 40 Hz, second order, to take the band's ring after a piezo's step away | 93 of the 930 strings whose partials start together came out as a note around C♯6: what a pluck's onset puts under 40 Hz repeats at every short period | the 40 Hz band, and the later half of the windows naming the same note |
| The later half's dip within 15 cents of the whole windows' dip, instead of on the same note | 930 of 930 right, but one more extra note on the sloppy lines (an A2) | the same note |
| A second band, high-passed at 20 Hz, that must place the dip within 10 cents of the first (computed on the model) | it held back none of the three wrong notes it was for | the later half |
| Each window allowed its own mean (a difference function blind to what is constant over a window), to read through the band's ring after a step (computed on the model, 320 strings) | over the 40 Hz band: 2 of the 3 notes read a semitone low remained; over a first-order band at 5 or 10 Hz: none read low, but 2 read near E6 (10) | the later half |

#### The other fixed waits

Each shortened alone, over the four sets in three ranges (7092 notes; now: 7057 right, 11 wrong,
29 extra, 30 under 40 ms):

| Wait | Shortened to | Gain | Cost |
|---|---|---|---|
| The reading holds, 2 ms | 1 ms | 0 to 0.4 ms on the median | 2 more extra notes and 4 more under 40 ms |
| | 0.5 ms | the same | 3 more wrong notes |
| The same note again: 4 ms after the tracker's two periods | 2 ms, 0 | none: those notes go out on the early reading now, or wait for the lowest note | none |
| The attack, 2.5 ms | 1.5 ms | 1 ms on every note | 3 more wrong notes on a six-string |
| A note a lower one could be under: 5 ms at least | 4 ms | 0.2 to 1 ms from G♯3 up | none measured, but it gives up the proof that no note from 200 Hz up is misread ("Why the early reading can be trusted", 6) for G♯3 to B3; and like the tracker, nothing is read inside the first 8 ms of an attack |
| The stretch that must repeat: 0.6 of a period | 0.5 | 1 ms on low notes | one wrong early note |
| The same note again: read from 5 ms | 4 ms | none | none |

None is worth taking. What is left in Fastest is the attack's 2.5 ms, 1.6 periods of a low note
(two when a lower note could be under it, or when the string gives a pulse a period and little
else), the hop of the analysis, and the 60 ms of the very first note.

#### The audio buffer

The standalone app keeps asking for the smallest buffer of at least 2.5 ms (128 samples at 44.1
and 48 kHz). 64 samples would take 1.3 to 1.5 ms off every note. It was left alone because
nothing here can run it: no interface, no way to open the app. What it would risk is a callback
that misses its time on an interface or a system that does not hold 64 samples; for Sedef MIDI
that is not a click but a hole in the input, which reads as a wrong or a missing note, and the
app shows nothing when it happens. A player whose interface holds it loses nothing by choosing
64 in Audio input…, and the page says so.

#### Cost

Fastest: 188 ns per sample against 181 before (0.90 % of one core at 48 kHz, from 0.87 %), on
the same machine in the same minute; Steady 183 against 181. That was before the guards of "Why
the early reading can be trusted" (7, 10); with them, measured only on a loaded machine (load 100
to 150, 0.8.0 and now in turn, three times each): Fastest 522 ns against 477 (+9 %), Steady 495
against 486 (+2 %, which is this measure's noise: the tracker alone, the same code in both, moved
by 2.6 %). Fastest is still at most 2.6 % of one core at 48 kHz. `process()` still never
allocates.
Nothing outside the MIDI layer changed: the tracker and the amp are untouched, and 73 renders
(the 49 factory presets, and Trench in Mono, in Poly and in the Sub Octave Riff preset on the synthetic DI
and the seven piezo DIs) are byte-identical to main's.

**Left open:** a low string plucked near the bridge with the fingers still takes two periods
(above); a fret that lands 9 ms or more after the pick is a hammer-on, and now and then gives the
old note for about 10 ms first (5 of 48 such cases, 4 before); the first note after Sedef MIDI
starts still waits 60 ms; a soft pick over a ringing note still waits for the tracker (up to
66 ms on the sloppy lines, 74 before); whether a 64-sample buffer holds on the musician's
interface; a note under 200 Hz named before a whole period of it has come round is right on
every string here, which is measured, not proved ("Why the early reading can be trusted", 5
and 7); and when a piezo's low F♯1 whose partials start together is damped, the tracker sends a
D6 that ends within a millisecond and sounds nothing (0.8.0 does the same; the early reading has
no part in it).
