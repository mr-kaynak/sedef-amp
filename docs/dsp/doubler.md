# Doubler

`engine/include/sedef/dsp/Doubler.h`, `engine/src/dsp/Doubler.cpp`

## What it models

Double tracking: the same part played twice and panned hard left and right.
Two takes never line up exactly. Their timing differs by a few milliseconds and
wanders, and their pitch differs by a few cents. The Doubler makes both sides
into "takes": each side gets its own short, slowly drifting delay. `spread`
sets how far the right take trails the left.

## Signal flow

```
L ─► delay (1.5 ms + drift L) ───────────────────────────► L
R ─► delay (1.5 ms + spread + drift R) ─► soften + 1 dB ─► R
```

Each side delays its own input channel, so a stereo cab or room keeps its
image. On a mono source (L == R) the result is double tracking. When the engine
runs the post chain in mono (one channel), the Doubler passes audio through
untouched.

## Drift

Each side's delay moves independently. Modulation is evaluated every 32
samples and interpolated in between.

| Component | Rate (L / R) | Depth | Effect |
|---|---|---|---|
| Timing wander | random walk, 0.3 Hz | min(0.8 ms, 12 % of spread) | the takes slide against each other; ≈ 1 cent |
| Pitch wobble | sine 0.9 / 1.13 Hz, × 0.6 | 0.45 ms | ≈ 2–3 cents |
| Pitch jitter | random walk 1.7 / 2.1 Hz, × 0.4 | 0.45 ms | ≈ 1 cent, keeps the motion from sounding periodic |

The wander depth scales with spread, so at 3 ms the takes cannot drift into
each other and flange.

Changing spread glides through two one-poles with a 120 ms time constant. A
3 → 20 ms jump therefore bends the right side's pitch by under a semitone
instead of clicking.

## Control mapping

| Control | Mapping |
|---|---|
| spread (3–20 ms) | nominal right − left offset. Clamped; non-finite values fall back to 6 ms |

## Mono compatibility: the trade-off

Summing two copies of a signal offset by Δ is a comb filter, with notches at
(2k + 1) / (2Δ). At the default 6 ms the first notch is at 83 Hz, then 250 Hz
and so on. This is inherent to two-take double tracking and the main cost of
the effect. Mitigations:

- **The notches move.** The relative delay drifts continuously, so the mono sum
  sounds like a gentle, slow flange rather than a fixed hollow tone.
- **The double is darker and 1 dB louder.** The right take passes a one-pole
  low-pass at 7.5 kHz and gains 1 dB. Unequal levels and spectra limit notch
  depth, to about −18 dB rather than a full null at matched frequencies. The
  1 dB lift also partly offsets the precedence effect, which otherwise pulls
  the image towards the earlier (left) side.
- **Larger spreads are kinder in mono.** Notches get denser and less tonal; at
  15–20 ms they read as space more than colour.

The alternative is a mid/side widener that is perfectly mono compatible: it
adds ± a delayed copy to a mono centre. It does not sound like two takes, so it
was not chosen. If mono playback matters more than the double-tracked sound,
keep spread at 12 ms or more, or leave the Doubler off for mono-critical mixes.

## References

- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011.
  Delay-based effects, comb filtering, modulated delay lines.
- J. Blauert, *Spatial Hearing*, rev. ed., MIT Press, 1997. Precedence effect
  and summing localisation.
- H. Haas, "Über den Einfluss eines Einfachechos auf die Hörsamkeit von
  Sprache", *Acustica* 1, 1951.

## Tests (`engine/tests/timefx/DoublerTests.cpp`, tag `[timefx][doubler]`)

- Output stays finite with random (including out-of-range) spreads and
  full-scale noise at 44.1, 48 and 96 kHz, in blocks of 1, 17, 64 and 512.
- Mono white noise in gives |correlation(L, R)| < 0.1 at lag 0 for spreads of
  3, 6 and 20 ms.
- The cross-correlation peak sits at the spread (4, 10 and 18 ms), within
  ±1.6 ms of drift, and stands clearly above the correlation floor.
- The left side keeps its level within ±0.2 dB. The right side is +1 dB
  ±0.3 dB.
- Pitch movement measured from zero crossings of a 1 kHz tone stays under
  10 cents, but is above zero.
- Toggling spread between 3 and 20 ms every 100 ms is click-free (second
  difference of a 220 Hz tone under 6e-3).
- Silence in gives silence out. Mono passes through untouched.
- `[.bench][timefx-bench]` prints ns/frame.

## Limitations / future work

- Only one spread control. There are no separate amounts for wander and pitch.
- Timing drift is limited to about 1 ms, so it does not imitate the larger
  phrasing differences between real takes.
