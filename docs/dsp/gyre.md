# Gyre (phaser)

`engine/include/sedef/dsp/Gyre.h`, `engine/src/dsp/Gyre.cpp`

## What it models

The classic four-stage all-pass phaser: identical first-order all-pass stages swept together by an
LFO, mixed 50/50 with the dry signal, so the frequencies the stages turn through an odd multiple
of 180° cancel. Four stages cut two notches. A Stages switch adds two more stages, for a third
notch and a denser swirl, and a Feedback control returns the wet signal to the input, which raises
resonant peaks between the notches.

## Chain position

**After the drives, before the amp** (after Coral: Beacon, Gyre, Jetty, then Ripple). A
phaser after the drive moves the notches through a harmonically rich signal, where they are
heard; before a high-gain drive, the drive's compression and new harmonics fill the notches back in.
Gyre sits after Beacon (the warmer, lopsided throb) and before Jetty (whose comb has the most
teeth), so the modulation grows denser along the chain.

## Signal flow

```
in ─► (+) ─┬──────────────────────────────────────────────┬─► ½ (v + wet) · trim ─► out
       ▲   └─► AP ─► AP ─► AP ─► AP ─┬─► AP ─► AP ─► (4/6 crossfade) ─► wet
       │                             └────────────────────────────────┘     │
       └──────────────── × feedback (0 … 0.85) ◄──────────────────────────────┘
           AP: TPT one-pole all-pass, break = 600 Hz · 2^(2 · depth · tri(LFO))
```

- **Stages.** Topology-preserving first-order all-passes, all at one break frequency, recomputed
  every sample. Each turns the phase from 0 to −180° through its break. Four stages make notches
  where `4φ = −π` and `−3π`: at `fc · tan(π/8)` and `fc · tan(3π/8)`, about 0.41 and 2.41 times the
  break (249 Hz and 1.45 kHz at the 600 Hz centre). Six stages: `fc · tan(π/12)`, `fc` and
  `fc · tan(5π/12)` (161 Hz, 600 Hz and 2.24 kHz).
- **Sweep.** A triangle LFO in octaves, ±2 octaves at full Depth: the notches move at a constant
  musical rate and turn round at the ends without a step. The LFO phase is kept in double
  precision, so 0.1 Hz holds its rate at 192 kHz.
- **Feedback.** The wet output is added back at the input; the dry half of the mix is taken after
  that sum, so the notches stay exactly where the stages put them and feedback only raises the
  peaks between them (by up to 1 / (1 − 0.85), +16.5 dB less the trim). The knob is capped at
  0.85.
- **Level.** The 50/50 mix loses about 3.5 dB of a guitar into the notches, and feedback adds it
  back at the peaks: `trim = 1.5 · (1 − 0.5 · feedback)` keeps the guitar reference tone within
  about 2 dB of unity across the Feedback travel.
- **Stages switch.** Stages 5 and 6 always run; the switch crossfades between the fourth and the
  sixth stage's output over 30 ms (smoothstep), so it never clicks.
- Linear: no oversampling, no latency.

## Control mapping

| Control | Mapping |
|---|---|
| `gyreSpeed` | LFO rate 0.1 → 8 Hz, skewed so noon is about 1 Hz. |
| `gyreDepth` | Sweep width, 0 → ±2 octaves about 600 Hz (±1.6 octaves at the default 0.8). |
| `gyreFeedback` | 0 → 0.85 fed back. |
| `gyreStages` | 4 stages (two notches) or 6 stages (three notches). |

## Measurements (48 kHz; `engine/tests/pedals/ModsReport.cpp`)

| Stages | Notches at the centre (Depth 0) | Guitar level, Feedback 0 / 0.5 / 1 |
|---|---|---|
| 4 | 249 Hz, 1.45 kHz | −0.1 / −0.9 / −0.7 dB |
| 6 | 161 Hz, 600 Hz, 2.24 kHz | +0.1 / −0.1 / +2.2 dB |

At full Depth the break sweeps from 150 Hz to 2.4 kHz. Cost: 37 ns a sample, 0.18 % of one core
(48 kHz mono), with four or six stages.

## References

- J. O. Smith, *Physical Audio Signal Processing*, CCRMA, Stanford (online): "Phasing with
  First-Order Allpass Filters", the notch condition and the classic four-stage structure.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011: phaser.
- V. Zavalishin, *The Art of VA Filter Design*, Native Instruments, 2012/2018: the
  topology-preserving one-pole all-pass.
- The level trim and the sweep law are our own.

## Tests (`engine/tests/pedals/GyreTests.cpp`)

- Finite for random settings and full-scale noise; silence in gives silence out.
- Notch count and positions: two notches with four stages and three with six, each within 1 % of
  the formula and deeper than 40 dB, at 44.1, 48, 88.2, 96 and 192 kHz.
- Feedback leaves the notches within 1 % of where they were and lifts the peak between them by more
  than 8 dB, but less than 16 dB, at full Feedback.
- The LFO runs at the Speed within 1 % (0.1, 1 and 8 Hz at 44.1, 96 and 192 kHz); full Depth sweeps
  the break to 150 Hz and 2.4 kHz (within 1 %) and half Depth one octave either way; in the audio a
  150 Hz tone dips twice per LFO period, the period repeating.
- Stable at full Feedback with both stage counts at every supported rate: full-scale noise stays
  bounded (peak < 6) and rings out to silence.
- The guitar reference tone stays near unity across Feedback; Speed, Depth, Feedback and Stages
  automation are click-free.
- No harmonics or aliases from a static setting (THD < 0.01 %, inharmonic < −75 dB); output
  identical for block sizes 1 to 4096.

## Known limitations

- Ideal, matched stages: no component spread between stages and no transistor colour.
- One LFO for both channels: no stereo spread yet.
