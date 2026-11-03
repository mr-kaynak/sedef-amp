# Conch (wah)

`engine/include/sedef/dsp/Conch.h`, `engine/src/dsp/Conch.cpp`

## What it models

The classic inductor wah: a transistor stage with an inductor-capacitor resonance whose feedback a
rocker pot changes, sweeping a vocal band-pass peak from about 350 Hz at the heel to 2.2 kHz at the
toe. In **Auto** mode the picking envelope rocks the pedal for you, from where the rocker sits
upwards (an auto-wah with the wah's own voice).

## Chain position

**Before the drives** (after the gate and Anemone, before Tide). A wah in front of the drives is
the classic order: the drive saturates the moving peak, which is what gives the cocked, vocal
sound, and Tide after it evens out the level the peak adds. After a high-gain drive the wah would
filter the distortion's fizz instead.

## Signal flow

```
in ──────────────────────────► TPT state-variable filter ─ band ─► × trim · f0 / 300 Hz ─► out
 │                                   ▲ f0 = 350 Hz · (2200 / 350)^p
 │                                   │ Q  = Qmid · 2.5^(1 − 2p)
 └─► |x| ─► peak envelope (4 ms / 150 ms) ─► Auto: p = rocker + (1 − rocker) · (1 − e^(−env·g))
```

- **Response.** The published small-signal responses of the inductor wah (Holters and Zölzer,
  DAFx-11, Figure 4) share one +6 dB/oct skirt below the resonance at every pot position, and the
  peak stays at about the same height (+24 dB at the heel, +20 dB at the toe) because the
  resonance is sharp at the heel (Q about 10) and broad at the toe (Q about 1.4). Conch keeps both:
  `H = (f0 / fx) · BP(f0, Q)` with BP the state-variable band-pass (peak Q, +6 dB/oct below), so the
  low skirt is `f / fx` whatever the position, and Q falls 2.5× each side of mid travel, so
  `Q · f0` and with it the peak height stay constant. A trim (−8.5 dB) puts the guitar reference
  tone near unity at mid travel.
- **Filter.** A trapezoidal (topology-preserving) state-variable filter, recomputed every sample:
  it stays clean when the cutoff and damping move per sample, which a direct-form biquad does not.
- **Rocker.** Host automation arrives in block-sized steps; the rocker glides (12 ms time
  constant), so a stepped or fast sweep never zippers.
- **Auto.** A peak detector (4 ms attack, 150 ms release) on the input drives the rocker up from
  the Position: `p = rocker + (1 − rocker) · (1 − e^(−env · g))`, `g = 10^((−6 + 42·sens)/20)`. The
  detector runs in Manual too, so switching to Auto mid-note picks up where the note is. The
  Manual/Auto switch crossfades over 40 ms with a smoothstep curve.
- Linear: no oversampling, no latency.

## Control mapping

| Control | Mapping |
|---|---|
| `conchPosition` | Rocker, heel 0 to toe 1: f0 = 350 Hz → 2.2 kHz, exponential (877 Hz at noon). |
| `conchResonance` | Q at mid travel, 2 → 8 exponential (4 at noon); ×2.5 at the heel, ÷2.5 at the toe. |
| `conchMode` | Manual, or Auto (the envelope rocks the pedal up from the Position). |
| `conchSensitivity` | Auto only ("Auto sensitivity"): detector gain −6 dB → +36 dB. |

## Measurements (48 kHz, Resonance at noon; `engine/tests/pedals/ModsReport.cpp`)

| Position | Peak | Q | Peak gain | 100 Hz | Guitar reference tone |
|---|---|---|---|---|---|
| 0 (heel) | 350 Hz | 10.0 | +12.8 dB | −17.3 dB | −4.7 dB |
| 0.25 | 554 Hz | 6.3 | +12.9 dB | −17.8 dB | −1.1 dB |
| 0.5 | 878 Hz | 4.0 | +12.9 dB | −17.9 dB | −1.0 dB |
| 0.75 | 1389 Hz | 2.5 | +12.9 dB | −18.0 dB | −4.1 dB |
| 1 (toe) | 2200 Hz | 1.6 | +12.9 dB | −18.1 dB | −6.7 dB |

The 1/k-harmonic reference tone is darker than a real pickup, so it reads the toe as quieter than
a guitar does; a broadband (pink) input reads it about as much louder.

Cost (`[mods-bench]`, 48 kHz mono): 27 ns a sample in Manual, 44 ns in Auto, 0.13 % and 0.21 % of
one core.

## References

- M. Holters, U. Zölzer, "Physical Modelling of a Wah-wah Effect Pedal as a Case Study for
  Application of the Nodal DK Method to Circuits with Variable Parts", *Proc. DAFx-11*, Paris,
  2011: the inductor wah's circuit, component values and measured small-signal responses.
- J. O. Smith, *Making Virtual Electric Guitars and Associated Effects Using Faust*, CCRMA,
  Stanford (online): its wah-pedal section, a resonant filter swept by the pedal.
- V. Zavalishin, *The Art of VA Filter Design*, Native Instruments, 2012/2018: topology-preserving
  (trapezoidal) state-variable filter.
- A. Simper, "Linear Trapezoidal Integrated State Variable Filter", Cytomic technical paper, 2013.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011: wah-wah filter.
- The Q-against-travel law (2.5× either side), the trim and the Auto mapping are our own fit.

## Tests (`engine/tests/pedals/ConchTests.cpp`)

- Finite output for random settings and full-scale noise (44.1/48/96 kHz, blocks 1/17/64/512,
  mono and stereo); silence in gives silence out.
- The measured peak lands on the mapped f0 within 1 % and its −3 dB Q within 6 % of `qAt`, for
  five positions and two Resonance settings at 44.1, 48, 88.2, 96 and 192 kHz.
- One low skirt: heel and toe agree within 0.6 dB at 40 Hz and rise 6 dB/oct; the peak height is
  +12.9 dB ± 0.3 dB at every position.
- Rocker sweeps are zipper-free: the click indicator stays within the static settings' own, and a
  fast 2 Hz heel-to-toe rock of a 220 Hz tone puts nothing above 6 kHz (−80 dB). Both checks fail
  with the rocker's glide removed.
- Auto: a held level A puts the rocker at `p + (1 − p) · travel(A)`; harder picking and more
  sensitivity open it further; silence brings it back to the Position. Manual ignores the envelope.
- Resonance, Sensitivity and Manual/Auto automation are click-free; no harmonics or aliases
  (THD < 0.01 %, inharmonic < −75 dB at 0.9 FS); output identical for block sizes 1 to 4096.

## Known limitations

- Small-signal (linear) model: the real circuit's transistor saturates at the peak for hot inputs
  (about 6 dB above the small-signal level, per the paper); Conch stays clean.
- No MIDI expression input yet; `conchPosition` is automatable, so a host can drive it.
