# Sedef MIDI

**Play MIDI instruments from your guitar.** Sedef MIDI listens to your guitar's DI, one note at a
time, and sends MIDI notes in real time: play a bass line on the guitar and hear it on a bass
instrument, record it as MIDI and edit it afterwards.

Status: **preview**. A test build for macOS comes from CI (see [Test build](#test-build)); it is
not in the installers yet.

<!-- Screenshot: docs/images/midi.png once the UI is final. -->

## What you get

- **Sedef MIDI.app**, a standalone app. It opens your audio interface's input and sends its notes
  through its own MIDI port, **Sedef MIDI**, which every program lists as a MIDI input, like a
  keyboard. On macOS it is a CoreMIDI virtual source; on Linux an ALSA sequencer port.
- **Sedef MIDI** as a plug-in (AU, VST3, CLAP) for the guitar's audio track. Its notes are the
  plug-in's MIDI output, for hosts that route a plug-in's MIDI to instrument tracks.
- **Sedef MIDI (Instrument)**, an instrument AU for Logic, which takes the guitar through its side
  chain (see below).
- Both plug-ins can also send their notes to a MIDI output you pick, or to a virtual port of their
  own, "Sedef MIDI (plug-in)". This is off by default.

## The controls

| Control | What it does |
|---|---|
| **Input meter and Gate** | The meter shows your guitar's level. The gate starts low enough for a quiet interface; drag its handle up only if hum or noise lights the **Signal** ring while you're silent: picks under it never start a note. The ring over the meter fills and lights while you're above it. |
| **Note** | The note being sent, large enough to read from the guitar, with how far your pitch is from it (±50 cents). The last note stays, unlit, after it ends. |
| **Octave** | −2, −1 (default), 0 or +1. A bass sounds an octave under a guitar, so −1 puts a guitar line in a bass's register. |
| **Mode** | *Notes* (default) sends notes only. *Notes + Bend* also sends vibrato and slight bends as pitch bend. |
| **Bend range** | In Notes + Bend: must match the bend range set in your instrument (2 semitones in most). |
| **Timing** | *Fastest* (default) sends each note as soon as it is sure: low strings take longer. *Steady* sends every note 38 ms after its pick, so one track delay of −38 ms puts every recorded note on its pick. |
| **Velocity: Sensitivity, Curve** | How much your picking strength sets the velocity (0 %: every note at 100). The scale follows how you pick on your input, so the same playing gets the same velocities whether your interface is set high or low. *Soft*: a light pick already plays loud; *Hard*: loud notes need a hard pick. |
| **Channel** | The MIDI channel, 1-16. |
| **Guitar audio / Side chain** | *Through* lets the guitar's sound pass; *Mute* silences it. The standalone app and the instrument start muted: they are there for the notes. |
| **Send to / Also send to** | The standalone app: its own port, or a MIDI output. The plug-ins: the host only (default), their own virtual port, or a MIDI output such as an IAC bus. |
| **Panic** | Ends every note on all 16 channels. |
| **Audio input…** | Standalone app only: picks the audio interface, input, sample rate and buffer. |

## Setting up

### A standalone instrument app

1. Open Sedef MIDI.app, choose **Audio input…** and pick your guitar's input.
2. In the instrument app, choose **Sedef MIDI** as its MIDI input (on Windows, the output you picked;
   see below).

### Logic: the standalone app (simplest)

1. Run Sedef MIDI.app as above.
2. In Logic, create a software instrument track with your instrument and record-arm it. Logic takes
   MIDI from every input by default; to take only Sedef MIDI, choose it as the track's MIDI input in
   the inspector.

### Logic: the instrument AU with a side chain

Logic records MIDI from other instrument tracks ("MIDI From", described in Logic Pro's guide under
*Route MIDI internally to software instrument tracks*), and only from tracks whose instrument
plug-in sends MIDI. That is why there is an instrument version of Sedef MIDI:

1. Create a software instrument track and load **AU Instruments > Sedef > Sedef MIDI (Instrument)**.
2. In its plug-in window, set **Side Chain** to your guitar track. The guitar goes to Sedef MIDI.
3. Create a second software instrument track with the instrument you want to hear. Set its input
   to **MIDI From** the Sedef MIDI (Instrument) track, and record-arm it.

### Logic: the effect AU on the guitar track

Insert **Audio Units > Sedef > Sedef MIDI** on the guitar's audio track. Logic does not pass an
audio effect's MIDI to other tracks, so use **Also send to** (below) to reach an instrument track.
If your version of Logic offers the guitar track under **MIDI From**, that works too.

### Any AU or host: through an IAC bus

1. Open **Audio MIDI Setup > Window > Show MIDI Studio**, double-click **IAC Driver** and tick
   **Device is online**.
2. In Sedef MIDI, set **Also send to** to the IAC bus.
3. On an instrument track, choose that IAC bus as the MIDI input.

The plug-ins' own virtual port, **Sedef MIDI (plug-in)**, does the same without the IAC bus; some
sandboxed hosts don't allow plug-ins to create ports, and then the IAC bus is the way.

### Other hosts (VST3, CLAP)

Put Sedef MIDI on the guitar track and route its MIDI output to an instrument track the way your
host does it (often: set the instrument track's MIDI input to the guitar track). Notes reach the
host at their exact sample.

### Windows and Linux

- **Linux:** the standalone app's port is an ALSA sequencer port named Sedef MIDI.
- **Windows:** Windows has had no virtual MIDI ports of its own, so Sedef MIDI sends to an output you
  pick: a loopback MIDI port another app listens to, or a hardware port. Windows MIDI Services, now
  rolling out with Windows updates, brings built-in loopback endpoints (made in its MIDI settings
  app): pick one side as Sedef MIDI's output and the other as the receiving app's input. Sedef MIDI
  uses the classic Windows MIDI interface, through which MIDI Services shows its endpoints to older
  apps; this is not tested yet.

## Playing tips

- **One note at a time.** Damp the strings you aren't playing. A string left ringing under the
  next note is usually fine when the new note is clearly louder; two strings at a similar level
  (a chord, or fingerstyle with strings ringing into each other) give short wrong notes between
  the right ones.
- **Play higher with −2.** A bass line played higher on the neck with the octave at −2 is recognised
  faster than the same line on the low strings with −1 (see the latency table).
- **Any input level works.** Sedef MIDI works the same with your interface's peaks anywhere from
  about −30 to −6 dBFS: the gate's default sits under the softest notes of a quiet input, and
  velocity and note-offs follow your own playing, not the input's level. A very first note
  played before any other can come out softer than the rest.
- **Raise the gate only for hum or noise.** If the **Signal** ring lights while you're silent
  (single-coil hum, a noisy cable), drag the gate just above it. Too high and soft notes go
  missing.
- **Fingerstyle is fine.** Moving the fretting hand to the next note before you pluck it doesn't
  start a note on a string that has died down; a move while the string is still loud can give a
  short one.
- **Steady for recording.** Record in Steady with a −38 ms track delay and every note sits on its
  pick. A note picked softly over a string still ringing can land a little late, and a pick right
  after a mute a little early; damping the previous string keeps them on the pick. For playing
  live, Fastest feels quicker.
- **Velocity.** Your loudest picks of the last few dozen notes set the top of the scale. Keep the
  sensitivity high if you play with dynamics; lower it, or set the curve to Soft, if quiet notes
  come out too soft. A long passage played much softer than the rest slowly comes up to a
  usual velocity.

## How fast is it

From the pick to the note, at 48 kHz, plus your audio buffer (for example 5.3 ms at 256 samples):

| String | E2 | A2 | D3 | G3 and up |
|---|---:|---:|---:|---:|
| Fastest | 28 ms | 22 ms | 17 ms | 16 ms |
| Steady | 38 ms | 38 ms | 38 ms | 38 ms |

A low note needs about two of its periods before its pitch is sure; above A3, Sedef MIDI waits one
period of the low E (16 ms) to rule out a lower note under it. See
[the design](design/guitar-to-midi.md#as-built) for the measurements.

## Test build

CI builds a macOS test build on every run of the `CI` workflow: the artifact
**sedef-midi-test-build-macOS** holds the app, both AUs, the VST3, the CLAP and
[TESTING.md](../tools/packaging/midi/TESTING.md), which explains where each goes, how to open an
unsigned build, and the checklist below.

## Checklist for the first test

Each step says what you should see, and what to send back if you don't. TESTING.md has the details.

1. **Standalone app into an instrument app.** The instrument plays what you play. If not: does the
   meter move, does the note display move, and does "Sedef MIDI" appear among the other app's MIDI
   inputs?
2. **Standalone app into Logic.** An instrument track records MIDI from "Sedef MIDI". If not: what
   Logic's MIDI activity display shows while you play, and the track's MIDI input setting.
3. **Effect AU on the guitar track, instrument track MIDI From.** Tell us whether Logic offers the
   guitar track under MIDI From at all; a screenshot of the menu either way.
4. **Instrument AU with the guitar on its side chain, a second instrument track MIDI From it.**
   The second track plays and records. If not: does the Side Chain menu list the guitar track, does
   Sedef MIDI's meter move, and a screenshot of the MIDI From menu.
5. **Either AU into an IAC bus, an instrument track listening.** The instrument plays. If not: the
   line under **Also send to**.
6. **Feel.** The delay on the low E and A strings in Fastest; then Steady with a −38 ms track delay:
   where do recorded notes land? Then your own bass lines: which notes come out wrong, missing or
   extra, and how you played them. A short recording of the DI with the MIDI it gave helps most.
