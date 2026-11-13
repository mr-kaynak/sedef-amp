# Looper

Status: implemented in `engine/include/sedef/dsp/Looper.h`, for the release after 0.9.0. Owners:
core maintainers.

A looper for practice and writing in the standalone app: record a few bars, play over them,
stack parts on top, take back the last one. One button does the work (record, then play, then
overdub, then play), so it can be driven with one hand on the guitar and a key or a click.

## 1. What a player can do

- Press **Record** (the main button, `L` anywhere, or Space in the looper) and play; press it
  again to close the loop. It plays back at once, seamlessly, from where you pressed.
- With the **click running**, the loop snaps to whole bars: recording starts exactly on the
  next "1", and the press that closes it is rounded to the nearest bar line. An optional
  **one-bar count-in** gives a full bar of clicks first. Once a loop plays, the click is locked
  to it, so they never drift apart.
- Without the click, the loop is as long as you played it (free length).
- Press again to **overdub** a part on top; again to go back to playing. **Feedback** sets how
  much of the old take remains under an overdub (100 %: all of it, 0 %: you replace it).
- **Undo** takes back the last overdub, **Redo** puts it back. **Clear** empties the looper;
  its notice offers Undo.
- **Stop** stops playback (or cancels a recording that has not started); the main button plays
  the loop again from its start, on the next "1" when the click runs.
- **Level** sets how loud the loop plays against what you play live.
- The looper never clicks: every start, stop, punch-in, punch-out and the seam where the loop
  wraps are 10 ms crossfades.

## 2. Where it lives

**The standalone app only.** The engine has the looper, and only the standalone app reserves
its memory and shows its panel, as with the metronome. The plug-in does not offer it:

- A DAW already records, loops and overdubs, sample-exact, and keeps the audio with the
  project. A loop inside a plug-in would be lost when the session reopens: the loop audio
  (up to 92 MB, section 7) cannot travel in a plug-in's saved state.
- Every plug-in instance would reserve the memory, one per track.
- A plug-in loop would ignore the DAW's transport (play, stop, tempo, position), which is
  confusing next to the DAW's own.

The looper sits next to the metronome: a header button opens its panel.

## 3. What it records

The looper records the **processed output** of the rig: after the pedals, the amp, the cab and
the post effects (EQ, Doubler, Drift, Fathom), before the metronome and before the output
gain, in stereo. Its playback is added at the same point.

```
… ─► Fathom ─► Looper (record the live signal, add the loop) ─► Metronome ─► Output gain ─► Out
```

Why there:

- **What you hear is what loops.** The common practice move is to record a rhythm part with
  one sound and solo over it with another. Recording the output keeps the rhythm's sound when
  the preset changes for the solo. Recording the DI and playing it through the rig again would
  turn the rhythm part into the solo sound.
- **The click is never in the loop**: the metronome is added after the looper.
- **Output gain stays the master volume** of both the live playing and the loop, and the
  tuner's mute silences both.
- **It costs a copy, not a second rig**: playback reads memory; it does not run the amp again.

The cost of this choice: a delay or reverb tail that rings past the loop's end is cut at the
seam (crossfaded, section 6) rather than carried into the next pass, as in any looper placed at
the end of a chain.

## 4. States

| State | Means | Main button | Stop | Undo / Redo | Clear |
|---|---|---|---|---|---|
| Empty | no loop | Record | – | Undo a Clear | – |
| Armed | waiting for the "1" to record, or to play | cancel | cancel | – | cancel |
| Recording | the first take | close the loop and play | close and stop | – | discard |
| Closing | recording until the bar line the press was rounded to | – (ignored) | stop when closed | – | discard |
| Playing | the loop plays | overdub | stop | last overdub | clear |
| Overdubbing | playing and adding what you play | play | stop | last overdub (ends the overdub) | clear |
| Stopped | a loop is held, silent | play | – | last overdub | clear |

- **Record** is only possible from Empty: to record a new loop, Clear first.
- **Play from Stopped** starts the loop at its beginning: at once without the click, on the next
  "1" with it (Armed for play).
- Pressing the main button while Armed **cancels** (back to Empty, or to Stopped when it was
  waiting to play). The panel says so.
- While Closing the main button is ignored: the loop closes on its own at the bar line, and
  then plays.

## 5. Timing, in samples

All times are the looper's own sample count (samples processed since it was reset). A press
from the UI acts at the first sample of the next audio block; in the engine's tests a press can
carry the exact sample it acts on. Downbeats are the samples on which the metronome sounds step
0 of a bar: `ceil(anchor + k · interval)`, the metronome's own grid (`Metronome.cpp`), so "on the
downbeat" means on the very sample where the click's first sample sounds.

### Free length (the click is not running)

- Record pressed at sample `p`: the take starts at `p`.
- Closed at sample `q`: the loop is **`L = q − p` samples long**, and loop sample 0 plays at
  `q`. A loop shorter than `Lmin` (below) keeps recording until it is `Lmin` long.
- The longest loop is `Lmax` (section 7); recording closes there by itself and plays.

### Bars (the click is running when Record is pressed)

- **Start.** `d1` is the first downbeat at or after `p − G` (`G`, the grace, below). Without
  count-in the take starts at `s = d1`; with count-in at `s` = the downbeat after `d1`, so a full
  bar of clicks comes first. If `d1 < p` (pressed up to `G` after a "1"), the take starts
  retroactively at that "1": the samples since then are in the looper's input ring.
- **Close.** Pressed at `q`, with `a` the last downbeat at or before `q` and `B` the bar length
  the metronome is counting (`steps × interval`, in samples): if `a > s` and `q − a < B / 2`,
  the press was late and the loop closes at `a`: `L = a − s`, and playback is already at
  position `q − a` when the press acts. Otherwise the loop closes at the next downbeat `c`:
  recording continues to it (Closing), `L = c − s`, and loop sample 0 plays at `c`.
- So **`L` is exactly the number of samples between the two downbeats on the metronome's grid**:
  `L = ceil(A + (j + K)·I) − ceil(A + j·I)` for a take of `K` steps (whole bars) starting at step
  `j`. The tests compute this from the grid and require equality.
- **Lock.** The grid's bar is a fraction of a sample longer or shorter than `L / bars` (an
  interval of 24 615.38… samples at 117 bpm and 48 kHz). Left alone, a loop and the click would
  drift apart by up to one sample per pass: 9 ms after half an hour of a one-bar loop. So while a
  loop recorded in Bars plays and the tempo and meter are those it was recorded at, every wrap
  re-anchors the click: its step 0 sounds on the wrap sample, and the steps inside a pass are
  `ceil(w + k·I)` from it, the same in every pass. If the click was started in the middle of the
  loop, it snaps to the loop at the next wrap. After a tempo or meter change the click runs free.
- If the click stops while Armed, the wait is cancelled. If it stops during a Bars take, the
  next close press closes it free, and a Closing take closes at that sample.

### Constants

| Constant | Value | Why |
|---|---|---|
| `X`, crossfade | 10 ms, `round(0.010·fs)` samples (441, 480, 882, 960) | section 6 |
| `G`, grace after a "1" | 50 ms | Taps meant to land on a beat lead or lag it by 20–80 ms with a spread of about 30 ms (Repp 2005, "Sensorimotor synchronization: a review of the tapping literature", *Psychonomic Bulletin & Review* 12(6)). A press up to 50 ms after the "1" was meant for it; the shortest step the click plays (an eighth at 400 bpm) is 75 ms, so 50 ms never reaches the next beat. |
| `Lmin`, shortest loop | 250 ms | The shortest bar the click plays (2/4 at 400 bpm) is 300 ms; a free loop under 250 ms is a double press. It also keeps the latency compensation (at most 100 ms) and two crossfades inside one loop. |
| `R`, latency compensation | the device's input + output latency + the rig's latency, at most 100 ms | below |

### Latency compensation

The player hears the click (and the loop) `O` samples after the engine produces it (output
latency) and plays along; the guitar reaches the looper `I` samples after it is played (input
latency plus the rig's own latency). Recorded as it arrives, everything the player plays in time
lands `R = O + I` late, and each overdub layer adds another `R`: with a 128-sample buffer that is
6 to 10 ms per layer, a flam after two or three overdubs.

The looper therefore writes the input `R` samples earlier than it plays: the take's sample 0 is
the input at `s + R`, and an overdub writes at the playback position minus `R`. The playback
itself is not delayed. A note played in time with the click (or with the loop) then plays back in
time with it. The same shift is right in free mode: both presses go through the same UI delay,
which cancels, and the take's content moves by `R` like everything else. `R` comes from the
audio device's reported latencies (`AudioIODevice::getInputLatencyInSamples()` and
`getOutputLatencyInSamples()`) plus `Engine::latencySamples()`, Transpose's share included as
it stands in the block where the take or layer starts; a device that reports wrongly leaves the
loop off by its error. `R` is taken when a take or an overdub starts, so a device change (or a
new Transpose setting) never moves a layer half-way.

Writing `R` behind means a take's last `R` samples are written after it closes, while loop sample
0 already plays: the take is complete `R` samples after it closes, and only then is the seam
crossfaded (section 6). An overdub pressed in those samples starts writing when they end.

## 6. Click-free boundaries

Every place where the loop starts, stops, joins itself or changes content is a crossfade of
length `X` with a raised-cosine window `w(n) = ½ (1 − cos(π n / X))`.

**Why 10 ms.** A hard cut between two signals is a step of height `h`, whose spectrum falls as
`h / (2πf)`: broadband, the click. A raised-cosine transition of length `T` multiplies that
spectrum by `|cos(πfT) / (1 − 4f²T²)|`, which falls as `1 / (4f²T²)` above `1/T`. The energy of
the transition above a frequency `fa`, against a hard cut's, is then

```
ρ(T) = ∫ A(f)² / f² df  /  ∫ 1 / f² df   (both from fa to ∞)  ≈  1 / (160 · (fa·T)⁴)
```

Numerically, with the exact `A(f)`:

| `T` | above 250 Hz | above 500 Hz | above 1 kHz | above 2 kHz |
|---|---|---|---|---|
| 2 ms | −6.9 dB | −18.7 dB | −33.0 dB | −45.8 dB |
| 5 ms | −27.2 dB | −38.2 dB | −49.8 dB | −61.9 dB |
| **10 ms** | −38.2 dB | **−49.8 dB** | −62.0 dB | −74.0 dB |
| 20 ms | −49.8 dB | −62.0 dB | −74.0 dB | −86.1 dB |

A guitar's fundamentals and strong low partials sit below 500 Hz and mask what a transition puts
there; above it the ear is most sensitive to a click. We ask for the transition's energy above
500 Hz to be 50 dB under a hard cut's: 10 ms. Shorter fades click on a quiet passage (5 ms is
only 38 dB down there); longer ones soften a pick attack that falls on a punch-in for longer and
shave more off the loop's end. 10 ms is also the engine's bypass fade, so every switch in Sedef
sounds alike.

**Where:**

- **The seam.** When the take is complete, its last `X` samples crossfade into the `X` samples
  the looper heard just *before* the take began (kept in its input ring). Those flow, sample for
  sample, into loop sample 0, so the wrap continues the way the playing did when it was recorded.
  The loop's first note keeps its attack untouched, which matters most on the downbeat; a tail
  ringing at the end fades into what preceded the start (usually near silence) over 10 ms.
  Rejected: fading the start in from the samples after the end, which keeps the end but softens
  the downbeat's attack for 10 ms on every pass.
- **Punch-in and punch-out.** The overdub's write gain ramps 0 → 1 and 1 → 0 over `X`, and with
  it the feedback: a sample becomes `old · (1 − d·(1 − fb)) + in · d` for a ramp value `d`.
- **Play and stop.** Playback fades in over `X` whenever it starts from silence, and fades out
  over `X` on Stop and Clear. That includes the first pass after a take: nothing of the loop
  played before it, so loop sample 0 would otherwise be a step from silence (a note ringing
  across the "1" puts it anywhere). Only that first pass is softened, while the player is
  usually still playing over it; from the second pass on the wrap is the seam.
- **Undo and redo while playing**: the old and the new content crossfade over `X` at the
  playhead.
- A command that arrives during one of these fades (a second press within 10 ms) takes effect
  when it ends.

**Bound (tested).** On a sine of amplitude `A`, any crossfade between two pieces of it can add at
most `max |w′| · |Δ| ≤ (π / 2X) · 2A = πA / X` to one sample-to-sample step, against the sine's
own `2πfA / fs`. The tests measure the largest step across the seam, a punch-in and out, a stop,
an undo and a redo, and require it to stay under the sine's own plus `πA / X` (a hard cut adds up
to `2A`).

## 7. Memory

All loop memory is reserved off the audio thread. Each frame of loop is stereo `float`, and undo
keeps a second copy of whatever the last overdub changed (section 8), so the looper needs
`2 ch × 4 B × 2 = 16 B` per frame of its longest loop.

**The budget: 120 s at 48 kHz, `Lmax` = 5 760 000 frames, 87.9 MiB (92.2 MB).** Long enough for a
32-bar 4/4 section at 64 bpm, at the rates nearly every interface runs at, in under 1 % of an
8 GB machine. The frames are fixed, so the length in seconds follows the rate:

| Rate | Longest loop | Reserved |
|---|---|---|
| 44.1 kHz | 130.6 s | 87.9 MiB |
| 48 kHz | 120.0 s | 87.9 MiB |
| 88.2 kHz | 65.3 s | 87.9 MiB |
| 96 kHz | 60.0 s | 87.9 MiB |
| 176.4 kHz | 32.7 s | 87.9 MiB |
| 192 kHz | 30.0 s | 87.9 MiB |

At 96 kHz a minute still holds 16 bars of 4/4 at 64 bpm. Small parts: the input ring
(`G + X` = 60 ms, sized for 192 kHz: 11 520 frames, 90 KiB) and the chunk tables (25 KiB).

**When.** The memory is reserved the first time the looper is opened (its panel, its key) in a
standalone session, on the message thread: zeroing 92 MB takes 10–40 ms there, and the audio
thread picks the finished memory up through one atomic pointer at the next block. A standalone
app that never loops reserves nothing; the plug-in never does. The memory stays until the app
quits.

**Sample-rate changes** clear the loop (a loop recorded at 48 kHz would play at the wrong pitch
at 44.1). A new buffer size or the Latency setting prepares the engine again at the same rate:
the loop stops and is kept.

## 8. Undo

Undo restores the loop exactly as it was before the last overdub, bit for bit, and Redo exactly
as it was after. Overdubbing works in place on the loop; the question is where the "before"
lives and what restoring it costs on the audio thread.

| Scheme | Undo | Starting the next layer | Memory | Verdict |
|---|---|---|---|---|
| A second buffer, copied at each punch-in | swap | copy the whole loop: 46 MB in one block, 5–10 ms, against a 2.7 ms block (128 at 48 kHz) | 2× | rejected: not real time |
| Save what the layer overwrites, redirect reads while undone | O(1) | after Undo then a new overdub, the undone stretch must be copied back: up to the whole loop, or a growing list of pending stretches | 2× | rejected: unbounded |
| Store the layer as a difference | O(1) | add the difference into the loop: the whole loop again | 2× | rejected |
| **Copy-on-write chunks** | swap two tables | copy one table | 2× | **chosen** |

**Chunks.** The loop is a table of chunk numbers, one per 4096 frames (`Lmax` needs 1407), into
a pool of `2 × 1407 = 2814` chunks. Two tables exist: *live* (what plays) and *other* (the state
before the last overdub, or after it once undone, or the loop a Clear removed). Each chunk counts
the tables that use it (0, 1 or 2).

- **A new overdub layer** releases *other* and copies *live* into it: 1407 entries, about a
  microsecond. Every chunk is now shared.
- **Writing** into a shared chunk first copies it to a free chunk (32 KiB, a few microseconds,
  once per 4096 frames written, about 85 ms of overdub at 48 kHz), so *other* keeps the old
  samples untouched.
- **Undo and Redo** swap the two tables. **Clear** moves *live* into *other* and leaves *live*
  empty; Undo brings it back. Recording a new loop releases *other*, so a Clear can be undone until
  the next take.
- **The pool never runs out:** at most two tables of at most 1407 chunks each are referenced, and
  the pool has 2814. Copy-on-write only happens on a shared chunk, which counts once for two
  tables, so a free chunk always exists. No failure path.

Chunk size: 4096 frames keeps the table work per layer at 1407 entries and a copy at 32 KiB.
At 1024 frames the copies would be 8 KiB but the tables 5628 entries; either fits the block
budget, and 4096 makes the chunk index a shift.

## 9. Real time ([ADR-0005](adr/0005-real-time-safety.md))

- The audio thread never allocates: the pool, the ring and the tables are reserved before the
  looper runs (section 7). The engine's allocation test runs every looper command under the
  counting allocator.
- **Commands** (main, stop, undo, redo, clear) go from the message thread to the audio thread
  through a lock-free single-producer queue, read at the start of each block.
- **State for the UI** (state, flags, length, position, bars) is published through atomics in
  `Engine::Telemetry`, read by the editor's 30 Hz telemetry.
- **The metronome's downbeats** come from `Metronome::downbeats()`, which runs the click's own
  scheduler forward on a copy of its state, so the looper and the click agree on every sample.
- Cost: one stereo copy into the ring per sample while the looper exists, plus a read per sample
  while it plays and a read-modify-write while it overdubs.

## 10. Parameters and messages ([ADR-0004](adr/0004-single-parameter-schema.md))

Settings are parameters, appended to `ParameterList.inc`, not automatable and, like the
metronome's, global: presets never store or load them, and they are kept with the session.

| Id | Range | Default | |
|---|---|---|---|
| `looperLevel` | −30 … +6 dB | 0 dB | how loud the loop plays; smoothed over 20 ms |
| `looperFeedback` | 0 … 100 % | 100 % | how much of the old take stays under an overdub; smoothed over 20 ms |
| `looperCountIn` | off/on | off | a one-bar count-in before a take, when the click runs |

Transport actions are messages (as the metronome's pattern and tap are), never parameters: a
press is an event, not a value to automate or restore. Bridge:

- `sedef_looper_open()`: reserves the memory (section 7); answers whether the looper is ready.
- `sedef_looper_command(action)`: `'main' | 'stop' | 'undo' | 'redo' | 'clear'`. The engine
  decides what `main` means from its own state, so a UI a few milliseconds behind can't send the
  wrong action.
- `TelemetryEvent.looper` (standalone only): state, whether a loop is closing or armed for play,
  length and position in samples, bars (0 for a free loop), bars still to wait, and whether Undo,
  Redo or "Undo clear" is possible.

## 11. UI

A **Looper** button in the header, next to the metronome's, opens the looper's panel. The button
carries the looper's state when the panel is closed: a ring that fills with the loop's position,
red while recording, nacre while playing, amber while overdubbing.

The panel, top to bottom:

- **The main button**, large, with the loop's progress ring around it and the state written
  beside it: "Recording" (red), "Playing", "Overdubbing" (amber), "Stopped", "Waiting for the
  1", "Closing at the bar line". The button names what it will do next: Record, Play,
  Overdub, Cancel.
- The loop's **length** (bars and seconds when synced, seconds when free) and its position.
- **Stop**, **Undo**, **Redo**, **Clear** (Clear shows "Loop cleared · Undo").
- **Level** and **Feedback** knobs; **Count-in** switch, with a note that loops snap to bars
  while the click runs.

Keys: `L` anywhere (outside text fields) is the main button, `Shift+L` is Stop; inside the panel
Space is the main button too. The panel follows [ui.md](ui.md): the shared buttons and knobs,
visible focus, accessible names, a polite live region that announces state changes
("Recording", "Playing, 4 bars"), and under reduced motion the ring steps instead of gliding.

## 12. Tests

Engine (`engine/tests/looper/`):

- State machine, sample-exact: free start and close offsets; Bars start on the downbeat, with and
  without count-in, with the grace; close rounding (early, late, first bar), `L` equal to the
  metronome grid's distance; Closing; cancel; Stop during a take; minimum and maximum length.
- Latency compensation: a click-aligned input lands at loop sample 0; overdubs stay aligned.
- Crossfade continuity bound (section 6) at the seam, punch-in/out, stop, play, undo and redo.
- Overdub feedback; Undo, Redo and Clear restore bit-exact loops, also after several layers and
  "undo then overdub".
- Block-size invariance (1, 32, 64, 128, 512, 4096, 97, 441) and rates 44.1/48/88.2/96 kHz: the
  same events at the same samples give bit-identical output and loops.
- The lock: after many passes the click and the loop wrap on the same sample.
- The engine: records the post-effects signal without the click, commands act at the next block,
  no allocation while every command runs, prepare at the same rate keeps the loop.
- Factory presets render byte-identical to main with the looper idle (`sedef-render`, every
  preset at 44.1, 48 and 96 kHz and in Linear phase).

UI (Vitest): the panel in each state, its buttons and keys, the header button's state, the
shortcuts, and the mock's looper.

## 13. Known limits

- The loop is not saved: closing the app loses it. (Export is a later feature.)
- Recording the output means a reverb or delay tail past the loop's end is cut at the seam.
- A tempo change after recording does not stretch the loop; the click then runs free.
- One level of undo (the last overdub).
