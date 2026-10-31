# The intonation workbench

## Why a tuned guitar can still sound out of tune

Fretting a string does two things: it shortens the vibrating length (that's the point) and it
stretches the string down to the fret, which raises its tension a little. The extra tension makes
fretted notes slightly sharp, more so the higher the action and the thicker the string. Bridge
saddles exist to compensate: moving a saddle away from the neck lengthens the string just enough
that the 12th fret lands exactly one octave above the open string.

## What Sedef measures

For each string, the workbench compares two stable readings:

- the **open string**, which you tune to pitch first, and
- the **12th-fret fretted note**, played with your normal fretting pressure.

The octave error is `c = 1200 · log2(f12 / (2 · f0))` cents. A positive value means the fretted
note is sharp.

Readings come from the McLeod Pitch Method on a decimated signal (see
[dsp/tuner.md](dsp/tuner.md)). The tracker skips the first ~150 ms after each pluck, because a
freshly picked string reads sharp while it settles, and then averages the sustained part. A reading
is only accepted once its spread is below about half a cent.

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
- Turn off Transpose, and use a clean sound or the tuner's direct input.

## Limits

The model assumes the stretch factor doesn't change as the saddle moves, which holds for the
small moves intonation needs. Very high action, worn frets, or a badly cut nut can need fixes the
saddle can't provide.
