# Pearl (overdrive)

`engine/include/sedef/dsp/Pearl.h`, `engine/src/dsp/Pearl.cpp`

## What it models

A transparent, touch-sensitive mid-hump overdrive: an op-amp gain stage with a pair of diodes
across its feedback resistor. Because the diodes limit only the *feedback* voltage, the stage's
output is the input plus a clipped, band-limited copy of it. Low notes pass mostly through the
clean term, so the low end stays full and tight; mids get the gain and the grit; picking softer
cleans it up.

## Signal flow

```
in ─► HP 20 Hz ─► mid boost (peak 850 Hz) ─┬─────────── delay (= oversampler latency) ──┐
                                            │                                            │
                                            └─► HP 650 Hz ─► × G ─► LP (14→5 kHz) ─►     │
                                                [4× oversampled] feedback-diode limiter  │
                                                (antiderivative anti-aliased) ─► ×knee ─►+
                                                                                          │
                     out ◄─ × trim·level ◄─ LP 11 kHz ◄─ tone LP (1.5→8 kHz) ◄─ DC block ◄┘
```

- **Quasi-static model** (after Yeh, Abel & Smith): `out = in + D(G · HP(in))`. The feedback
  network's high-pass (650 Hz) sets the mid hump; its capacitor band-limits the clipping path, with
  the corner falling as the gain rises (14 kHz → 5 kHz).
- **Diode limiter** `D`, normalised to a knee of 1: `0.85 · a/√(1+a²) + 0.15 · asinh(a)`. Linear at
  first, a firm knee, then a slow logarithmic rise like the diode law, so harder picking still gets
  a little louder. The negative half has an 18 % earlier knee (mismatched diodes) for a touch of
  even-order warmth; the DC block removes the resulting offset. Knee: 0.35 full scale.
- **Anti-aliasing**: the limiter runs 4× oversampled (2× at 96 kHz, 1× above 100 kHz) and uses
  first-order antiderivative anti-aliasing (closed-form antiderivative, evaluated in double).
  Inharmonic products of a 1.3 kHz or 2.6 kHz note at full gain stay below −68 dB re the
  fundamental at 44.1/48 kHz (−40…−50 dB with oversampling alone).
- The clean path is delayed by exactly the oversampler latency so the two paths sum coherently
  (the ADAA's half-sample delay at the oversampled rate, 1/8 of a base sample at 4×, is negligible).

## Control mapping

| Control | Mapping |
|---|---|
| `gain` | Clipping-path gain 3 dB + 37 dB × gain (3…40 dB, linear in dB). Also moves the clipping path's low-pass corner 14 kHz → 5 kHz (exponential). |
| `mid` | Pre-clip RBJ peaking EQ, 850 Hz, Q 0.8, 0…+10 dB (linear in dB). Feeds both paths. |
| `tone` | Post first-order low-pass, 1.5 kHz → 8 kHz (exponential; ~3.5 kHz at noon). |
| `level` | 40 dB × (level − 0.5) (±20 dB) plus a gain-dependent trim, `trimDb(g) = −2.26 − 7.7 g + 3.44 g²`, fitted so that at level 0.5 a guitar-like reference tone (eight harmonics of 147 Hz, −10 dBFS peak) comes out at −0.5 dB (gain 0), 0 dB (gain 0.5) and +1 dB (gain 1). |

Gains are ramped per sample (30 ms); filter coefficients are recomputed every 32 samples while
their knob is moving. `latencySamples()` returns the oversampler's latency (37 samples at
44.1/48 kHz, 31 at 88.2/96 kHz, 0 above).

Measured at gain 0.5, sine at 0.2 FS: THD 4.9 % at 82 Hz vs 19 % at 750 Hz (low end clean); at
196 Hz THD goes 2.6 % → 13 % → 16 % for 0.05/0.2/0.5 FS input (touch sensitivity).

## References

- D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and
  Overdrive Guitar Effects Pedals", *Proc. DAFx-07*, 2007 — op-amp with diodes in the feedback
  loop, quasi-static approximation.
- D. T. Yeh, *Digital Implementation of Musical Distortion Circuits by Analysis and Simulation*,
  PhD thesis, Stanford University, 2009.
- J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using
  Continuous-Time Convolution", *Proc. DAFx-16*, 2016; S. Bilbao, F. Esqueda, J. Parker,
  V. Välimäki, "Antiderivative Antialiasing for Memoryless Nonlinearities", *IEEE Signal
  Processing Letters* 24(7), 2017.
- A. S. Sedra, K. C. Smith, *Microelectronic Circuits* — non-inverting op-amp stage, diode law.
- The limiter curve, corner frequencies, gain law and trim are our own design.

## Tests (`engine/tests/pedals/PearlTests.cpp`)

- Finite output for random settings and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- THD rises monotonically with gain (< 10 % at gain 0, > 20 % at gain 1).
- Low end: at gain 0.5 an 82 Hz note distorts less than half as much as a 750 Hz one and keeps its
  fundamental within 3 dB; THD rises with input level (touch sensitivity).
- Mid boost lifts 1 kHz against 100 Hz by 7–11 dB at full `mid`, about half that at 0.5.
- Tone darkens counter-clockwise; level 0.5 is within ±1.5 dB of unity for every gain.
- Aliasing below −60 dB re fundamental for high notes at full gain.
- `latencySamples()` equals the oversampler's; the clean and clipping paths are time-aligned (the
  low-level response is a smooth hump, no comb filtering — the test fails if the delay is removed).
- Automating each knob (and all at once) is click-free.

## Known limitations

- Quasi-static: the feedback capacitor's interaction with the diodes (frequency-dependent clipping
  threshold) is approximated by band-limiting before the limiter.
- The trim is fitted to one steady reference tone; real playing at very different levels will be
  louder or quieter accordingly.
