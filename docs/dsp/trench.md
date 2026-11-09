# Trench (octave down)

`engine/include/sedef/dsp/Trench.h`, `engine/src/dsp/Trench.cpp`, with
`engine/include/sedef/analysis/MonoTracker.h` (Mono) and
`engine/include/sedef/dsp/detail/PolySub.h` (Poly)

## What it models

An octave-down pedal with two modes, picked with the Mono/Poly switch (`trenchMode`):

- **Mono** is the analog monophonic octave divider: the note's fundamental is isolated, a
  comparator with hysteresis turns it into one edge per period, and two flip-flops divide the
  edges by two and by four. Their square waves, one and two octaves below the note, are shaped by
  the input's envelope and blended with the dry signal. It plays one note at a time, as the
  circuit does, with the circuit's thick, synth-like square sub.
- **Poly** divides every partial of the input, so chords, double stops and ringing strings all
  get their octave down. Its subs are the input's own spectrum at half (and a quarter of) its
  frequencies: rounder than Mono's squares, closer to an octave-down string.

Neither mode shifts pitch with a delay line: the removed Transpose shifter (see
[transpose.md](transpose.md)) is not used, and the direct path has no latency.

Mono is the default, so sessions and presets saved before the switch existed sound as they were
voiced (the Benthic preset, for one, is written for single-note riffs).

## Signal flow

```
in ─┬──────────────────────────────────────────────────────────── × direct ─────────────────┐
    ├─► HP 45 Hz ─► LP 1.1 kHz ─► mean |x| (10 ms) ─► gate (floor +12 / +6 dB) ──┐          │
    │                                                                            │          │
    ├─► Mono: MonoTracker ─► analytic fundamental ─► phase ÷2 ─► Schmitt trigger │          │
    │         ─► flip-flop ÷2 ─► square −1 ─┐ polyBLEP edges                      │          │
    │            flip-flop ÷2 ─► square −2 ─┴─► × level·π/2 × gate (note held) ─┐ │          │
    │                                                                         │ │          │
    └─► Poly: PolySub (filter bank at ~4 kHz) ─► sub −1, sub −2 ─► × gate ─────┤◄┘          │
                                                                              ▼            │
                     × (sub1, sub2), Mono/Poly crossfade ─► tone LP ─► HP 10 Hz ───────────+─► out
```

## Mono: the note tracker

`analysis::MonoTracker` is a reusable real-time component (it is also meant for a future
guitar-to-MIDI feature). It answers three questions every sample: is a note sounding, which, and
what is its fundamental as an analytic (complex) signal.

- **Period estimator.** The input is high-passed at 40 Hz, low-passed at 1.5 kHz (4th-order
  Butterworth) and decimated to 7–8 kHz. A normalised square difference function (NSDF, McLeod
  and Wyvill's MPM) is kept up to date incrementally for every lag, each lag over a window of one
  candidate period (at least 4 ms), so a period can be judged on two of its lengths of signal.
  Every 0.5 ms the key maxima are picked as in MPM (the first within 90 % of the highest), with
  parabolic interpolation, and two guards: a peak at twice or three times the chosen lag that is
  3 % more periodic wins (strong 2nd and 3rd partials make the classic octave-high error), and a
  "virtual fundamental" is refused: a single-bin DFT over two periods must find real energy at the
  candidate (an open fifth repeats at a period neither string plays).
- **Picks.** A pick is where the 2 ms power of the low band, or of a 2 kHz click band, jumps four
  times over its 40 ms power. It is dated back to where the rise began. For two periods (at least
  8 ms) after a pick nothing is judged: the attack's step leaks energy into every lag and hides a
  virtual fundamental, and its first periods are short.
- **Note logic, with hysteresis.** A note locks once two estimates in a row agree and are
  periodic enough (NSDF peak ≥ 0.8; ≥ 0.7 right after a pick, when the old string still rings
  under the new one). While locked it follows moves of up to a semitone per hop (vibrato, bends,
  glides), survives up to 30 ms of poor estimates (pick noise, a ringing neighbour), and changes
  note only when the new period has been seen consistently: 2 ms for a new note, 8 ms for an
  octave jump. The held note has a continuity prior: a challenger must beat it by
  max (0.05, 0.4 × (1 − held clarity)), and the held note must still have its fundamental.
- **Fundamental.** Two cascaded complex one-pole resonators, steered to the note, pass its
  fundamental: 0.5 × f wide after a lock or a change of note, narrowing to 0.2 × f over three
  periods (a fast lock, then a neighbouring string and the 2nd partial far down). The steering has
  a 35-cent dead band, so a wobbling estimate never sweeps the filter's phase. The partial's
  frequency is measured on the resonator output (its phase turn per sample, smoothed over four
  periods) and reported, but never fed back: a frequency-locked loop around a narrow resonator
  was unstable.
- **For a note-to-MIDI layer** (nothing MIDI is built yet), every push() also says what changed:
  `event()` is a note-on (a note locked), a note change (a different note took over) or a
  note-off, at the sample that returns it; `samplesSincePick()` dates the pick behind it (the
  rise of its onset); `pickLevel()` is the pick's peak 2 ms RMS over its first 10 ms, a velocity
  source; `hz()` is the continuous pitch (for bends and vibrato) and `clarity()` the confidence. A
  legato step may come as a glide of `hz()` rather than a note change.

### Why the first tracker chopped

The 0.4.0 tracker took its period from Schmitt-trigger edges on a tracking low-pass whose corner
moved only after three periods agreed within 8 %, and rested at 200 Hz without a note. On real
strings the 2nd and 3rd partials are often as strong as the fundamental: the low-pass let them
through, the edges followed them, consecutive periods disagreed, confidence halved on each, and
the gate (which followed confidence, 4 ms up and 15 ms down) chopped the sub 12–20 times a second.
Its tests only used idealised plucks. The new tests (`engine/tests/pedals/OctaverDi.h`) render
what a guitar does: bridge and piezo pickups with the 2nd and 3rd partials 4–8 dB over the
fundamental, loud picks, the previous note ringing a fourth below or a fifth above, vibrato, a
whole-step bend, palm-muted sixteenths and legato runs. The old tracker fails 72 of their checks.

## Mono: the divider

- **Half phase.** The tracker's analytic fundamental u is followed sample by sample: the half
  angle w turns by half of each step, `w ← w · √(u · conj(w)²)` with the principal root, so it
  is continuous and exactly half the fundamental's phase.
- **Schmitt trigger** on cos (half phase) with ±0.25 hysteresis: one edge per period of the note;
  phase wiggles up to asin (0.25) around a crossing cannot toggle twice. The edge time is
  interpolated between samples.
- **Flip-flops**: sub −1 follows the trigger (÷2), sub −2 toggles on its falling edges (÷4). Each
  toggle is band-limited with a polyBLEP residual split between the sample before the edge (held
  back one sample) and the one after, so the squares need no oversampling.
- **Envelope and gate**: the subs are `square × level × π/2`, `level` being the band-limited
  input's mean |x| over 10 ms (π/2 makes it the amplitude of a sine), times a gate. The gate's
  target is "the level gate is open (see "The level gate") and a note is held"; it rises in 4 ms
  and falls in 15 ms. So the sub is silent on silence, hiss and hum, fades after a note stops,
  and never holds a stuck note.

## Poly: one voice per partial

`detail::PolySub` divides every partial it can resolve.

- **Resampling.** A 6th-order Butterworth low-pass at 1.6 kHz, then decimation to about 4 kHz
  (fs / round (fs / 4000)). The voices come back by linear interpolation and a 4th-order
  Butterworth low-pass at 1.5 kHz. Together about 1 ms of delay on the sub path.
- **Filter bank.** Complex band-pass filters, each three cascaded one-pole resonators: 14 Hz wide
  (−3 dB, all three poles) from 50 Hz, then a 60th of the centre frequency above 840 Hz, up to
  1.2 kHz: 79 bands. The first pole carries a zero at −f_c, so a band's output is the analytic
  signal of the partial near its centre without its negative-frequency image.
- **One band per partial.** A partial is a peak of the bank's power across bands, and only the
  band at the peak (its owner) is voiced. An owner keeps its partial until a neighbour is 3 dB
  stronger, then hands its phase and fade over: one partial stays one continuous voice through
  vibrato, bends and beats. Owners closer than the (widened) bandwidth merge into the stronger.
  A new partial is a strict peak, away from the bank's edges and from other owners, no more than
  50 dB under the strongest band (and above −94 dBFS). Voices fade in and out over 3 ms. Bands
  without a voice copy the phase of their stronger neighbour, so a new owner starts in step with
  the partial's previous one.
- **Phase division.** An owner's phase step from one bank sample to the next is halved (and
  halved again) with the principal square root, and accumulated into its sub's phase. The level
  is the band's magnitude divided by the band's response at the partial's frequency (the step's
  angle), at most +1.9 dB, times two (an analytic band carries half a real partial).
- **Resolution on demand.** The narrow bands resolve the partials of a chord, but three poles
  14 Hz wide take about 25 ms to speak. A pick (the 2 ms power four times over the 40 ms power),
  and a sudden drop (the 2 ms power 24 dB under it: a palm mute), widen every band four times;
  they narrow back with a 30 ms time constant. The detector rests 50 ms after each detection: the
  2 ms power ripples with a low note's waveform and the 40 ms power lags behind the pick.
- **Level gate.** Poly shares Mono's level detector and level gate (4 ms up, 15 ms down), so
  hiss and hum are not divided. The subs otherwise carry the input's own envelope. The bank's own
  floors (a band's power, the pick detector's power) are numerical only, at −180 dBFS: what is
  noise is the level gate's call.

### Why not halve every band

The literature on low-latency filter-bank pitch shifting (Juillerat et al.'s constant-Q bank,
Thuillier's octave doubler) translates or phase-scales each band. For an octave up, scaling the
phase is exact. For an octave down it is not: halving a phase has two answers, w and −w, and
each band picks one on its own. A partial seen by two bands then comes out of them in or out of
phase, and the sum warbles or cancels; two partials in one band turn into intermodulation
(products at f_b − f_a/2 and 3f_a/2 − f_b). We built that design first, with sign alignment
between neighbouring bands: on the chords below, its note subs came out 10–25 dB under an ideal
octave down, its strongest off-grid peaks reached 17 dB over the weakest note's sub, and its level
warbled by up to 5.3 dB. Alignment chattered (about 90 flips a second per band) and did not help.
One owner per partial removes the sign question (there is only one root to follow), and peak
picking with handover is the identity phase locking of the phase-vocoder literature, run on a
filter bank instead of an FFT.

The physical limit stays: two partials closer than the bank's resolution are one partial to it.
At 14 Hz the bank separates the low strings of a chord (the closest distinct low partials of an
open E are about 40 Hz apart). Partials closer than that (an equal-tempered major third's 2nd
partial against the root's 5th, 3 Hz apart) beat as they do on the strings, and their octave down
beats at a slightly different rate, which puts weak sidebands next to the note.

## The level gate

Both modes share it. It keeps hiss and hum from being divided. Until 0.5 it was a fixed level:
open above −52 dBFS mean |x| of the detector band (45 Hz–1.1 kHz), shut below −58 dBFS. A player
whose interface is set low never got past it. On a fingerstyle DI peaking at −30.6 dBFS the
detector level's median is −61 dBFS, and the sub spoke on a fifth of the notes (measurements
below). Raising Input Gain fixed it, but nothing told the player so.

The gate is now set against the input's own noise floor. An interface's gain moves its noise and
the playing together, so a gate relative to the floor works the same at any input level.

- **Open and close.** The gate opens when the level is 12 dB over the floor and closes when it
  falls to 6 dB over it. The 10 ms level of band-limited noise stays within about 5 dB of a floor
  that follows its dips, so noise alone neither opens the gate nor holds it open.
- **The floor.** It follows the level down at once, because the dips between notes are the noise.
  It climbs by 6 dB a second only while the gate is shut, or while the gate is open but nothing
  has been picked for 2 s. (A pick is the 10 ms level jumping over twice its 100 ms mean.) That
  lets go of a hum that came up while the gate was open. A player re-picks well within 2 s, so
  held notes and continuous playing never lift the floor into the music.
- **After a reset** (and when the plug-in starts), the first 30 ms set the floor, and nothing is
  divided before then. That is three time constants of the level. Hiss or hum present from the
  start is never divided, not even for a moment.
- **Its range.** The floor stays between −112 and −64 dBFS mean |x|.
  - At the bottom, the gate opens from −100 dBFS. That is 10 dB over the band-limited noise of a
    quiet 24-bit interface (−110 dBFS and below), and 50 dB under the quietest playing the tests
    use (−40 dBFS peaks).
  - At the top, it asks no more than the fixed gate did (open at −52, close at −58 dBFS). A
    "floor" louder than that is played signal, not noise: a steady test tone, for example.

Mono's tracker still has to name a note before its sub speaks, and its voicing and clarity
reject hiss. Poly divides whatever passes the gate. The gate's numbers come from the statistics
of noise and from interface noise, not from one recording. The same rules give the same results
from −40 to −6 dBFS peaks (measurements below).

## Control mapping

| Control | Mapping |
|---|---|
| `direct` | Dry gain 2 × direct (linear): unity at noon, +6 dB at 10. At noon with both subs at 0 the output is a bit-exact copy of the input, in both modes. |
| `sub1`, `sub2` | Sub gains 2 × 0.95 × knob: at noon a sustained note's sub is within 2 dB of the note's level in Mono, within 1 dB in Poly. |
| `tone` | Sub low-pass corner 150 Hz × 20^tone (Q 0.6), then a 10 Hz DC block. |
| `trenchMode` | Mono or Poly. A change crossfades the two over 20 ms; the path that has faded out is reset and stops costing CPU. |

All gains ramp per sample over 30 ms; the tone corner follows a 20 ms smoother and is redesigned
every sample while it moves (Poly's smooth subs showed the steps a once-per-block update left).
`latencySamples()` is 0: the direct path is not delayed, and the subs' own delay is part of the
effect, like an analog divider's.

## Measurements

All at 48 kHz unless stated. Lock: from the pick to the sub's gate past 0.5 on the right note
(Mono), or to the sub reaching half its settled level, behind an ideal octave down (Poly).

### Mono on synthetic playing (`sedef_engine_tests "Trench measurements"`)

| scenario | E2 | A2 | E3 | A3 | E4 | E5 |
|---|---|---|---|---|---|---|
| lock, bridge / piezo / loud pick (ms) | 28.3 | 22.3 | 16.3 | 13.0–13.3 | 11.3 | 11.3 |
| lock, 0.4.0 tracker (ms) | never | 30–39 | 28 | 23–27 | 20–23 | 22–45 |
| lock over a ringing neighbour (ms) | 26.0–26.7 | 20.7 | 14.7 | 11.7 | | |

- After the lock, on every scenario (three pickups, ringing neighbours a fourth below and a fifth
  above, vibrato, a whole-step bend): no gate flips, the sub at 90 % of its settled level for
  99.2–100 % of the note, its level modulation 0.2–1.3 dB, both subs within 1.2 cents of f/2 and
  f/4, every sub period in the right octave. The 0.4.0 tracker never locked on E2 with any of the
  three pickups, on any ringing-neighbour case, or on E2 vibrato and bends.
- Palm-muted sixteenths at 140 bpm: first lock 28.3 ms (E2) and 22.3 ms (A2), then no gate flips
  and every chug's sub within 1.3 dB of the others (0.4.0: 32 and 18 flips, 2.7 dB).
- Legato runs (hammer-ons a whole step apart, 150 ms each): every step re-locks within 6.7–12 ms.
- Lock is about two periods of the note plus the gate's attack; on E4 and E5 the 8 ms attack
  hold dominates.
- Steady notes E2–E5: subs within 0.005 cents, onset peak at most 1.09 × the settled peak, the
  sub at noon −1.8 to +1.4 dB against the note. An open E major strum: the sub follows the
  strum's root (the other strings are its harmonics), peaking at 0.35 against 0.39 for a steady
  E3.

### Mono and Poly on real DI, against 0.4.0

Trench alone, Direct 0, Sub −1 0.5, Tone 0.5; notes are where the input's 10 ms RMS is above
−40 dBFS. "Sub present": the sub's 50 ms level within 12 dB of the input's. "Dropouts": the
sub's 50 ms level falling 10 dB under its own peak over the previous 100 ms, per second of
notes. The piezo recordings are public-domain DI from IEM Graz, fed 7 dB hotter (as for the
amp); the riff and the strums are synthetic DI.

| input | 0.4.0 gate on / flips | Mono gate on / flips | sub present 0.4.0 / Mono / Poly | dropouts 0.4.0 / Mono / Poly |
|---|---|---|---|---|
| piezo DI, fingerstyle (33 s) | 69 % / 19.6 per s | 97 % / 1.3 per s | 94 / 99 / 100 % | 1.8 / 0.3 / 0.4 per s |
| piezo DI, modal phrases (15 s) | 34 % / 17.0 per s | 94 % / 3.7 per s | 73 / 98 / 100 % | 4.0 / 2.4 / 2.1 per s |
| synthetic low riff (64 picks) | 14 % / 7.1 per s | 55 % / 1.7 per s | 19 / 57 / 94 % | 4.7 / 2.3 / 0.7 per s |
| synthetic strummed chords (24) | 16 % / 9.7 per s | 25 % / 1.7 per s | 32 / 28 / 100 % | 2.6 / 0.9 / 0.0 per s |

Lock per pick on the riff and the strums (to half the sub's settled level): Poly 8.8 ms median
and 12.3 ms at the 90th percentile on the riff, 3.3 and 13.2 ms on the strums, with a sub on
every pick. Mono leaves 23 of the riff's 64 picks and 15 of the 24 strums without a sub: the
chords it cannot follow.

On the single-note stretches of the fingerstyle recording, Mono's sub has no gate flips and
stays at 90 % of its level all the time (0.4.0: 3.4 flips per note, 64 % of the time). On the
modal phrases about a fifth of the frames are read an octave low: the guitar body's resonance
near 100 Hz gives low notes a strong component an octave under the string (a known limit,
below).

### At any input level

The suites above with the input's peaks at −40, −30, −18 and −6 dBFS (a scratch harness over the
same scenarios; `"Trench works the same at any input level"` checks a subset in ctest). The
fixed gate of 0.4 and 0.5's gate against the noise floor:

| peak | gate | Mono flips | held (min) | lock E2–E3 / A3–E5 (max) | legato lock | Poly chords off-grid / strongest peak | Poly warble on a bend |
|---|---|---|---|---|---|---|---|
| −40 dBFS | fixed | 8 | 0 % | never / never | never | +5.8 / −24.1 dB | 4.1 dB |
| −30 dBFS | fixed | 1 | 0 % | 28.3 ms / never | 12.0 ms | −23.8 / −25.2 dB | 1.7 dB |
| −18 dBFS | fixed | 0 | 99.2 % | 28.3 / 13.3 ms | 12.0 ms | −23.8 / −25.2 dB | 1.7 dB |
| −6 dBFS | fixed | 0 | 99.2 % | 28.3 / 13.3 ms | 12.0 ms | −23.9 / −25.2 dB | 1.7 dB |
| −40 to −6 dBFS | floor | 0 | 99.2 % | 28.3 / 13.3 ms | 12.0 ms | −23.9 / −25.2 dB | 1.7 dB |

With the floor, every number is the same at every level, and equal to the fixed gate's at
−6 dBFS. (The worst note cents on the chords read 1.27 instead of 1.25.)

A player's own dry DI, recorded with the interface gain low: fingerstyle, tuned a whole step
down. One file runs up each string (peaks −30.6 dBFS, noise floor near −101 dBFS); the other has
13 strummed chords (peaks −27.1 dBFS). Trench alone, measured as on the real DI above, as recorded
and with gain to −6 dBFS peaks:

| input | gate | Mono gate on / flips | sub present Mono / Poly | dropouts Mono / Poly |
|---|---|---|---|---|
| runs, as recorded | fixed | 18 % / 0.9 per s | 20 / 20 % | 0.7 / 0.7 per s |
| runs, as recorded | floor | 100 % / 0.4 per s | 100 / 100 % | 0.2 / 0.1 per s |
| runs, −6 dBFS | fixed | 99 % / 0.6 per s | 100 / 100 % | 0.2 / 0.1 per s |
| runs, −6 dBFS | floor | 100 % / 0.5 per s | 100 / 100 % | 0.2 / 0.1 per s |
| chords, as recorded | fixed | 35 % / 1.7 per s | 40 / 51 % | 1.8 / 1.3 per s |
| chords, as recorded | floor | 79 % / 2.8 per s | 84 / 100 % | 2.9 / 0.0 per s |
| chords, −6 dBFS | fixed | 79 % / 3.1 per s | 84 / 100 % | 2.9 / 0.0 per s |
| chords, −6 dBFS | floor | 79 % / 3.0 per s | 84 / 100 % | 2.9 / 0.0 per s |

Mono on the chords stays at 84 % because it follows one note (see the limitations). Poly's
intermodulation on the strums, 0.25–1.25 s after each one, is now the same at both levels. Its
strongest off-grid peak sits a median 22.6 dB under the strongest note (fixed gate as recorded:
16.4 dB). Against the weakest note the median is −11.7 dB (fixed gate: −4.2 dB as recorded,
−11.4 dB at −6 dBFS). The worst strum reads +10.2 dB against its weakest note, against +4.6 dB
with the fixed gate at −6 dBFS. That is not a louder product: the strum at 5.3 s now has all six
notes' subs where the fixed gate's run missed one, so its weakest note is a quieter one. Against
the strongest note, every strum is within 2 dB of the fixed gate's run.

### Poly (`sedef_engine_tests "PolySub measurements"`)

Each string alone, against an ideal octave down (every partial at half its frequency, same
amplitude and decay):

| string | E2 82.4 | A2 110 | D3 146.8 | G3 196 | B3 246.9 | E4 329.6 |
|---|---|---|---|---|---|---|
| lag at 44.1 kHz (ms) | 9.4 | 9.8 | 8.2 | 7.1 | 6.8 | 8.2 |
| lag at 48 kHz (ms) | 11.9 | 10.8 | 8.5 | 7.1 | 8.6 | 7.8 |
| lag at 96 kHz (ms) | 10.8 | 11.0 | 8.5 | 7.0 | 7.4 | 9.5 |
| lag at 192 kHz (ms) | 10.9 | 10.6 | 8.5 | 10.8 | 8.5 | 8.0 |
| level (dB) | +0.2 | −0.1 | −0.3 | −0.6 | −0.4 | −0.9 |

Both subs land within 0.25 cents of f/2 and f/4. Chords, 0.45–1.45 s after the strum:

| chord | notes an octave down | off-grid power | strongest off-grid peak, re strongest / weakest note | warble |
|---|---|---|---|---|
| E major, open | within 1.3 cents | −24.4 dB | −29.1 / −19.7 dB | 0.9 dB |
| A major, open | within 0.8 cents | −25.8 dB | −29.6 / −15.6 dB | 1.6 dB |
| E5 power chord | within 0.1 cents | −24.1 dB | −25.2 / −19.3 dB | 0.7 dB |
| A5 power chord | within 0.1 cents | −28.2 dB | −28.9 / −23.5 dB | 1.4 dB |
| F major barre | within 0.7 cents | −23.9 dB | −26.4 / −16.7 dB | 1.5 dB |

"Off-grid" is everything not within 10 cents (or 2 Hz) of a partial of the ideal octave down;
the ideal itself measures −33 dB on this grid (window leakage). Warble: peak-to-trough of the
50 ms level around its 300 ms mean (the ideal octave down: 0.5–1.6 dB, from the strings' own
beating). A damped note or chord is 60 dB down 58–66 ms after the mute.

### The 20 dB intermodulation goal

The goal was every intermodulation product at least 20 dB under the chord's notes, the weakest
note included. Against the strongest note every chord makes it, by 5–10 dB. Against the weakest,
A5 makes it with 3.5 dB to spare, E major and E5 miss by under 1 dB, and A major and F major miss
by 4.4 and 3.3 dB.

- **E major and E5** (worst products at 144 and 103 Hz). Two partials about 41 Hz apart (329.6
  against 370.4 Hz, 164.8 against 123.5 Hz): the stronger one leaks through the skirt of its
  neighbour's band.
- **A major and F major** (546 and 434 Hz). Two partials 7–9 Hz apart (a string's 10th partial
  against another's 4th: 1100 against 1108.7 Hz, 873 against 880 Hz). Telling them apart takes a
  window of more than 100 ms, which no playable delay allows.

What we tried, on the same chords:

- **Narrowing a held chord's bands.** Taking them to half their width over 150 ms, and back to
  full width on each pick, brings E major to −22.1 dB and E5 to −24.6 dB. But a whole-step bend
  then sweeps the partials across the narrow bands, and the sub's level warbles by 3.4–5.3 dB
  (1.3–1.7 dB without the narrowing), over the 3 dB warble goal. At 0.6–0.7 of the width the
  warble is 2.6–3.4 dB, and F major gets worse (−13 dB).
- **Narrowing only while no partial changes owner.** On a chord the owners keep handing over, so
  the bands stayed wide.
- **Weighting owner bands by level.** This cannot help. A product's level follows the leaking
  neighbour, so any gain on the owner band scales the product and the note together.

The design stays as it is. Through a clean amp the products are real: they sit 7–60 dB above
what a perfect octave down puts at the same frequencies. Each one also sits under a
conservative masking threshold, taken from the perfect octave's spectrum (tonal maskers,
Schroeder spreading, a 14.5 + z dB offset). The closest are E5's weak products near 550 Hz,
where little else plays: 2.5 dB under. E5's loudest product, at 103 Hz, is 8 dB under. Through a
high-gain amp they land where the amp's own intermodulation of a perfect octave down already
puts energy: f_b − f_a/2 is the amp's 2y − x of the two subs.

### CPU (`sedef_engine_tests "Pedal CPU cost"`, one channel at 48 kHz)

| | ns per sample |
|---|---|
| Trench Mono | 89–91 |
| Trench Poly | 105–109 |
| Pearl (for scale) | 118–132 |

## Where it goes in the chain

First, right after the gate: **Trench** → Anemone → Conch → Remora → Tide → Urchin → Sargasso →
Pearl → Kelp → Coral → Beacon → Gyre → Jetty → Ripple → Breaker. That is where the pedalboard
puts it by default, and the order we recommend; the player can move it anywhere.

- **Ahead of every drive.** Both modes need the string's own partials. Every harmonic a drive adds
  is one more the tracker has to see past (Mono), or one more partial to divide along with the
  drive's intermodulation (Poly). With Trench first, the measurements above hold.
- **Ahead of the compressor.** The sub's envelope is the input's. A compressor in front would
  flatten it, and the sub would stop following the picking. With Trench first, Tide evens out
  the guitar and the sub together.
- **Behind the gate.** Hiss and hum between notes never reach the level detector, so the sub
  stays silent between notes.
- **Not with the octave fuzz.** With Urchin on as well, the rectifier folds the sub too. In Mono
  the sub's f/2 then stands 17.6 dB above the octave: a growl that is neither pedal's sound. Each
  factory preset uses one octave pedal at a time.

## References

- P. McLeod, G. Wyvill, "A Smarter Way to Find Pitch", *Proc. International Computer Music
  Conference*, 2005 — the NSDF and the key-maximum rule (MPM).
- P. Dutilleux, K. Dempwolf, M. Holters, U. Zölzer, "Nonlinear Processing", ch. 4 in U. Zölzer
  (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011 — octave division with flip-flops.
- P. Horowitz, W. Hill, *The Art of Electronics*, 3rd ed., Cambridge University Press, 2015 —
  comparators with hysteresis, flip-flop frequency dividers.
- V. Välimäki, A. Huovilainen, "Antialiasing Oscillators in Subtractive Synthesis", *IEEE Signal
  Processing Magazine* 24(2), 2007; V. Välimäki, J. Pekonen, J. Nam, "Perceptually informed
  synthesis of bandlimited classical waveforms using integrated polynomial interpolation",
  *J. Acoust. Soc. Am.* 131(1), 2012 — polyBLEP.
- E. Thuillier, *Real-Time Polyphonic Octave Doubling for the Guitar*, MSc thesis, Aalto
  University, 2016 — phase scaling in a complex filter bank, its intermodulation, and
  single-sideband modulation as the alternative.
- N. Juillerat, S. Schubiger-Banz, S. Müller Arisona, "Low Latency Audio Pitch Shifting in the
  Time Domain", *Proc. ICALIP*, 2008 — a constant-Q filter bank with per-band single-sideband
  shifting.
- J. Laroche, M. Dolson, "Improved Phase Vocoder Time-Scale Modification of Audio", *IEEE Trans.
  Speech and Audio Processing* 7(3), 1999 — peak picking and identity phase locking.
- R. J. McAulay, T. F. Quatieri, "Speech Analysis/Synthesis Based on a Sinusoidal
  Representation", *IEEE Trans. ASSP* 34(4), 1986 — partials as tracked spectral peaks.
- The tracker's note logic, the owner and handover rules, the widening, the gates and all
  constants are our own.

## Tests

`engine/tests/analysis/MonoTrackerTests.cpp`, `engine/tests/pedals/TrenchTests.cpp` and
`engine/tests/pedals/PolySubTests.cpp`; the synthetic playing is in
`engine/tests/pedals/OctaverDi.h`, the chords in `engine/tests/pedals/OctaverChords.h`.

- MonoTracker: locks within two periods (or 8 ms) plus 5 ms at 44.1–192 kHz, E2 to E5; names
  bright and piezo notes and loud picks in the right octave (> 99 % of reports, median within
  3 cents); its fundamental turns exactly once per period; follows a new note over a ringing
  neighbour, vibrato, bends and legato steps; reports nothing for silence, hiss or an open fifth;
  resets to its initial state.
- Trench, both modes: finite output for random settings and full-scale noise; silence in gives
  silence out; direct at noon with the subs down is a bit-exact copy, with no latency; subs
  silent (exactly zero) over 3 s of hiss at −90 and −60 dBFS and of 50 and 60 Hz hum at −70 and
  −60 dBFS; a note 40 dB over a −70 dBFS hum gets its sub (in tune in Mono, at its level in Poly)
  and is 60 dB down within 150 ms of a mute; a hum that comes up after silence is let go within
  8 s; the real-playing scenarios, two chords and a bend give the same numbers with their peaks
  at −40, −30, −18 and −6 dBFS over hiss 70 dB under the peaks; automating each knob, all of
  them and the mode is click-free.
- Mono: subs within 1 cent of f/2 and f/4 from E2 to E5 at 44.1, 48 and 96 kHz; lock and level
  bounds; no burst on onsets, chords and releases (a muted note's sub is 60 dB down within
  150 ms); legato re-locks; the real-playing scenarios with their bars (no flips after the lock,
  ≥ 90 % of the settled level for ≥ 95 % of the note, no modulation above 5 Hz deeper than 3 dB,
  within 5 cents, > 99 % of periods in the right octave, lock ≤ 30 ms below A3 and ≤ 15 ms from
  A3 up); palm-muted sixteenths.
- Poly: every string an octave and two octaves down within 5 cents, its level within 1.5 dB of
  an ideal octave down, its lag under 13 ms, at 44.1, 48, 96 and 192 kHz; the five chords with every
  note within 10 cents, the off-grid power 20 dB down, the strongest off-grid peak 20 dB under the
  strongest note's sub, and warble under 3 dB; silence stays exactly silent and a mute is 60 dB down within 150 ms; bounded on
  full-scale noise and squares; reset matches a fresh instance; at noon, notes and a strum come
  out within 3 dB of their level.

## Known limitations

- A hum or hiss that comes up while nothing is played looks like a note to the level gate. It is
  divided until the floor catches up: 2 s, then 1 s for every 6 dB it stands over the old floor
  (about 6.5 s for a −70 dBFS hum after digital silence). Hum present when the plug-in starts,
  or hum under the playing, is never divided.
- A note held without a new pick keeps its sub until the floor, climbing 6 dB a second from 2 s
  after the pick, comes within 6 dB of it. With peaks near −30 dBFS that is 6.5 s after the pick
  for a string decaying with a 2 s time constant (the note then 33 dB under its attack) and 8.8 s
  for a 5 s one (19 dB under). The floor never climbs past −64 dBFS, so no note loses its sub
  sooner than with the fixed gate of 0.4: at −6 dBFS peaks both shut at −58 dBFS.
- Playing under −90 dBFS peaks is not divided: the gate opens from −100 dBFS. Below about
  −60 dBFS peaks, Mono's tracker also needs a little longer to see a pick (its onset detector
  ignores 2 ms power under −70 dBFS).

- Mono plays one note at a time. On a chord it follows the root when the other strings are its
  harmonics (power chords, open majors) and fades otherwise.
- Mono's lock takes about two periods of the note (28 ms on E2) plus the gate's attack.
- Mono can read a note an octave low when the instrument adds a strong component an octave
  under the string (an acoustic body's air resonance near 100 Hz, heard on piezo DI); a magnetic
  pickup does not.
- Poly's subs speak 7–12 ms behind an ideal octave down (the bank's resolution has a price), and
  only partials up to 1.2 kHz are divided, so its subs are darker than Mono's squares.
- Poly cannot separate partials closer than about 14 Hz: two strings whose partials nearly
  coincide beat, and their octave down beats a little differently. A strong partial also leaks
  a little into the band of a neighbour about 40 Hz away. The strongest sidebands sit 25–30 dB
  under the strongest note of a chord and 15–24 dB under the weakest, so open A and F major miss
  the 20 dB goal (see "The 20 dB intermodulation goal").
