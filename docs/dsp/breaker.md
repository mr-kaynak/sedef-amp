# Breaker (treble booster)

`engine/include/sedef/dsp/Breaker.h`, `engine/src/dsp/Breaker.cpp`, shared transistor stage in
`engine/include/sedef/dsp/detail/BjtStage.h`

## What it models

The classic single-transistor germanium treble booster: a small input capacitor into a
common-emitter stage. The capacitor and the stage's low input resistance form, with the guitar's
pickup, a resonant band-pass: the lows never reach the transistor, the upper mids get the whole
gain. Played into a driven amp it tightens and brightens it. **Range** swaps the input capacitor
for a larger one (Full), which lets the lows in for a broad boost. **Boost** bypasses more of the
emitter resistor: more gain, and the junction's exponential curve exposed, so the stage clips
softly and lopsidedly when pushed.

## Signal flow

```
in ─► input network: pickup (6 kΩ, 2.5 H) + C (4.7 nF Treble | 47 nF Full) into 7.5 kΩ
      (both ranges run; the switch crossfades over 20 ms)
   ─► [4× oversampled] common-emitter stage, implicit Ebers–Moll curve with emitter feedback
       (tabulated, antiderivative anti-aliased)
   ─► DC block 16 Hz ─► bandwidth LP 12 kHz ─► out (no trim: the boost is the point)
```

- **Input network**: the voltage across the stage's input resistance is the band-pass
  `H(s) = sC·Rin / (s²LC + sC(Rp + Rin) + 1)`, centred where the capacitor resonates with the
  pickup's inductance (1468 Hz with 4.7 nF, 464 Hz with 47 nF), with a peak gain of
  Rin / (Rin + Rp) = −5.3 dB. Discretised with the bilinear transform, prewarped at the centre.
- **Stage**: the same normalised curve as Remora (`detail::BjtStage.h`), `w = ln(1 + x) + g·x +
  W(x)`. Cutoff headroom 2.4 V, saturation headroom 5.5 V (the swing of the published
  germanium booster waveforms), thermal voltage 1.17 × 25.5 mV, open-loop gain gm·Rc = 2.4 V / Vt
  (about 38 dB). Boost asks for a closed-loop gain; the emitter feedback's loop gain follows:
  `g = gm·Rc / gain − 1`. Small g leaves the junction's curve exposed, so distortion grows with
  boost and is dominated by the second harmonic. 33 tables span the boost knob, shared by every
  instance.
- **Onset**: after a reset the input fades in over 5 ms, so the resonant input network is not
  struck by a step when the pedal is switched on.
- **Anti-aliasing**: 4× oversampling (2× at 88.2/96 kHz, 1× above) plus first-order
  antiderivative anti-aliasing.

## Control mapping

| Control | Mapping |
|---|---|
| `breakerBoost` | Stage gain 8 + 28 × boost dB (linear in dB): 8 → 36 dB. With the input network's −5.3 dB at its centre, the pedal's peak gain is 2.9 → 30.9 dB. Smoothed over 20 ms. |
| `breakerRange` | Treble (4.7 nF, centre 1468 Hz) or Full (47 nF, centre 464 Hz), crossfaded over 20 ms. |

`latencySamples()` is the oversampler's.

## Measurements (48 kHz, `engine/tests/pedals/BreakerTests.cpp`)

Small-signal gain (dB):

| Range, boost | 100 Hz | 200 Hz | 400 Hz | 700 Hz | 1 kHz | 1.5 kHz | 3 kHz | 6 kHz | 10 kHz |
|---|---|---|---|---|---|---|---|---|---|
| Treble, 0 | −25.2 | −19.0 | −12.5 | −6.5 | −1.6 | 2.8 | −6.5 | −14.8 | −21.8 |
| Treble, 0.5 | −11.2 | −5.0 | 1.5 | 7.5 | 12.4 | 16.8 | 7.5 | −0.8 | −7.8 |
| Treble, 1 | 2.8 | 9.0 | 15.5 | 21.5 | 26.4 | 30.8 | 21.5 | 13.2 | 6.2 |
| Full, 0 | −5.5 | −0.2 | 2.8 | 2.1 | 0.2 | −2.6 | −8.4 | −15.2 | −21.9 |
| Full, 0.5 | 8.5 | 13.8 | 16.8 | 16.1 | 14.2 | 11.4 | 5.6 | −1.2 | −7.9 |
| Full, 1 | 22.5 | 27.8 | 30.8 | 30.1 | 28.2 | 25.4 | 19.6 | 12.8 | 6.1 |

Treble: peak between 1.3 and 1.65 kHz, −3 dB corner on the low side between 950 and 1250 Hz,
100 Hz at least 24 dB below the peak. Full: peak between 380 and 560 Hz, low corner between 160 and
250 Hz. The digital response follows the analogue network (times the 12 kHz roll-off) within
0.5 dB from 100 Hz to 6 kHz.

Soft clipping: Treble, a 1.5 kHz sine (the band's centre).

| Input | Boost 0 | Boost 0.5 | Boost 1 |
|---|---|---|---|
| 0.01 FS | 0.00 % | 0.11 % | 2.78 % (H2 0.028, H3 0.0001) |
| 0.03 FS | 0.01 % | 0.33 % | 7.88 % (H2 0.079, H3 0.0007) |
| 0.1 FS | 0.04 % | 1.13 % | 16.7 % (H2 0.157, H3 0.050) |
| 0.3 FS | 0.13 % | 4.21 % | 26.3 % (H2 0.105, H3 0.220), peak 4.2 |

Distortion grows smoothly with level and with boost; second harmonic first, then the third as the
stage reaches saturation; the output is bounded by the transistor's swing (below 5.6 FS).

Aliasing at full boost, worst inharmonic component re the fundamental, 0.3 FS sine: −111.2 /
−112.2 dB (1318.5 / 2637 Hz at 44.1 kHz), −109.2 / −108.9 dB (at 48 kHz).

Level on the reference guitar tone at boost 0.5: +1.6 dB (Treble, its energy sits below the band),
+13.9 dB (Full).

## References

- B. Holmes, M. Holters, M. van Walstijn, "Comparison of Germanium Bipolar Junction Transistor
  Models for Real-time Circuit Simulation", *Proc. DAFx-17*, 2017 — the treble booster case study:
  germanium Ebers–Moll parameters, the input network with a guitar pickup, published waveforms
  (about +5.5 / −2.5 V swing, about 30 dB at the band's centre with a low-impedance source).
- B. Holmes, M. van Walstijn, treble booster parameter optimisation, *Proc. DAFx-16*, pp. 47–54,
  2016.
- J. J. Ebers, J. L. Moll, *Proc. IRE* 42(12), 1954 — the junction law.
- A. S. Sedra, K. C. Smith, *Microelectronic Circuits* — emitter degeneration, input resistance of
  a common-emitter stage, series RLC band-pass.
- J. Parker, V. Zavalishin, E. Le Bivic, *Proc. DAFx-16*, 2016; S. Bilbao, F. Esqueda, J. Parker,
  V. Välimäki, *IEEE Signal Processing Letters* 24(7), 2017 — antiderivative anti-aliasing.
- The gain law, headroom normalisation and component values are our own, set against the
  published figures above.

## Chain position

**Last, straight into the amp.** A treble booster is a tone shaper for a driven amp: it should see
the whole board's output and hand the amp its boosted upper mids. In front of a fuzz or
overdrive it would be clipped flat by them.

## Tests (`engine/tests/pedals/BreakerTests.cpp`)

- Finite output at 44.1–192 kHz, block sizes 1–4096, random settings; silence in gives silence out.
- Block-size independence (< 1e-5) at every rate and range.
- The peak, low corner and rejection of each range at named frequencies (see above).
- Peak gain equals `stageGainDb + 20 log10(Rin / (Rin + Rp))` within 0.5 dB at boost 0, 0.5, 1,
  both ranges; 2.9 and 30.9 dB at 1470 Hz.
- The digital network matches the analogue one within 0.5 dB.
- Clean at minimum boost (< 0.5 % THD at 0.1 FS); distortion rises with boost and with level,
  dominated by H2 (> 3× H3 at 0.03 FS, full boost); output bounded by the swing.
- Aliasing below −60 dB re fundamental at full boost, 44.1 and 48 kHz.
- `latencySamples()` equals the oversampler's; automating Boost and flipping Range both ways is
  click-free.

## Known limitations

- No output trim: at full boost the peak is about 31 dB above the input. Turn Boost down, or the
  amp's input, for a clean amp.
- The stage is quasi-static and its gain is flat across the band: the emitter bypass capacitor's
  own frequency dependence (more gain higher up) is not modelled; the input network does the
  shaping.
- CPU: about two to three times Coral's cost, from the blended table lookups at 4×
  (`"Pedal CPU cost"` in `engine/tests/pedals/PedalBenchmarks.cpp`).
- A DI recorded through an instrument input already carries the pickup's unloaded resonance; the
  input network adds the loaded resonance on top of it.
