# Jetty (flanger)

`engine/include/sedef/dsp/Jetty.h`, `engine/src/dsp/Jetty.cpp`

## What it models

A bucket-brigade (BBD) flanger: a short delay, 0.1 to 16 ms, swept by a slow LFO and mixed 50/50
with the dry signal, so a comb of notches sweeps through the sound, the "jet". Feedback (regen)
sharpens the comb into resonant teeth, in either polarity: positive rings at multiples of 1/delay,
negative at the odd multiples of half that, which sounds hollower. The BBD's clock-dependent band
limit is kept as cheap colour; its companding noise is not modelled.

## Chain position

**After the drives, last of the modulation pedals before Ripple** (Beacon, Gyre, Jetty, Ripple). A
flanger's comb is heard best on a harmonically rich, already-driven signal, and its feedback
peaks would be squared off by a drive after it. Of the three time-modulation pedals it has the
densest, most metallic comb, so it comes last, where it modulates the others rather than being
smeared by them.

## Signal flow

```
in ─┬──────────────────────────────────────────────────────────────┬─► ½ (x + wet) · trim ─► out
    └─► (+) ─► soft limit ─► delay line ─► cubic read (d) ─► BBD band-limit LP ─┬─► wet
         ▲                                                                       │
         └──────── × feedback (−0.95 … +0.95) ◄── 50 Hz high-pass ◄──────────────┘
   d = Delay · 2^(1.5 · depth · tri(LFO))   (0.1 … 16 ms)
```

- **Delay.** A delay line read with cubic (Catmull-Rom) interpolation at a fractional delay that
  moves every sample, so a sweep never zippers. The LFO is a triangle in octaves of delay: the
  notches move at a constant musical rate. Delay glides over 60 ms when you turn it, so the knob
  doesn't bend the pitch hard; the LFO phase is kept in double precision.
- **Comb.** With no feedback, `|H| = |cos(π f d)|`: notches at `(k + ½) / d`, so 250 Hz, 750 Hz,
  1.25 kHz … at the default 2 ms.
- **BBD band limit.** A real BBD's clock is `stages / (2 · delay)`, and its anti-alias filters sit
  near a third of that. Jetty models 512 stages: a 12 dB/oct low-pass (Q 0.6) at clock / 3, capped
  at 16 kHz, that follows the delay, so long delays get darker, as on hardware. The filter's own
  low-frequency group delay, `1 / (2 Q g)` samples, is taken off the read, so the notches stay where
  Delay puts them.
- **Feedback.** The wet signal goes back into the line through a 50 Hz high-pass (the circuit's
  coupling capacitor, so bass does not build up) and a soft limit (knee 1, ceiling 2) that holds
  full regen bounded the way a BBD's headroom does. Capped at ±0.95.
- **Level.** `trim = 1.68 · (1 − c · g²)`, with `c = 0.41` for positive and `0.59` for negative
  feedback (negative feedback rings where a guitar has more energy), keeps the guitar reference
  tone within about 1.5 dB across the Feedback travel.
- **Reset.** A cleared line is empty; the delayed path fades in over 5 ms from the moment the first
  signal reaches its output, so switching the pedal on never arrives as a step.
- No oversampling, no latency (the delayed path is the effect, not a latency).

## Control mapping

| Control | Mapping |
|---|---|
| `jettySpeed` | LFO rate 0.05 → 5 Hz, skewed so noon is 0.5 Hz. |
| `jettyDepth` | Sweep width, 0 → ±1.5 octaves of delay about Delay (±0.9 octaves at the default 0.6). |
| `jettyDelay` | The base (manual) delay, 0.25 → 10 ms, skewed so noon is about 1.6 ms; the swept delay is held to 0.1 to 16 ms. |
| `jettyFeedback` | −100 → +100 %, mapped to −0.95 → +0.95 fed back. |

## Measurements (48 kHz; `engine/tests/pedals/ModsReport.cpp`)

| Feedback | Guitar level (Speed 0.3 Hz, Depth 0.6, Delay 2 ms) |
|---|---|
| −100 % | 0.0 dB |
| −50 % | −0.2 dB |
| 0 | −0.3 dB |
| +50 % | −1.5 dB |
| +100 % | +0.1 dB |

At a 1 ms delay the notches fall at 500 Hz, 1.5 kHz and 2.5 kHz within 1 %. Positive feedback
rings at 1, 2 and 3 kHz, negative at 500 Hz, 1.5 and 2.5 kHz; the teeth stand more than 20 dB
above the troughs at +80 % and more than 15 dB at −100 %, and at +80 % the 1 kHz tooth's Q rises
from the plain comb's 2 to above 6. Cost: 42 ns a sample, 0.20 % of one core (48 kHz mono).

## References

- J. O. Smith, *Physical Audio Signal Processing*, CCRMA, Stanford (online): "Flanging" and the
  feedforward and feedback comb filters.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011: flanger, fractional delay
  and interpolation.
- J. Dattorro, "Effect Design, Part 2: Delay-Line Modulation and Chorus", *J. Audio Eng. Soc.*
  45(10), 1997: interpolated, modulated delay lines.
- C. Raffel, J. Smith, "Practical Modeling of Bucket-Brigade Device Circuits", *Proc. DAFx-10*,
  Graz, 2010: BBD clock, stage count and anti-alias filtering.
- V. Zavalishin, *The Art of VA Filter Design*, 2012/2018: the band-limit filter.
- The level trim, the limit and the 512-stage choice are our own.

## Tests (`engine/tests/pedals/JettyTests.cpp`)

- Finite for random settings and full-scale noise; silence in gives silence out.
- Comb notches sit at the odd multiples of `1 / (2 d)` within 1 % and deeper than 30 dB, for 1, 2
  and 5 ms, at 44.1 kHz (fractional delays), 48 and 96 kHz.
- Feedback polarity picks the teeth: positive at multiples of 1 kHz within 3 %, negative at odd
  multiples of 500 Hz within 4 % (the coupling high-pass pulls the first tooth up about 3 %), with
  the contrast and Q above.
- Modulation depth and rate: the delay reaches `Delay · 2^(±1.5 · depth)` within 0.5 % and repeats
  at the Speed within 1 % (0.05, 0.5 and 5 Hz at 44.1 and 192 kHz); a 1 kHz tone dips once per
  millisecond of delay travel.
- Stable at ±100 % feedback with the shortest and longest Delay at 44.1, 96 and 192 kHz: peak below
  3 (the soft limit holds the loop), rings out to silence.
- Interpolation does not zipper: a fast, deep sweep of a 220 Hz tone leaves nothing above 6 kHz
  (−60 dB); every control's automation is click-free.
- The guitar reference tone stays near unity across Feedback; a static comb adds no harmonics
  (THD < 0.01 %, inharmonic < −75 dB) and the soft limit's aliases stay below −50 dB when driven;
  output identical for block sizes 1 to 4096.

## Known limitations

- No companding (compressor/expander) noise or breathing, and no clock bleed: kept clean on purpose.
- One LFO for both channels: no stereo spread yet.
