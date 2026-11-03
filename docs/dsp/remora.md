# Remora (two-transistor fuzz)

`engine/include/sedef/dsp/Remora.h`, `engine/src/dsp/Remora.cpp`, shared transistor stage in
`engine/include/sedef/dsp/detail/BjtStage.h`

## What it models

The classic two-transistor fuzz: two common-emitter stages in a shunt-feedback pair, the output
transistor's collector feeding back to the input transistor's base. Three things make it more than
a clipper, and Remora models all three:

- **Low input impedance.** The pair loads the guitar (a few kilohms against the pickup's 6 kΩ and
  the 250 kΩ volume pot), so rolling the guitar's volume back cuts the drive far more than the
  level: the fuzz cleans up into a crunch and then nearly clean, and gets brighter on the way.
  Sedef sees a DI, so the guitar's knob is a control on the pedal: **Guitar volume**.
- **Bias.** Where the output transistor rests between cutoff and saturation sets how lopsided
  the clipping is. At noon it is symmetric (odd harmonics); turned down it starves the
  transistor: gated, sputtering, even harmonics; turned up it runs hot against saturation.
- **Germanium or silicon.** Germanium has low current gain, so the pair has little feedback: a
  soft, warm, compressed fuzz with the junction's curve exposed. Silicon has several times the
  gain: more feedback, a harder and brighter fuzz with more gain.

## Signal flow

```
in ─► guitar load: pickup L + R, volume pot, into the pair's input impedance (LP, DC gain)
   ─► × input gain (fuzz, Ge/Si) ─► [4× oversampled] transistor pair, implicit Ebers–Moll curve
       (tabulated, antiderivative anti-aliased) with a slow bias servo
   ─► DC block 20 Hz ─► bandwidth LP (4.5 kHz Ge … 12 kHz Si) ─► fizz LP 11 kHz (Q 0.6)
   ─► × trim · volume ─► out
```

### The transistor pair

The pair is one quasi-static stage (`detail::BjtStage.h`): the relative change of the output
transistor's collector current, x (−1 is cutoff, xs is saturation), satisfies

```
w = ln(1 + x) + g·x + W(x),     W(x) = κ·xs·(atanh(x/xs) − x/xs)  for x > 0
```

where w is the drive in thermal-voltage units, `ln(1 + x)` the base-emitter junction (Ebers–Moll),
`g·x` the pair's shunt feedback with loop gain g, and W a saturation wall that is flat at rest and
holds x below xs (κ sets how soft the knee is). The curve is monotonic, bounded, and has slope
1/(1 + g) at rest. Its output in volts is the cutoff headroom times x.

- **Bias** moves the resting point: the fraction `rest` of the 0.5 V output swing already dropped
  across the collector load runs from 1.5 % (starved; logarithmic travel below noon so the
  starved end spreads over the knob) through 50 % at noon to 94 % (hot). gm follows the resting
  current, so `g = loopGain · rest / 0.5` and `xs = (1 − rest) / rest`.
- **Germanium / silicon**: loop gain at noon 3 / 24, saturation knee κ 1 / 0.25, input impedance
  7.5 kΩ / 15 kΩ, bandwidth 4.5 / 12 kHz, gain range (fuzz 0…1) 14…34 dB / 22…44 dB. The switch
  morphs over 50 ms (gain, curve, servo and filters together), so flipping it never thumps.
- **Bias servo**: the pair's DC feedback through the emitter bypass capacitor holds the average
  collector current near rest (time constant 30 ms). A loud, lopsided note pushes the operating
  point towards cutoff; the decay that follows comes out gated (the sputtery "choke" of a starved
  fuzz) until it recovers.
- **Solving**: the curve has no closed form. It is solved offline (Newton in variables that
  straighten each side of the curve, checked against bisection to 1e-9) and tabulated: 768 nodes
  on a warped axis with an exact node at w = 0 (silence stays exact), cubic Hermite with exact
  slopes, and the running integral of the interpolant for the anti-aliasing. 33 tables per
  transistor type span the bias knob and are blended linearly; they depend on nothing else, so
  every instance shares them (built on first `prepare`, about 1.6 MB for both types).
- **Anti-aliasing**: 4× oversampling (2× at 88.2/96 kHz, 1× above) plus first-order
  antiderivative anti-aliasing on the tabulated curve; a change of bias or material re-anchors the
  antiderivative so it never spikes.

## Control mapping

| Control | Mapping |
|---|---|
| `remoraFuzz` | Small-signal gain of the pair, linear in dB: 14 → 34 dB (Ge), 22 → 44 dB (Si). |
| `remoraVolume` | 40 dB × (volume − 0.5) (±20 dB) plus a fitted trim: Ge `−(3.80 − 1.26 e^(−fuzz/0.28))` dB, Si `−(3.57 + 0.34 fuzz)` dB, so that at volume 0.5, noon bias and the guitar full up the reference tone (eight harmonics of 147 Hz, −10 dBFS peak) leaves at its input level for every fuzz setting. |
| `remoraBias` | Resting point 1.5 % … 50 % (noon) … 94 % of the swing, as above. |
| `remoraGuitarVolume` | The guitar's 250 kΩ audio-taper pot (taper 3.5) between a 6 kΩ / 2.5 H pickup and the pair's input impedance: a first-order low-pass whose DC gain and corner both follow the knob. Shows 0–10 like the guitar's own knob. |
| `remoraTransistor` | Germanium / Silicon, morphed over 50 ms. |

Knobs are smoothed over 20 ms, gains ramp over 30 ms, filters update every 32 samples while
moving. `latencySamples()` is the oversampler's (37 samples at 44.1/48 kHz, 31 at 88.2/96 kHz, 0
above).

## Measurements (48 kHz, `engine/tests/pedals/RemoraTests.cpp`)

Clean-up: germanium, fuzz 1, bias noon, a 196 Hz note at 0.2 FS.

| Guitar volume | 10 | 9 | 8 | 7 | 6 | 5 | 4 |
|---|---|---|---|---|---|---|---|
| THD | 41.8 % | 29.7 % | 19.5 % | 13.0 % | 9.0 % | 6.8 % | 5.4 % |
| Level | 0 dB | −0.6 | −1.2 | −1.8 | −2.5 | −3.2 | −4.1 |

Bias: germanium, fuzz 0.5, 196 Hz at 0.1 FS. "Even/odd" is (H2 + H4) / (H3 + H5).

| Bias | 0 | 0.1 | 0.3 | 0.5 | 0.7 | 0.9 | 1 |
|---|---|---|---|---|---|---|---|
| H2 | 0.688 | 0.607 | 0.420 | 0.011 | 0.174 | 0.324 | 0.394 |
| H3 | 0.325 | 0.197 | 0.033 | 0.278 | 0.243 | 0.148 | 0.083 |
| Even/odd | 2.06 | 2.73 | 7.69 | 0.04 | 0.82 | 2.51 | 4.46 |

Silicon's larger loop gain keeps it more symmetric off noon (even/odd 0.49 at bias 0.3, 0.42 at
0.9): more feedback linearises the asymmetry, as in the circuit.

Germanium against silicon (bias noon, guitar full up; THD and H5/H3 for 196 Hz at 0.1 FS; level
for the reference tone at volume 0.5):

| Fuzz | Type | Small-signal gain, 1 kHz | THD | H5/H3 | Level |
|---|---|---|---|---|---|
| 0 | Ge | 2.2 dB | 6.3 % | 0.07 | 0.00 dB |
| 0 | Si | 13.3 dB | 33.3 % | 0.46 | 0.00 dB |
| 0.5 | Ge | 11.2 dB | 30.5 % | 0.41 | −0.08 dB |
| 0.5 | Si | 24.1 dB | 42.0 % | 0.59 | +0.04 dB |
| 1 | Ge | 21.0 dB | 40.6 % | 0.57 | 0.00 dB |
| 1 | Si | 34.9 dB | 42.9 % | 0.60 | 0.00 dB |

(The small-signal gain is the whole pedal's at volume 0.5, trim and guitar loading included.)

Aliasing at full fuzz, worst inharmonic component re the fundamental, 0.3 FS sine:

| | 1318.5 Hz, 44.1 kHz | 2637 Hz, 44.1 kHz | 1318.5 Hz, 48 kHz | 2637 Hz, 48 kHz |
|---|---|---|---|---|
| Germanium | −111.3 dB | −110.2 dB | −109.2 dB | −108.9 dB |
| Silicon | −75.2 dB | −69.8 dB | −78.5 dB | −71.9 dB |

## References

- J. J. Ebers, J. L. Moll, "Large-Signal Behavior of Junction Transistors", *Proc. IRE* 42(12),
  1954 — the junction law.
- B. Holmes, M. Holters, M. van Walstijn, "Comparison of Germanium Bipolar Junction Transistor
  Models for Real-time Circuit Simulation", *Proc. DAFx-17*, 2017 — the two-transistor fuzz case
  study: germanium Ebers–Moll parameters (current gain in the 40s to 100s), published waveforms
  (about 34 dB small-signal gain, asymmetric clipping of a few hundred millivolts) that set our
  swing and gain range.
- A. S. Sedra, K. C. Smith, *Microelectronic Circuits* — shunt-feedback pairs, loop gain, input
  impedance of a feedback amplifier, the volume pot as a loaded divider.
- J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using
  Continuous-Time Convolution", *Proc. DAFx-16*, 2016; S. Bilbao, F. Esqueda, J. Parker,
  V. Välimäki, "Antiderivative Antialiasing for Memoryless Nonlinearities", *IEEE Signal
  Processing Letters* 24(7), 2017.
- D. T. Yeh, *Digital Implementation of Musical Distortion Circuits by Analysis and Simulation*,
  PhD thesis, Stanford University, 2009 — quasi-static modelling of transistor stages.
- The normalised curve, the bias law, the servo, the voicing numbers and the trim are our own
  design, tuned by measurement against the targets above.

## Chain position

**Right after the gate and the octave divider, ahead of the compressor and every drive.** The
clean-up depends on seeing the guitar's pickup and volume pot directly. A buffer, compressor or
another drive in front changes the source impedance and the level, and the fuzz no longer cleans
up; the real circuit has the same rule, which is why this fuzz goes first. The one pedal ahead of
it is Trench: an octave divider can't track a fuzzed note, and its subs into a fuzz are a
classic pairing.

## Tests (`engine/tests/pedals/RemoraTests.cpp`, `BjtStageTests.cpp`)

- Finite output at 44.1–192 kHz, block sizes 1–4096, random settings; silence in gives silence out.
- Block-size independence (< 1e-5) at every rate.
- Clean-up: THD falls monotonically as the guitar rolls back (> 35 % on 10, < 20 % on 7, < 10 %
  on 5) while the level drops only 1–8 dB; rolling back brightens the tone.
- Bias: symmetric at noon (even/odd < 0.25), even harmonics grow away from noon; a starved bias
  gates quiet notes (> 15 dB quieter) but not loud ones; a starved fuzz chokes after a burst and
  recovers within a second.
- Ge against Si: silicon has more gain, more distortion, more upper harmonics and more highs.
- Level matching (±1 dB at volume 0.5), volume range ±20 dB.
- Aliasing below −60 dB re fundamental at full fuzz, 44.1 and 48 kHz.
- `latencySamples()` equals the oversampler's.
- Automating every knob, all at once, and flipping the transistor switch is click-free.
- The transistor stage: Newton against bisection, monotonic and bounded curves, table against the
  exact curve (< 1e-5), table integral against the closed-form antiderivative, ADAA averaging.

## Known limitations

- Quasi-static: the pair is one memoryless curve plus the servo; the coupling and feedback
  capacitors' interaction with the clipping is approximated by the filters around it.
- Temperature and leakage drift of real germanium are not modelled; the bias knob covers the
  range they move through.
- A DI recorded through an instrument input already carries the pickup's unloaded resonance; the
  guitar load here adds the loaded behaviour on top of it.
- CPU: two blended table lookups per oversampled sample put Remora at about two to three times
  Coral's cost (`"Pedal CPU cost"` in `engine/tests/pedals/PedalBenchmarks.cpp`; 0.5–1 µs per
  sample at 48 kHz on a heavily loaded machine, so treat the absolute figure as an upper bound).
