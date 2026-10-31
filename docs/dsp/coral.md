# Coral (distortion)

`engine/include/sedef/dsp/Coral.h`, `engine/src/dsp/Coral.cpp`

## What it models

A high-gain distortion: an op-amp gain stage (up to about +50 dB) into silicon diodes to ground,
followed by a passive low-pass "cut". Tight pre-emphasis keeps palm mutes defined and lets the pick
attack drive the clipper hardest; the cut control tames the fizz, getting darker clockwise.

## Signal flow

```
in ─► HP 100 Hz (Q 0.55) ─► high shelf +6 dB @ 1 kHz ─► × G ─► LP (GBW: 18→7 kHz)
   ─► [4× oversampled] diode clipper with rail-bounded tail (antiderivative anti-aliased)
   ─► DC block 15 Hz ─► cut LP (12→1.2 kHz, Q 0.6) ─► LP 10 kHz ─► × knee·trim·level ─► out
```

- **Pre-emphasis**: second-order high-pass at 100 Hz plus a first-order high shelf (+6 dB above
  1 kHz) before the gain, the classic emphasis/de-emphasis arrangement: tight lows, articulate
  attack; the post filters take the edge back off.
- **Op-amp bandwidth**: a gain-bandwidth-limited stage gets darker as its gain rises, modelled by a
  first-order low-pass whose corner falls from 18 kHz to 7 kHz with `gain`.
- **Clipper**, normalised to a diode knee of 1 (0.25 full scale):
  `0.94 · tanh(r) + 0.06 · R · tanh(r / R)`: a firm knee, then the slow rise of the diode law,
  bounded by the op-amp's output swing R (6 positive, 5.4 negative: a slightly off-centre bias that
  adds a little even-order content at extreme gain). The output is bounded whatever goes in.
- **Anti-aliasing**: 4× oversampling (2× at 96 kHz, 1× above 100 kHz) plus first-order
  antiderivative anti-aliasing (antiderivative in closed form via log-cosh, evaluated in double).
  At full gain, inharmonic products of a 1.3 kHz / 2.6 kHz note measure below −68 dB re the
  fundamental at 44.1/48 kHz, against −40…−46 dB with oversampling alone.

## Control mapping

| Control | Mapping |
|---|---|
| `gain` | 3 dB + 44 dB × gain, plus 3 dB from the emphasis shelf at 1 kHz: 6…50 dB at 1 kHz (linear in dB). Also moves the bandwidth corner 18 → 7 kHz. |
| `cut` | Second-order low-pass (Q 0.6), corner 12 kHz × (1/10)^cut: 12 kHz → 3.8 kHz at noon → 1.2 kHz. |
| `level` | 40 dB × (level − 0.5) (±20 dB) plus a gain-dependent trim, `trimDb(g) = −4.62 + 7.03 e^(−g/0.22)`, fitted so that at level 0.5 the guitar-like reference tone (eight harmonics of 147 Hz, −10 dBFS peak) comes out within ±0.3 dB of its input level at every gain. |

Gains are ramped per sample (30 ms); filter coefficients are recomputed every 32 samples while
their knob is moving. `latencySamples()` returns the oversampler's latency (37 samples at
44.1/48 kHz, 31 at 88.2/96 kHz, 0 above).

## References

- D. T. Yeh, J. S. Abel, J. O. Smith, "Simplified, Physically-Informed Models of Distortion and
  Overdrive Guitar Effects Pedals", *Proc. DAFx-07*, 2007 — diode clipper to ground, op-amp gain
  stage.
- D. T. Yeh, *Digital Implementation of Musical Distortion Circuits by Analysis and Simulation*,
  PhD thesis, Stanford University, 2009.
- J. Parker, V. Zavalishin, E. Le Bivic, *Proc. DAFx-16*, 2016; S. Bilbao, F. Esqueda, J. Parker,
  V. Välimäki, *IEEE Signal Processing Letters* 24(7), 2017 — antiderivative anti-aliasing.
- A. S. Sedra, K. C. Smith, *Microelectronic Circuits* — op-amp gain-bandwidth, diode law.
- The clipper curve, emphasis filters, gain law and trim are our own design.

## Tests (`engine/tests/pedals/CoralTests.cpp`)

- Finite output for random settings and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- Turning `cut` up monotonically removes energy above 5 kHz (> 15 dB from 0 to 1).
- THD rises monotonically with gain.
- Output peak stays below 0.6 FS at level 0.5 (below 6 FS at level 1) for full-scale noise and a
  +20 dBFS sine, for random gain/cut.
- Level 0.5 is within ±1.5 dB of unity for every gain.
- Aliasing below −60 dB re fundamental for high notes at full gain (fails without the ADAA).
- `latencySamples()` equals the oversampler's.
- Automating each knob (and all at once) is click-free.

## Known limitations

- Quasi-static: the op-amp's rail clipping is folded into one memoryless curve with the diodes,
  and slew-rate limiting is not modelled.
- At full gain the stage amplifies the noise floor by up to 50 dB; put the gate first (the rig
  does).
