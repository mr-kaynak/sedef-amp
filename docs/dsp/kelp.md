# Kelp (mid overdrive)

`engine/include/sedef/dsp/Kelp.h`, `engine/src/dsp/Kelp.cpp`

## What it models

The classic mid-hump overdrive: a non-inverting op-amp stage with two silicon diodes across its
feedback resistor, a strong low cut ahead of the clipping, then a passive low-pass and an active
treble control. Its output is the input plus whatever the feedback network lets the diodes keep,
so the lows pass clean while the midrange is pushed and clipped: the hump sits around 700 Hz to
1 kHz. With the drive down and the level up it is the familiar mid-pushed boost that tightens a
high-gain amp. With the drive up it is a warm, compressed, honking overdrive.

Kelp and [Pearl](pearl.md) share the clipping topology but not the voicing:

| | Pearl | Kelp |
|---|---|---|
| Clipping-path gain | 3 dB to 40 dB | 21.5 dB to 41.4 dB (the circuit's 51 k + 500 k over 4.7 k) |
| Diodes | ad-hoc curve, mismatched pair | diode law (1N4148 model), matched pair: symmetric |
| After the clipper | low-pass 1.5 to 8 kHz | 723 Hz passive low-pass, treble shelf |
| Extra | mid boost | none |

Pearl stays transparent at low gain; Kelp always has its hump.

## Signal flow

```
in ─► HP 20 Hz (input coupling) ─┬──────────── delay (= oversampler latency) ───────────┐
                                  │                                                       │
                                  └─► HP 720 Hz (R4 C3) ─► × Rf/R4 ─► LP 1/(2π Rf Cf) ─►   │
                                      [4× oversampled] diodes: v + a sinh v = w           │
                                      (first-order ADAA) ─► × n·Vt ─────────────────────► +
                                                                                          │
      out ◄─ × trim·level ◄─ HP 10 Hz ◄─ treble: y + (G−1)·HP 5 kHz(y) ◄─ LP 723 Hz ◄─────┘
```

- **Clipping stage** (quasi-static, after Yeh, Abel & Smith): with an ideal op-amp the inverting
  input follows the input, so the current through R4 + C3 (4.7 kΩ, 47 nF: a 720 Hz high-pass) is
  forced through the feedback network. Ignoring Cf, that network obeys
  `i Rf = V + 2 Is Rf sinh(V / n Vt)`: in thermal-voltage units `v + a sinh v = w`, with
  `w = i Rf / n Vt` (what Rf alone would develop) and `a = 2 Is Rf / n Vt`. The op-amp's output is
  `in + V`. Cf (51 pF) band-limits the clipping path ahead of the solver, its corner falling from
  61 kHz to 5.7 kHz as the drive rises.
- **Drive**: Rf = 51 kΩ + 500 kΩ × an audio taper (`(e^{4d} − 1)/(e^4 − 1)`, 12 % at noon). It sets
  both the gain (21.5 → 41.4 dB small-signal into the diodes) and `a`, so the diodes' knee drops
  from about 0.36 V to 0.24 V as the drive rises, as in the circuit.
- **Diodes**: 1N4148/1N914 SPICE parameters, Is = 2.52 nA, N = 1.752, Vt at 27 °C. Two in
  anti-parallel: the curve is odd, so the clipping is symmetric (even harmonics stay below
  −150 dB; see the table).
- **Solver**: `v + a sinh v = w` is convex and increasing for v ≥ 0, and both `w` and
  `asinh(w/a)` bound its root from above. Two Halley steps from the smaller bound in float land
  within 4e-6 (relative residual) over w = 1e-6 … 1e6 and the whole drive range; one Newton step
  in double squares that error. sinh and cosh at the root follow by Taylor expansion.
- **Anti-aliasing**: 4× oversampling (2× at 96 kHz, 1× above 100 kHz) and first-order
  antiderivative anti-aliasing. The antiderivative of the implicit curve is closed-form in `v`:
  `F(w) = v²/2 + a (v sinh v − cosh v + 1)` (from `dw = (1 + a cosh v) dv`). When the drive moves,
  `a` steps once per 32 base-rate samples and the stored antiderivative is re-evaluated, so the
  difference quotient stays consistent.
- **Tone stage**: the 1 kΩ / 220 nF passive low-pass (723 Hz), then the active treble control,
  modelled as a first-order shelf `(1 + s/ωz)/(1 + s/ωp)`, pole at 5 kHz, zero moving from 5 kHz
  (dark: only the low-pass is left) to 560 Hz (bright). That shelf equals `1 + (G − 1)·HP(ωp)`
  with `G = ωp/ωz`, so it runs as a fixed 5 kHz high-pass blended in with a per-sample smoothed
  gain: turning the knob never touches a filter coefficient (recomputing the shelf every 32 samples
  measured 12× the static click indicator in the sweep test).
- The clean term is delayed by exactly the oversampler latency so the two paths sum coherently.

## Control mapping

| Control | Mapping |
|---|---|
| `drive` | Rf = 51 k + 500 k · audio taper (see above): gain and diode knee. |
| `tone` | Treble blend G − 1 with G = (5000/560)^tone: 0 dB (dark) to +19 dB above 5 kHz, against the 723 Hz low-pass. |
| `level` | 40 dB × (level − 0.5) (±20 dB) plus a trim, `trimDb(d) = −4.11 − 1.26 d`, fitted so that at level 0.5 the reference tone (eight harmonics of 147 Hz, −10 dBFS peak) comes out within ±0.1 dB of its input level at every drive. |

Gains ramp per sample over 30 ms; the drive's filter corner and diode curve follow a 20 ms
smoother, updated every 32 samples. `latencySamples()` returns the oversampler's latency (37
samples at 44.1/48 kHz, 31 at 88.2/96 kHz, 0 above).

## Measurements (48 kHz)

Small-signal response, tone and level at noon (dB re input):

| drive | 100 Hz | 200 Hz | 400 Hz | 720 Hz | 1 kHz | 2 kHz | 4 kHz | peak |
|---|---|---|---|---|---|---|---|---|
| 0 | +1.2 | +5.9 | +10.2 | +12.0 | +12.0 | +10.5 | +8.1 | 848 Hz |
| 0.5 | +5.8 | +11.3 | +15.7 | +17.6 | +17.6 | +16.1 | +13.7 | 848 Hz |
| 1 | +18.1 | +23.8 | +28.3 | +30.1 | +30.1 | +28.2 | +24.6 | 823 Hz |

At drive 0 the bass at 100 Hz sits 10.8 dB under the hump and 4 kHz 3.9 dB under it. Tone (4 kHz
against 800 Hz, drive 0): −9.3 dB at 0, −4.0 dB at noon, +1.1 dB at 10.

Harmonics of a 440 Hz sine at −20 dBFS peak (a firm single note), tone at 10 (dB re fundamental):

| drive | THD | H2 | H3 | H4 | H5 | H7 | +peak / −peak |
|---|---|---|---|---|---|---|---|
| 0 | 13.0 % | −160 | −17.8 | −177 | −33.7 | −61.6 | 1.000 |
| 0.25 | 17.5 % | −160 | −15.5 | −172 | −26.6 | −39.2 | 1.000 |
| 0.5 | 23.7 % | −161 | −13.3 | −174 | −21.1 | −27.9 | 1.000 |
| 0.75 | 28.6 % | −161 | −12.2 | −173 | −18.4 | −23.2 | 1.000 |
| 1 | 30.7 % | −161 | −11.8 | −172 | −17.4 | −21.7 | 1.000 |

The clipping softens as the drive goes down: THD falls and the higher harmonics fall faster (H5
is 16 dB under H3 at drive 0, 6 dB under it at full drive). At drive 0 a single note at an
ordinary picking level (330 Hz, −26 dBFS peak, 50 mV) has 1.7 % THD, and Level at 10 adds 20 dB:
the clean, mid-pushed boost. Aliasing at full drive: inharmonic products of a 1.3 kHz / 2.6 kHz
note at −91 / −82 dB re the fundamental.

In front of Abyss at gain 5.5 (Holdfast's pedal setting: drive 0, tone 5.5, level 8),
the output loudness stays within 0.7 dB of Abyss alone. On a low single-note riff the amp's
400 Hz–1.5 kHz band rises by 2.0 dB and 150–400 Hz drops by 1.0 dB: the amp is driven by the
mids rather than the low strings.

CPU: about 2.4× Pearl (4× oversampled diode-law solve), measured with `[.bench]`.

## Where it goes in the chain

The last drive before the distortion and the amp: Trench → Tide → Urchin → Pearl → **Kelp** →
Coral → Ripple. That is the engine's fixed order until the pedalboard lets the player choose; it
is also the order we recommend.

- **Last before the amp.** A mid-hump boost works on whatever follows it. Its bass cut (100 Hz is
  10.8 dB under the hump at drive 0) and its mid push decide what the amp's first stage sees.
  Only Coral, when it is on, and Ripple sit between Kelp and the amp.
- **After Pearl.** Pearl stays transparent at low gain, so it can push Kelp harder. The other way
  round, Kelp's hump would colour everything Pearl clips.
- **After the octave pedals.** A drive in front of them spoils their tracking and their octave
  (see [trench.md](trench.md#where-it-goes-in-the-chain) and
  [urchin.md](urchin.md#where-it-goes-in-the-chain)).
- **Ahead of Coral.** Into Coral it does what it does for a high-gain amp: a tighter,
  mid-forward distortion.

## References

- D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and
  Overdrive Guitar Effects Pedals", *Proc. DAFx-07*, 2007 — the op-amp clipping stage with diodes
  in the feedback loop, its component values, the quasi-static approximation and the tone stage.
- D. T. Yeh, *Digital Implementation of Musical Distortion Circuits by Analysis and Simulation*,
  PhD thesis, Stanford University, 2009.
- 1N4148 / 1N914 small-signal diode SPICE model (Is = 2.52 nA, N = 1.752), as published in diode
  manufacturers' model libraries and datasheets.
- J. Parker, V. Zavalishin, E. Le Bivic, "Reducing the Aliasing of Nonlinear Waveshaping Using
  Continuous-Time Convolution", *Proc. DAFx-16*, 2016; S. Bilbao, F. Esqueda, J. Parker,
  V. Välimäki, "Antiderivative Antialiasing for Memoryless Nonlinearities", *IEEE Signal
  Processing Letters* 24(7), 2017.
- A. S. Sedra, K. C. Smith, *Microelectronic Circuits* — non-inverting op-amp stage, diode law.
- The solver, the treble-shelf model, the drive taper and the trim are our own.

## Tests (`engine/tests/pedals/KelpTests.cpp`)

- Finite output for random settings and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- The solver satisfies `v + a sinh v = w` to 1e-9 over six decades of `w` at three drives, and the
  antiderivative's numerical slope equals the solution.
- The small-signal hump peaks between 600 Hz and 1.2 kHz at drive 0 and noon, with 100 Hz at
  least 9 dB and 4 kHz at least 3 dB below it.
- Tone brightens clockwise (4 kHz against 800 Hz, > 4 dB per half turn).
- Symmetric clipping: H2 and H4 below −40 dB and peaks equal within 2 % at every drive; THD and
  H5/H3 rise with the drive.
- Drive 0: < 3 % THD for a 50 mV single note, ≥ 12 dB of boost at Level 10.
- Level 0.5 within ±1.5 dB of unity across the drive range.
- Aliasing below −60 dB for high notes at full drive.
- `latencySamples()` equals the oversampler's; the clean and clipping paths are aligned (the test
  fails with the delay removed: 9 turns in the response instead of ≤ 2).
- Automating each knob, and all at once, is click-free.

## Known limitations

- Quasi-static: Cf's interaction with the diodes (a frequency-dependent clipping threshold) is
  approximated by band-limiting ahead of the solver; the op-amp's slew rate and rails are not
  modelled.
- The treble control is a first-order approximation of the active tone circuit, not its exact
  transfer function.
- The trim is fitted to one steady reference tone. Like the circuit, the pedal is louder relative
  to bypass on soft playing (more of the signal stays under the knee).
