# Sargasso (sustain fuzz)

`engine/include/sedef/dsp/Sargasso.h`, `engine/src/dsp/Sargasso.cpp`

## What it models

The classic four-stage sustain fuzz: an input stage, two cascaded clipping transistor stages with
diodes in their feedback, a passive tone network that blends a low-pass and a high-pass with one
pot (a mid scoop at noon), and a recovery stage that makes the level back up. The two clippers in
series hold a decaying note at full level until it falls below their threshold, so the note
sustains; **Sustain** sets the gain into them, and with it how far down that threshold reaches.

## Signal flow

```
in ─► HP 40 Hz ─► × drive (6 … 46 dB) ─► LP 3.5 kHz
   ─► [4× oversampled] clip A (antiderivative anti-aliased) ─► coupling HP 90 Hz ─► × 10
       ─► LP 3.5 kHz ─► clip B (antiderivative anti-aliased)
   ─► tone network (bilinear, prewarped at 1 kHz) ─► recovery: HP 30 Hz, LP 8 kHz (Q 0.6)
   ─► × knee · 4 · trim · volume ─► out
```

- **Clipping stages**: a common-emitter stage with antiparallel silicon diodes across its feedback
  resistor, normalised to the diodes' knee (0.45 FS at the stage output):
  `(1 − τ)·a/√(1 + a²) + τ·a/√(1 + (a/4)²)`, τ = 0.15: a soft knee, then the diode law's slow rise
  until the transistor runs out of swing at 4× the knee. The negative half's knee is 0.92 of the
  positive one (the transistor's bias sits slightly off centre), which leaves a trace of even
  harmonics. Square roots only, so the closed-form antiderivative is cheap.
- **Stage bandwidth**: each stage's feedback capacitor rolls it off above 3.5 kHz, which keeps the
  fuzz smooth rather than fizzy.
- **Tone network** (our component values): low-pass branch 18 kΩ + 22 nF, high-pass branch
  3.9 nF + 22 kΩ, a 100 kΩ linear pot between them, the wiper into a 100 kΩ load. Nodal analysis
  gives a second-order N(s)/D(s) for any pot position (`Sargasso::toneNetwork`); the bilinear
  transform, prewarped at 1 kHz, makes it a biquad, updated per control block while the knob
  moves.
- **Anti-aliasing**: 4× oversampling (2× at 88.2/96 kHz, 1× above) plus first-order antiderivative
  anti-aliasing on each clipper.

## Control mapping

| Control | Mapping |
|---|---|
| `sargassoSustain` | Drive into the first clipper, 6 + 40 × sustain dB (linear in dB). |
| `sargassoTone` | Pot position of the tone network: 0 = the low-pass alone (bass), noon = the scoop, 1 = the high-pass alone (treble). |
| `sargassoVolume` | 40 dB × (volume − 0.5) (±20 dB) plus a fitted trim, `−(14.05 − 1.66 e^(−sustain/0.25))` dB, so that at volume 0.5 and tone noon the reference tone (eight harmonics of 147 Hz, −10 dBFS peak) leaves at its input level for every sustain setting. |

Gains ramp over 30 ms; filters are smoothed over 20 ms and recomputed every 32 samples while
moving. `latencySamples()` is the oversampler's.

## Measurements (48 kHz, `engine/tests/pedals/SargassoTests.cpp`)

Tone network, analogue magnitude (dB):

| Tone | 100 Hz | 250 Hz | 500 Hz | 1 kHz | 2 kHz | 4 kHz | 8 kHz |
|---|---|---|---|---|---|---|---|
| 0 (bass) | −2.6 | −3.3 | −5.1 | −8.8 | −13.6 | −18.9 | −24.7 |
| 0.25 | −5.6 | −6.6 | −9.0 | −13.9 | −17.5 | −15.3 | −14.0 |
| 0.5 (noon) | −8.6 | −9.8 | −12.8 | −17.4 | −13.2 | −9.6 | −8.4 |
| 0.75 | −12.1 | −13.6 | −17.0 | −15.0 | −8.8 | −5.6 | −4.4 |
| 1 (treble) | −17.9 | −19.1 | −17.3 | −10.3 | −4.8 | −1.7 | −0.5 |

At noon the scoop is centred between 900 and 1150 Hz and 8–10 dB below the mean of the 100 Hz and
8 kHz shelves, which agree within 1 dB. The digital filter follows the analogue network within
0.3 dB up to 3 kHz (0.75 dB at 6 kHz, the bilinear warping above the 1 kHz prewarp point), and the
network matches a direct complex nodal solution of the circuit within 0.01 dB.

Sustain: a plucked 196 Hz note (0.3 FS, decaying with a 0.4 s time constant: 20 dB in 0.92 s on
its own), time until the output is 20 dB below its peak; THD and H2/H3 for a steady 196 Hz at
0.1 FS; level of the reference tone at volume 0.5.

| Sustain | 0 | 0.25 | 0.5 | 0.75 | 1 |
|---|---|---|---|---|---|
| Decay to −20 dB | 1.72 s | 2.14 s | 2.60 s | 3.06 s | 3.52 s |
| Small-signal gain, 1 kHz | 7.4 dB | 16.4 dB | 26.0 dB | 35.9 dB | 45.8 dB |
| THD | 12 % | 18 % | 20 % | 22 % | 22 % |
| H2/H3 | 0.090 | 0.083 | 0.064 | 0.038 | 0.018 |
| Level (volume 0.5) | 0.00 dB | −0.08 dB | −0.05 dB | −0.01 dB | −0.02 dB |

Aliasing at full sustain, worst inharmonic component re the fundamental, 0.3 FS sine: −82.0 /
−76.6 dB (1318.5 / 2637 Hz at 44.1 kHz), −82.8 / −77.1 dB (at 48 kHz).

## References

- D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and
  Overdrive Guitar Effects Pedals", *Proc. DAFx-07*, 2007 — diode clipping in a feedback path,
  quasi-static stages with band-limiting.
- D. T. Yeh, *Digital Implementation of Musical Distortion Circuits by Analysis and Simulation*,
  PhD thesis, Stanford University, 2009 — tone stacks by nodal analysis and the bilinear
  transform.
- A. S. Sedra, K. C. Smith, *Microelectronic Circuits* — common-emitter stages, diode law,
  passive RC networks.
- J. Parker, V. Zavalishin, E. Le Bivic, *Proc. DAFx-16*, 2016; S. Bilbao, F. Esqueda, J. Parker,
  V. Välimäki, *IEEE Signal Processing Letters* 24(7), 2017 — antiderivative anti-aliasing.
- The clipper curve, component values, gain staging and trim are our own design.

## Chain position

**After the compressor, before the overdrives.** The sustain fuzz does not depend on the guitar's
impedance, so it can follow Tide; a compressor in front evens out the attack it clips and makes
the sustain smoother. Remora still goes first, because its clean-up needs the guitar directly.

## Tests (`engine/tests/pedals/SargassoTests.cpp`)

- Finite output at 44.1–192 kHz, block sizes 1–4096, random settings; silence in gives silence out.
- Block-size independence (< 1e-5) at every rate.
- The tone network equals a direct complex nodal solution of the circuit (0.01 dB) at five pot
  positions and seven frequencies; the scoop's centre and depth at noon and both ends' tilt.
- The pedal's small-signal response follows the network when the tone knob turns.
- Sustain: decay time rises by more than 0.2 s per quarter turn, at least 1.8× from 0 to 1.
- Nearly symmetric clipping: H2 below a quarter of H3.
- Level matching (±1 dB at volume 0.5).
- Aliasing below −60 dB re fundamental at full sustain, 44.1 and 48 kHz.
- `latencySamples()` equals the oversampler's; automating every knob is click-free.

## Known limitations

- Quasi-static clippers: the stages' coupling capacitors are modelled as the filters between them,
  not as part of the nonlinearity, so there is no bias shift after loud transients.
- The component values are our own, chosen to land the scoop near 1 kHz; the many historical
  variants of this circuit (different scoop depths, flat-mid mods) are not offered.
