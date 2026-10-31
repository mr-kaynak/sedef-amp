# Drift (tape echo)

`engine/include/sedef/dsp/Drift.h`, `engine/src/dsp/Drift.cpp`

## What it models

A multi-head tape echo. A tape loop runs past one record head and up to four
playback heads. The model includes the parts that give tape echo its
character:

- **Wow and flutter**: slow and fast speed wobble that bends the pitch of the
  repeats.
- **Tape saturation** in the record path, with pre-emphasis.
- **HF and LF loss**, so each repeat comes back darker and thinner.
- **Bass and treble controls** in the playback amp. They act on the repeats and
  on the feedback.
- **Feedback from the last head**, enough to run away into self-oscillation.
- **Motor-like time changes**: the pitch bends while the time glides.

Disabling the effect stops new input from reaching the tape but lets the
repeats ring out (spillover).

## Signal flow

Two tape loops, A and B, share all voicing. Each loop has three chains:

```
record    x ─► pre-emphasis ─► drive ─► de-emphasis + LF loss ─► HF loss ─► tape
playback  heads ─► Σ level × head ─► tone (bass/treble) ─► wet
feedback  last head ─► tone ─► × loop gain ─► soft ceiling ─► ÷ drive slope

loop A records   input fade × (L + R) / 2  +  feedback of loop B
loop B records   feedback of loop A

out L = dry × in L + wet × (cos θ × wetA + sin θ × wetB)
out R = dry × in R + wet × (cos θ × wetB + sin θ × wetA),    θ = (1 − width) × π/4
```

Only A receives the input. A's last head feeds B, and B's last head feeds A, so
successive generations of repeats alternate between the two loops. Width then
decides how the loops are panned:

- **width 1**: A is hard left and B hard right, so repeats ping-pong L, R, L…
- **width 0**: both loops go to both sides at −3 dB (constant power), so every
  repeat is centred and L == R.

Mono output folds the stereo output to (L + R) / 2.

## Head patterns

Heads read the tape at fractions of the delay time T. Feedback always comes from
the head at T.

| `heads` | Positions (× T) | Levels | Feel (T = one beat) |
|---|---|---|---|
| 0 (1 head) | 1 | 1.0 | the beat |
| 1 (2 heads) | ½, 1 | 0.6, 0.8 | straight eighths |
| 2 (3 heads) | ⅓, ⅔, 1 | 0.5, 0.6, 0.62 | triplets |
| 3 (4 heads) | ⅓, ½, ⅚, 1 | 0.4, 0.55, 0.4, 0.6 | **swung sixteenths**: the off-beat sixteenth of each eighth is pushed to the triplet position; on-beats are stronger |

Earlier heads sit lower, so the main repeat leads. Each pattern's summed power
is about 1, so changing the head count does not change loudness much.

Internally the patterns use five head *slots* (⅓, ½, ⅔, ⅚, 1). Changing
pattern fades slot levels over 30 ms. Heads never move, so switching never
bends pitch.

## Delay time and sync

| Sync | T |
|---|---|
| Free | `timeMs` |
| Host | 60000 / bpm × `noteBeats(note)`. bpm is `transport.bpm` when `transport.hasHostTempo`, otherwise `tapBpm` |
| Tap | 60000 / `tapBpm` × `noteBeats(note)` |

T is clamped to 16–2500 ms, and the tape holds 2.6 s at the prepared rate.

`noteBeats` returns quarter-note beats for the 21 choices, sorted by length.
Triplets are ×⅔ and dotted notes ×1.5 of the straight value; for example
1/4 = 1, 1/8D = 0.75, 1/1D = 6.

`effectiveTimeMs()` reports the synced, clamped target for the UI. It is a
relaxed atomic.

**Glide.** The read position follows the target like a critically damped
spring with a 60 ms time constant, settling in about 0.3 s. That is the same
response as two cascaded one-poles, so the pitch bends smoothly. The spring's
velocity, which sets the playback speed, is a state clamped to **0.25×–2×**:

- The tape never runs backwards.
- Limits engage and release without pitch kinks. An earlier hard clamp on
  position left audible slope corners.
- Speed-up is capped at 2×. At 4× (an earlier setting), Hermite reads aliased
  audibly once feedback replayed material that had already been sped up. At 2×,
  the record path's HF loss keeps replayed content below Nyquist.

Big jumps therefore take longer than 0.3 s (2500 → 16 ms takes about 2.5 s).
Turning the time control with high feedback gives the familiar cascading
chirps: each pass replays the last one faster.

Reads use 4-point Hermite interpolation. The position is split into integer and
fraction in double precision, because at multi-second delays a float position
resolves only 1/64 sample.

## Control mapping

| Control | Mapping |
|---|---|
| feedback (0–1) | loop gain = 1.05 × knob. Below about 0.95 the echo decays; above it the echo self-oscillates |
| tapeMod (0–1) | depth = knob^1.5. At full: wow (0.6 Hz sine + 0.9 Hz random walk) about ±8 cents, flutter (6.3 Hz sine + 11 Hz random walk) about ±2–3 cents |
| tapeDrive (0–1) | y = g·tanh(k·x), k = 0.5 · 10^knob (0.5 → 5). g = 0.3 / tanh(0.3k), so a −10 dBFS peak keeps its level. At 0 the stage is clean to within about −0.5 dB of compression at full scale (ceiling 2); at 1 it is heavily compressed (ceiling about 0.32). Feedback is divided by the drive's small-signal slope (k·g), so drive does not move the runaway point |
| bass, treble (0–1) | first-order shelves at 220 Hz and 2.2 kHz: 0 → −12 dB, 0.5 → 0 dB, 1 → +6 dB. They act on the wet bus and on the feedback. Cuts reach further than boosts, because boosts inside a loop run away quickly |
| width (0–1) | θ = (1 − width)·π/4 (see signal flow) |
| mix (0–1) | **dual-ramp law with a 1.5 power**: wet = min(1, 2·mix)^1.5, dry = min(1, 2·(1 − mix))^1.5. At 0 only dry, at 1 only wet, at 0.5 both at full level. The default 0.35 puts the echo about 4.6 dB below the dry signal. The power gives finer control in the quiet-echo range |

The voicing that stays fixed:

- Pre-emphasis +6 dB above 2.5 kHz before the drive, and the matching
  de-emphasis after it. Highs saturate first, as on tape. With a linear drive
  the pair cancels.
- First-order LF loss at 45 Hz (this also blocks DC).
- 2nd-order HF loss at 6.8 kHz with Q 0.6, applied on every pass through the
  record head.

## Feedback, self-oscillation and bounds

The feedback path is: last head → tone → × loop gain → soft ceiling
0.7·tanh(x / 0.7) → ÷ drive slope.

- Tone sits *before* the ceiling, so boosts cannot lift a runaway past it.
- At feedback 1.0 with neutral tone, the oscillation builds slowly (about
  0.4 dB per pass) and settles around −10 dBFS on tape. In the wet output that
  is about 0.1 RMS.
- With tone boosts it saturates harder and gets louder.
- With continuous full-scale noise and every control at its worst, peaks stay
  below 4. The tests check this over 30 s.

## Spillover and idling

- Disabling fades the input into the tape out over 20 ms. The dry signal fades
  to unity at the same time, so a mix of 1.0 does not leave you silent.
- While disabled, the loop gain is held at or below 0.9 divided by the tone's
  peak gain, so even a self-oscillating echo dies away.
- Once nothing above −120 dBFS has been written for a whole tape length
  (2.6 s), `process` returns immediately: the dry signal passes bit-exact at
  about zero cost. A shorter test would be wrong, because a later, longer delay
  time could replay stale tape.
- Re-enabling starts recording again with a 20 ms fade-in.

## Real-time notes

Tapes are allocated in `prepare` (2 × 2.6 s). `reset` clears them.
Modulation, drive and tone coefficients update on a 32-sample control tick that
is independent of the host block size, so output is bit-identical for any
block partitioning (tested). Telemetry is a relaxed atomic.

## References

- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011.
  Delay-based effects, modulated delays, nonlinear processing.
- J. O. Smith III, *Physical Audio Signal Processing*, W3K, 2010. Variable
  delay lines, Doppler, interpolation.
- O. Niemitalo, *Polynomial Interpolators for High-Quality Resampling of
  Oversampled Audio*, 2001. 4-point Hermite.
- IEC 60386 / AES6: definitions of wow (slow) and flutter (fast) speed
  variation.
- General magnetic recording practice: record pre-emphasis / playback
  de-emphasis, head-gap HF loss, LF loss.

## Tests (`engine/tests/timefx/DriftTests.cpp`, tag `[timefx][drift]`)

- `noteBeats` covers all 21 choices, keeps triplet/dotted relations, increases
  monotonically, and clamps out-of-range indices.
- Output stays finite with random settings (including sync and host tempo) and
  full-scale noise at 44.1, 48 and 96 kHz, in blocks of 1, 17, 64 and 512.
- An impulse produces repeats exactly at the head positions of all four
  patterns (±3 samples), with no stray energy, and the first generation is
  entirely on the left.
- Width 1 alternates sides on each repeat. Width 0 gives L == R.
- Host sync: 120 bpm at 1/4 → 500 ms, and a repeat at 24000 samples. 1/8D →
  375 ms. With no host tempo it falls back to tap.
- Tap sync uses `tapBpm` and ignores the host.
- Time is clamped to 16–2500 ms, both in Free and in synced modes.
- Feedback 1.05 (clamped to the knob's full scale) with drive 0 or 1 and
  neutral or boosted tone, for 30 s with a burst or with continuous noise:
  finite, peak < 4, and the burst variant is still oscillating at the end.
- Spillover: after disabling, the old tail continues, the dry signal is at
  unity, and new input is not recorded. Later the module idles bit-exact, and
  it records again after re-enabling.
- Mix 0 is bit-exact dry. Mix 1 has no dry signal.
- Silence in gives silence out, even at full feedback.
- Output is identical for block sizes 1, 17, 64 and 512.
- Click-free automation is checked two ways:
  - Time, sync, note, head and tempo jumps with feedback 0: second difference
    under 1.5e-3 on a 110 Hz tone. Measured worst case is 5.7e-4.
  - Tone, feedback, drive, mod, width and mix jumps: under 0.02. Measured worst
    case is 9.8e-3; drive and treble add harmonics.
  With feedback, fast time changes cascade pitch upwards by design, so a
  second-difference test cannot separate that from a click.
- Time glides land on the new target.
- `[.bench][timefx-bench]` prints ns/frame.

## Limitations / future work

- The saturation is not oversampled (ADR-0008 asks for oversampling of
  nonlinear stages). Aliasing is kept low because the input is post-cab, the
  emphasis pair limits HF drive, and the record path's HF loss filters what
  lands on tape. An oversampled record path is possible future work.
- Wow and flutter are the same for both loops and all heads (one transport),
  and modulation depth is constant in milliseconds rather than scaling with
  tape speed.
- Reads are not band-limited, so speed-up above 1× relies on the record-path
  HF loss to avoid aliasing.
