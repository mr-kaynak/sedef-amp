# Urchin (octave-up fuzz)

`engine/include/sedef/dsp/Urchin.h`, `engine/src/dsp/Urchin.cpp`

## What it models

The transformer-and-diodes octave-up fuzz: a transistor gain stage drives a small interstage
transformer with a centre-tapped secondary, two germanium diodes rectify its two halves (a
full-wave rectifier), and a second transistor stage fuzzes the result. Folding the negative half
of every cycle up doubles the waveform's repetition rate, so a pure tone comes out an octave up.

There is no pitch shifter in it: the octave is whatever the rectifier makes of the signal, and the
model keeps that dependence. A note whose two half-cycles mirror each other (a fundamental with
only odd harmonics) folds into a clean octave. That is what a note fretted above the 12th fret
gives under the neck pickup, which then sits near the middle of the vibrating string, where the
even harmonics have their nodes; rolling the guitar's tone down helps the same way. A bright low
note on the bridge pickup has strong even harmonics, its half-cycles differ, and the rectifier
turns its harmonics into sum and difference tones that reinforce the note itself: a gnarly fuzz
with little octave. Chords intermodulate into a ring-modulator-like smear. The octave's strength
also changes from note to note with the phase of the second harmonic. Octave blends the rectified
signal against the plain fuzz (the same circuit without the rectifier).

## Signal flow

```
in ─► HP 40 Hz ─► transformer: HP 90 Hz, LP 3.5 kHz (Q 0.6) ─► × 4 (+12 dB)
   ─► [4× oversampled]  ┬─► germanium rectifier √(z² + 1) − 1, knee 0.15 V (ADAA)
                        │   ─► coupling: 2 × first-order HP 70 Hz ─► × 2 ─┐
                        └──────────────────────────────────────────────────┴─ blend (Octave)
      ─► × fuzz gain ─► transistor clipper: tanh above 0, 0.55·tanh(z/0.55) below (ADAA)
   ─► DC block 12 Hz ─► tone LP (1.2 → 9 kHz, Q 0.6) ─► LP 11 kHz ─► × trim·level ─► out
```

- **Calibration**: 1.0 full scale = 1 V at the jack, as for the amps
  ([amps.md §4](amps.md#4-gain-targets-and-input-level)).
- **Transformer**: a small interstage transformer passes neither deep lows nor much above the
  upper mids. The band limit also keeps the rectifier's input closer to a sine.
- **Rectifier**: `√(z² + 1) − 1` in units of the diode knee (0.15 V, germanium): above the knee it
  is |x| minus the drop, below it the diodes conduct on a square law. Soft picking still gives an
  octave (a square law is a frequency doubler too), slightly less clean: on the neck note, +12.9 dB
  of octave over the note when picked 30 dB softer, against +15.5 dB. Its antiderivative is closed-form:
  `(z √(z²+1) + asinh z)/2 − z`, odd in z.
- **Coupling**: the rectified signal's DC follows its envelope. Two first-order high-passes at
  70 Hz (in double, at the oversampled rate) keep it out of the fuzz stage, so an attack shifts the
  stage's bias only for a few milliseconds.
- **Output stage**: an asymmetric clipper (the negative half gives up earlier and softer), so the
  plain fuzz has some even harmonics of its own. The curve is bounded (+0.5 V, −0.28 V before the
  level control), whatever goes in.
- **Anti-aliasing**: both nonlinear stages run inside one 4× oversampler (2× at 96 kHz, 1× above
  100 kHz), each with first-order antiderivative anti-aliasing.

## Control mapping

| Control | Mapping |
|---|---|
| `fuzz` | Gain into the output stage, −6 dB + 40 dB × fuzz (linear in dB), ramped per sample. |
| `octave` | Blend from the plain signal (0) to the rectified octave (1), ramped per sample. |
| `tone` | Second-order low-pass (Q 0.6), 1.2 kHz × 7.5^tone: 1.2 kHz → 3.3 kHz at noon → 9 kHz. |
| `level` | 40 dB × (level − 0.5) (±20 dB) plus a trim, `trimDb(f) = −(1.243 + 17.867 f − 20.255 f² + 7.816 f³)`, fitted over Octave 0, 0.5 and 1 so that the reference tone comes out within 1.5 dB of its input level at level 0.5. |

`latencySamples()` returns the oversampler's latency (37 samples at 44.1/48 kHz).

## Measurements (48 kHz)

The octave (2f) against the note (f) at the output, fuzz, tone and level at noon, for three
sustained inputs: a neck note (A4 above the 12th fret, picked and heard near the middle of the
string, nothing above 2 kHz), a bridge note (open A2, bright, weak fundamental) and a pure sine:

| Octave | neck A4 | bridge A2 | sine 440 Hz |
|---|---|---|---|
| 0 | −34.2 dB | +2.6 dB | −28.3 dB |
| 0.25 | −8.2 dB | +0.9 dB | −11.4 dB |
| 0.5 | +0.4 dB | −2.3 dB | −7.1 dB |
| 0.75 | +13.2 dB | −19.0 dB | +10.6 dB |
| 1 | +15.5 dB | −9.8 dB | no f left |

The bridge note's input already carries 5.5 dB more 2f than f; the rectifier makes the note
stronger, not the octave. That is the honest failure: use the neck pickup.

Saturation: THD of the plain fuzz (octave 0, tone 10), and how much the output grows when a
330 Hz sine grows by 20 dB (−34 to −14 dBFS; octave 1, tone noon):

| Fuzz | THD, 220 Hz at −26 dBFS | output change for +20 dB in |
|---|---|---|
| 0 | 0.9 % | +28.6 dB |
| 0.25 | 6.7 % | +23.7 dB |
| 0.5 | 22.9 % | +15.1 dB |
| 0.75 | 37.6 % | +6.3 dB |
| 1 | 42.0 % | +1.2 dB |

(At low fuzz the octave path's square law makes the output grow faster than the input.) Level 0.5
against the reference tone: within −1.0…+1.5 dB for every fuzz and octave setting. Aliasing at full
fuzz, against the strongest harmonic: −83 / −72 dB (octave 0) and −71 / −69 dB (octave 1) for a
1.3 / 2.6 kHz note. CPU: about 2.5× Pearl.

## Where it goes in the chain

After the compressor, ahead of the drives: Trench → Tide → **Urchin** → Pearl → Kelp → Coral →
Ripple. That is the engine's fixed order until the pedalboard lets the player choose; it is also
the order we recommend.

- **Ahead of every drive.** The rectifier makes an octave only from a wave whose two half-cycles
  mirror each other (see above). A clipped input is close to a square wave. Rectified, that is a
  near-constant level with a dip at each zero crossing: buzz, not an octave.
- **After the compressor.** Tide evens out the notes and holds them up as they decay. The octave
  is strongest while the note is loud, so it sustains longer.
- **After the octaver, but not with it.** With Trench on as well, the rectifier folds Trench's
  square-wave sub too (see [trench.md](trench.md#where-it-goes-in-the-chain)).

## References

- P. Dutilleux, K. Dempwolf, M. Holters, U. Zölzer, "Nonlinear Processing", ch. 4 in U. Zölzer
  (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011 — harmonic and subharmonic
  generation; full-wave rectification as an octave doubler.
- P. Horowitz, W. Hill, *The Art of Electronics*, 3rd ed., Cambridge University Press, 2015 —
  centre-tapped full-wave rectifiers, transformer coupling.
- A. S. Sedra, K. C. Smith, *Microelectronic Circuits* — diode law and small-signal conduction,
  full-wave rectifiers.
- N. H. Fletcher, T. D. Rossing, *The Physics of Musical Instruments*, 2nd ed., Springer, 1998 —
  plucked strings: harmonic amplitudes against pluck and pickup position.
- J. Parker, V. Zavalishin, E. Le Bivic, *Proc. DAFx-16*, 2016; S. Bilbao, F. Esqueda, J. Parker,
  V. Välimäki, *IEEE Signal Processing Letters* 24(7), 2017 — antiderivative anti-aliasing.
- The transformer band, the knee, the clipper curve, the gain law and the trim are our own.

## Tests (`engine/tests/pedals/UrchinTests.cpp`)

- Finite output for random settings and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- 2f against f rises with every step of Octave for the neck note and a pure sine; at Octave 0 it
  is below −10 dB for the neck note.
- At Octave 1 the neck note's octave stands above +12 dB, at least 10 dB clearer than the bridge
  note's; a pure sine keeps no energy at f (> +40 dB).
- THD rises with Fuzz; at full fuzz 20 dB more input gives under 3 dB more output, and the peak
  stays below 1.0 for noise up to +20 dBFS.
- Tone darkens counter-clockwise; level 0.5 within ±2 dB of unity for fuzz 0…1 at Octave 0 and 1.
- Aliasing below −60 dB for high notes at full fuzz, octave off and on.
- `latencySamples()` equals the oversampler's; automating each knob, and all at once, is
  click-free.

## Known limitations

- The input transistor stage is linear; a real one saturates on very hot input, which squares the
  signal before the rectifier and weakens the octave further.
- The transformer is a fixed band-pass, not a model of its magnetics (saturation, leakage
  resonance).
- Chords and bright low notes smear by design; that is the circuit, not a defect to fix.
