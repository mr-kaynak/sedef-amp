# The intonation workbench

## Why a tuned guitar can still sound out of tune

Fretting a string does two things: it shortens the vibrating length (that's the point) and it
stretches the string down to the fret, which raises its tension a little. The extra tension makes
fretted notes slightly sharp, more so the higher the action and the thicker the string. Bridge
saddles exist to compensate: moving a saddle away from the neck lengthens the string just enough
that the 12th fret lands exactly one octave above the open string.

## What Sedef measures

For each string, the workbench compares two readings, each captured from one pluck:

- the **open string**, which you tune close to pitch first, and
- the **12th-fret fretted note**, played with your normal fretting pressure.

The octave error is `c = 1200 · log2(f12 / (2 · f0))` cents, with `f0` the open string as
measured, not the nominal note. A positive value means the fretted note is sharp.

Readings come from the McLeod Pitch Method on a decimated signal (see
[dsp/tuner.md](dsp/tuner.md)). The tracker skips the first ~150 ms after each pluck, because a
freshly picked string reads sharp while it settles, and then averages the sustained part.

## Tuning the open string

The open string has to be on the right note and close to pitch, not perfect:

| Steady reading | What happens |
|---|---|
| Within ±5 cents of the note | The workbench says "Good enough, measuring" and moves on by itself. |
| On the right note, further off (up to ±50 cents) | It says how far ("7.2 cents flat, tune up a little") and offers **Use this tuning**, which measures the 12th fret against the string as it is. Or tune and pluck again. |
| On another note | Nothing is measured; the tuner shows the way. |

Why close is enough: the octave is compared with the open string's measured frequency, so its
distance from the nominal note does not enter the formula above. It acts only through the string's
tension. Tension goes with the square of the frequency, so a string `d` cents off is
`2 · d · ln 2 / 1200 ≈ 0.12% per cent` off in tension: 0.6% at 5 cents, 1.2% at 10. The fretting
stretch adds a fixed amount of tension, so the sharpening it causes (a few cents, up to about ten:
that is what the saddle compensates) scales with `1/T` and changes by the same fraction. At 5
cents that is a few hundredths of a cent; at 10 cents about 0.1 cent. A reading used at the
50-cent extreme can be off by up to about half a cent, so tune closer for the final pass. The 5
cents is there so the guitar is in tune when you are done, not for the measurement.

Why not perfect: a plucked string never holds still. It starts a few cents sharp and settles over
~150 ms, wobbles by about a cent with your fretting pressure and the body moving, and sags slowly
flat as it decays. The previous version of the workbench wanted the instantaneous reading within
one cent of the note for a whole second; on the synthetic plucks below, with the strings tuned exactly,
that never happened once.

## Capturing a reading

Both notes are captured by the same rule (`ui/src/features/intonation/capture.ts`), so whatever a
pluck does to the pitch happens the same way in both and drops out of the octave between them:

- A capture averages the tracker's `stableHz` (itself the mean of the settled detections of the
  last 0.4 s) over **one second, starting 0.6 s after the pluck**. 0.6 s is about the earliest
  the tracker can call a note stable; reading both notes at the same moment of their decay cancels
  the slow flat drift.
- The reading must stay steady: the spread of the settled detections (`stableStdCents`) within
  **1 cent**, `stableHz` moving by no more than **4 cents** (a decaying string drifts and wobbles
  less than 3 in a second; a turned peg moves more), no gap over 0.25 s. The tracker must call the
  note `stable` (spread under 0.5 cent, high clarity) at least once during the second, which keeps
  a mixture of two ringing strings out.
- The open string counts when its nearest note is the target. The 12th-fret note counts within
  half a semitone of twice the open string as measured, not by the note grid: an open string used
  40 cents sharp with a saddle 12 cents sharp puts the octave nearer the next semitone. Further off
  is another note (another fret, the next string), and the workbench says what it hears.
- A pluck is captured once. If its reading stops being steady, or is not steady by 1 s after the
  pluck, the workbench asks you to pluck again rather than measure a later, drifted stretch.

The previous rule for the fretted note waited for the tracker's `stable` flag to hold without a
break for 0.5 s. A string wobbling by a cent drops that flag now and then; each break started the
capture again, later in the decay, and on a string sagging flat a later capture reads lower.

`engine/tests/analysis/PitchIntonationTests.cpp` runs the real tracker on synthetic plucks of the
six strings of a guitar and their 12th-fret octaves (attack glide of 8 and 12 cents, ±1 cent of
random wobble, 1.5 cents a second of flat drift, exponential decay, noise and mains hum), read at
the editor's 60 Hz, and checks the result against the true octave. With 12 plucks of each note
(`sedef_engine_tests "[intonation-report]"`):

| Wobble rates | Rule | Open string accepted | Result by | Octave error, rms | Worst |
|---|---|---|---|---|---|
| 1.5-8 Hz | previous | 0 of 72 | 2.08 s | 0.29-0.50 cent | 1.49 cent |
| 1.5-8 Hz | workbench | 72 of 72 | 1.63 s | 0.02-0.04 cent | 0.07 cent |
| 0.4-8 Hz | previous | 0 of 72 | 2.37 s (one missed) | 0.31-0.77 cent | 1.48 cent |
| 0.4-8 Hz | workbench | 72 of 72 | 1.63 s | 0.11-0.15 cent | 0.34 cent |

The tracker's own `stable` thresholds are unchanged: it calls every one of these notes stable by
0.8 s (a bass B0 takes about 0.9 s), so a decaying 12th-fret note does reach it; what failed was
asking it to hold without a break. A wander as slow as 0.4 Hz cannot average out in a second;
that, not the capture, limits the last row. A half-second capture left up to 0.6 cent of it.

## From cents to millimetres

Model the fretted note as the geometric ratio `L / (L − S/2)` times a constant sharpening factor
`k` (the stretch described above), with scale length `S` and current string length `L ≈ S`. If the
measured octave is `2 · q` with `q = 2^(c/1200)`, then `k = q`, and the saddle move `Δ` that brings
the octave back to exactly 2 satisfies `q · (S + Δ) = 2 · (S/2 + Δ)`, so

```
Δ = S · (q − 1) / (2 − q)  ≈  S · (2^(c/1200) − 1)
```

The approximation is within 0.01 mm of the exact value for any error you'd meet in practice. On a
25.5" (648 mm) scale, each cent is about 0.37 mm of saddle travel.

| Result | What to do |
|---|---|
| \|c\| ≤ 1.5 cents | In tune. Leave it. |
| c > 0 (sharp) | Move the saddle **away from the neck** (lengthen the string) by about Δ. |
| c < 0 (flat) | Move the saddle **toward the neck** (shorten the string) by about \|Δ\|. |

## Tips for good measurements

- Fit fresh strings first. Old strings intonate unpredictably.
- Set action and relief before intonation; changing them afterwards shifts the result.
- Play in your normal position with your normal pressure. Pressing hard makes notes sharp.
- Retune the open string after every saddle move, then measure again. Two or three passes are
  normal.
- Pluck the open string and the 12th fret the same way, with the same strength. A hard pluck
  rings sharp for longer, and the workbench reads both notes at the same moment after the pluck.
- After turning a peg, pluck again: each pluck is measured once.
- Turn off Transpose, and use a clean sound or the tuner's direct input.

## Limits

The model assumes the stretch factor doesn't change as the saddle moves, which holds for the
small moves intonation needs. Very high action, worn frets, or a badly cut nut can need fixes the
saddle can't provide.
