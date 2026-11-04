# Anemone (envelope filter)

`engine/include/sedef/dsp/Anemone.h`, `engine/src/dsp/Anemone.cpp`

## What it models

The classic envelope filter, or auto-wah: a resonant state-variable filter whose cutoff follows
how hard you pick. A fast attack makes the "quack" on every note, a slower release lets the filter
fall back as the note dies (the "ow"). Low-pass, band-pass or high-pass, sweeping up or down.

## Chain position

**Early, before the drives** (after Trench, before Conch, Remora and Tide). An envelope filter
reads the picking dynamics, so it wants the clean, dynamic guitar: after a fuzz or Tide the
envelope is flattened, and the filter barely moves. Trench, the octave down, goes first because its tracking
needs the plain guitar; it keeps the picking dynamics, and an octave into an envelope filter is
the classic synth-bass sound. Anemone comes before Conch because both sweep with the pick, and the
envelope one needs the untouched dynamics more.

## Signal flow

```
in ─┬───────────────────────────────────────► TPT SVF ─► LP / BP·(1/Q) / HP ─► × level(mode, Q) ─► out
    │                                           ▲ cutoff, Q per sample
    └─► max |x| over channels ─► peak detector (1 ms / 120 ms) ─► smoother (3 ms)
            ─► travel = 1 − e^(−env·g(sens)) ─► cutoff = 200 Hz · 2^(octaves · travel)   (Down: 1 − travel)
```

- **Envelope.** A peak detector with a 1 ms attack and a 120 ms release, then a 3 ms smoother. The
  smoother takes out the detector's ripple at twice the note's frequency, which would otherwise
  wobble the cutoff and add harmonics; it costs about 3 ms of attack. Stereo is detected as the
  louder channel, so both channels sweep together.
- **Mapping.** `travel = 1 − e^(−env · g)`, `g = 10^((−6 + 42·sens)/20)`, saturates smoothly: a
  hard pick opens the filter fully, without a hard ceiling. `cutoff = 200 Hz · 2^(octaves · travel)`
  with `octaves = 1 + 4 · range` (1 to 5 octaves, so up to 6.4 kHz). Down mirrors the travel: the
  filter starts open and closes on the pick.
- **Filter.** A trapezoidal state-variable filter, recomputed every sample, with `k = 1/Q`. The
  band-pass output is divided by Q so its peak stays at unity. Mode and Direction crossfade over
  30 ms with a smoothstep curve.
- Linear for a steady note: no oversampling, no latency.

### Level

Resonance lifts the low- and high-pass peaks Q times and narrows the band-pass. A per-mode makeup
and a power of `Q / 0.707` keep plucked notes within about 1 dB across the Resonance travel:

| Mode | Makeup | Power of Q / 0.707 |
|---|---|---|
| Low-pass | 0 dB | −0.2 |
| Band-pass | +7.0 dB | +0.45 |
| High-pass | +9.7 dB | −0.6 |

They were fitted on plucked, decaying notes, because a filter that sweeps with the envelope reads
a held tone and a played note differently (the cutoff falls through the harmonics as the note
dies). High-pass removes the fundamental, so it sits about 2 dB below the others on purpose.

## Control mapping

| Control | Mapping |
|---|---|
| `anemoneSensitivity` | Envelope gain −6 dB → +36 dB: how far a given pick opens the filter. |
| `anemoneRange` | Sweep width, 1 → 5 octaves above 200 Hz (3.4 octaves, to 2.1 kHz, at the default 0.6). |
| `anemoneResonance` | Q 0.707 → 16, exponential (3.4 at noon). |
| `anemoneMode` | Low-pass, Band-pass or High-pass. |
| `anemoneDirection` | Up (opens on the pick) or Down (closes on the pick). |

## Measurements (48 kHz; `engine/tests/pedals/ModsReport.cpp`)

Plucked-note level against Resonance (Sensitivity at noon, Range 0.6, direction Up):

| Resonance | Low-pass | Band-pass | High-pass |
|---|---|---|---|
| 0 | −0.3 dB | +0.3 dB | −2.6 dB |
| 0.25 | −0.6 dB | +0.2 dB | −1.8 dB |
| 0.5 | −0.7 dB | 0.0 dB | −1.7 dB |
| 0.75 | −0.4 dB | −0.2 dB | −1.9 dB |
| 1 | +0.4 dB | −0.4 dB | −2.2 dB |

Response time (0.3 FS note after silence): the envelope rises 10 to 90 % in under 10 ms, and the
cutoff gets more than 80 % of its way up (in octaves) within 10 ms of the pick; it falls 90 to 10 %
in 200 to 320 ms after the note stops. Cost: 61 ns a sample, 0.29 % of one core (48 kHz mono).

## References

- V. Zavalishin, *The Art of VA Filter Design*, Native Instruments, 2012/2018: the
  topology-preserving state-variable filter and why it tolerates per-sample modulation.
- A. Simper, "Linear Trapezoidal Integrated State Variable Filter", Cytomic technical paper, 2013.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011: envelope detection and
  adaptive (envelope-controlled) filters.
- D. Giannoulis, M. Massberg, J. D. Reiss, "Digital Dynamic Range Compressor Design: A Tutorial
  and Analysis", *J. Audio Eng. Soc.* 60(6), 2012: peak detectors with separate attack and release.
- The saturating travel law, the 1 ms / 120 ms voicing and the level table are our own.

## Tests (`engine/tests/pedals/AnemoneTests.cpp`)

- Finite for random settings and full-scale noise; silence in gives silence out.
- The mapping: `cutoffHz` and `travel` match the formulas at their ends; with a held level, the
  filter's measured band-pass peak lands on the mapped cutoff within 1 % (and its Q within 6 %),
  for Up and Down, two sensitivities and three levels. Harder picking moves Up up and Down down.
- The modes: low- and high-pass fall more than 33 dB three octaves into their stop band, the
  low-pass passes its pass band at 0 dB ± 0.5 dB, and the band-pass peak stands 15 dB above both
  sides.
- Response time: attack under 10 ms, release 200 to 320 ms, the cutoff 80 % of the way up within
  10 ms.
- Stable at full Resonance (Q 16) and full Sensitivity and Range, in every mode and direction, at
  44.1 to 192 kHz: full-scale noise stays bounded (peak < 12) and rings out to silence.
- Plucked notes stay within 1.5 dB of unity (high-pass of −2 dB) at Resonance 0, 0.5 and 1.
- Sensitivity, Range, Resonance, Mode and Direction automation are click-free.
- A held note gets no harmonics from the envelope (THD < 0.1 %); output identical for block sizes
  1 to 4096.

## Known limitations

- The envelope is a peak detector, not the rectifier-and-capacitor of a particular circuit; the
  voicing is by ear and by the response-time test, not matched to one pedal.
- A very fast, hard-picked passage keeps the filter near the top; lower Sensitivity for that.
