# Trench (octave down)

`engine/include/sedef/dsp/Trench.h`, `engine/src/dsp/Trench.cpp`

## What it models

The analog monophonic octave divider: the input is low-passed until little more than its
fundamental is left, a comparator with hysteresis (a Schmitt trigger) turns that into one edge per
period, and two flip-flops divide the edges by two and by four. Their square waves, one and two
octaves below the note, are shaped by the input's envelope and blended with the dry signal. It
plays one note at a time, as the circuit does. It does not shift pitch: the removed Transpose
shifter (see [transpose.md](transpose.md)) is not used.

Where the circuit sputters (chords, the noise of an attack, a dying note), Trench backs off: the
subs speak only while the edges agree with each other, and fade out otherwise.

## Signal flow

```
in ─┬──────────────────────────────────────────────── × direct ──────────────────────┐
    │                                                                                 │
    └─► HP 45 Hz ─► LP 1.1 kHz ─┬─► mean |x| (10 ms) ── level ── gate (hysteresis) ─┐ │
                                │                                                    │ │
                                └─► tracking LP (2 × Butterworth, corner 1.5 × note) │ │
                                    ─► Schmitt trigger (±0.3 × recent peak)          │ │
                                    ─► edge time (sub-sample) ─► period check        │ │
                                    ─► flip-flop ÷2 ─► square −1 ─┐                  │ │
                                       flip-flop ÷2 ─► square −2 ─┤ polyBLEP edges   │ │
                                                                  ▼                  ▼ │
                   × (sub1, sub2) × envelope × gate·confidence ─► tone LP ─► HP 10 Hz ─► +─► out
```

- **Detector**: a high-pass (45 Hz) and a fixed 1.1 kHz low-pass, then the tracking low-pass: two
  Butterworth sections whose corner sits 1.5 × above the note, where the fundamental passes and
  the second harmonic is 13 dB down on top of what the string gave it. The corner follows the
  median of the last three agreeing periods and moves only for a change of note (a 12 % deadband):
  a corner that chased every period would shift the filter's delay and with it the next edge, and
  the tracker would start to wobble around the note (it did: a three-period jitter of ±8 % on E2
  before the deadband). Without a note the corner rests at 200 Hz, where every guitar
  fundamental's harmonics are attenuated more than the fundamental.
- **Schmitt trigger**: thresholds at ±30 % of the filtered signal's recent peak. The peak decays
  over 1.5 periods of the note (3–30 ms), so the thresholds follow a fading string without
  dropping below the harmonics' wiggles. An edge's time is interpolated between samples.
- **Period check**: edges closer than 1/1400 s are noise and ignored; no edge for 1/40 s means
  the note is lost. Consecutive periods within 8 % agree. Confidence rises halfway to 1 on every
  agreeing period and halves on every other one; a lost note sets it to 0.
- **Flip-flops**: the first toggles on every accepted edge (÷2, one octave down), the second on
  every falling edge of the first (÷4, two down). Each toggle is band-limited with a polyBLEP
  residual split between the sample before the edge (held back one sample) and the one after, so
  the squares need no oversampling. The sub path is one sample behind the direct path; nothing is
  reported to the host.
- **Envelope and gate**: the subs are `square × level × π/2`, `level` being the band-limited
  input's mean |x| over 10 ms (π/2 makes it the amplitude of a sine), times a gate. The gate opens
  above −52 dBFS mean |x| and closes below −58 dBFS (hysteresis), and its target is the
  confidence; it rises in 4 ms and falls in 15 ms. So the sub is silent in silence and at low
  level, fades on chords and after a note stops, and never holds a stuck note.
- **Tone**: a second-order low-pass (Q 0.6) on the subs, 150 Hz → 670 Hz at noon → 3 kHz, then a
  10 Hz DC block (a square whose envelope moves carries a little DC).

## Control mapping

| Control | Mapping |
|---|---|
| `direct` | Dry gain 2 × direct (linear): unity at noon, +6 dB at 10. At noon with both subs at 0 the output is a bit-exact copy of the input. |
| `sub1`, `sub2` | Sub gains 2 × 0.95 × knob: at noon a sustained note's sub is within 2 dB of the note's level (see below). |
| `tone` | Sub low-pass corner 150 Hz × 20^tone. |

All gains ramp per sample over 30 ms; the tone corner follows a 20 ms smoother, updated every 32
samples. `latencySamples()` is 0.

## Measurements (48 kHz, plucks at 0.18 of the string)

| note | sub −1 | sub −2 | lock | onset / settled peak | sub −1 at noon vs note |
|---|---|---|---|---|---|
| E2 (82.4 Hz) | +0.07 cents | +0.06 cents | 49.5 ms | 0.98 | +0.7 dB |
| E3 (164.8 Hz) | +0.11 cents | +0.11 cents | 27.5 ms | 1.01 | +1.4 dB |
| E4 (329.6 Hz) | +0.06 cents | +0.06 cents | 19.5 ms | 1.08 | +0.1 dB |
| E5 (659.3 Hz) | −0.01 cents | −0.01 cents | 25.5 ms | 1.00 | −1.9 dB |

- Pitch: mean frequency of the sub over 0.4–1.5 s against f/2 and f/4.
- Lock: from the pick to the sub's 5 ms RMS reaching half its level 300–500 ms later. About four
  periods of the note plus the gate's attack; a real divider needs at least one period too.
- Onset / settled peak: the sub's peak in the first 500 ms over its peak in the next 500 ms, on a
  note that does not decay. The few per cent above 1 are where the band-limited edges fall between
  samples, not an overshoot.
- An open E major strum (input peak 0.23): the subs peak at 0.185, against 0.402 for a single
  steady E3. The periods of a chord do not agree, so the gate holds the subs down.
- A muted note: the subs never rise after the mute and are 60 dB down within 150 ms.
- Legato steps (a fifth up, an octave down, an octave up): the sub re-locks within 30 ms and lands
  within 1 cent.
- CPU: about 0.2× Pearl.

The failure to know about: a low note whose fundamental is much weaker than its second harmonic
(a bridge pickup, picked right at the bridge) can lock an octave high, so the "sub" plays at the
note's own pitch. It stays steady (no sputter), and the neck pickup or a rolled-off tone cures it,
as with the circuit.

## References

- P. Dutilleux, K. Dempwolf, M. Holters, U. Zölzer, "Nonlinear Processing", ch. 4 in U. Zölzer
  (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011 — harmonic and subharmonic
  generation, octave division with flip-flops.
- P. Horowitz, W. Hill, *The Art of Electronics*, 3rd ed., Cambridge University Press, 2015 —
  comparators with hysteresis (Schmitt triggers), flip-flop frequency dividers.
- V. Välimäki, A. Huovilainen, "Antialiasing Oscillators in Subtractive Synthesis", *IEEE Signal
  Processing Magazine* 24(2), 2007; V. Välimäki, J. Pekonen, J. Nam, "Perceptually informed
  synthesis of bandlimited classical waveforms using integrated polynomial interpolation",
  *J. Acoust. Soc. Am.* 131(1), 2012 — polyBLEP.
- The tracking-filter rule, the agreement logic, the gate and all constants are our own.

## Tests (`engine/tests/pedals/TrenchTests.cpp`)

- Finite output for random settings and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- Direct at noon with the subs down is a bit-exact copy; no latency.
- Both subs within 1 cent of f/2 and f/4 for E2, E3, E4 and E5, two pluck positions, at 44.1,
  48 and 96 kHz.
- Lock within 60 ms on E2 and 35 ms from E3 up; sub at noon within 3 dB of the note's level.
- No burst: onset peak ≤ 1.1 × settled peak on steady notes; a strum's sub never exceeds a
  steady single note's; a muted note's sub never rises and is 60 dB down within 150 ms.
- Legato steps re-lock within 30 ms, in tune within 1 cent, with no peak above the sustained
  level.
- Silent subs (exactly zero) for a note 50 dB down and for −60 dBFS hiss; a light note 20 dB
  down still gets its sub, in tune.
- Automating each knob, and all at once, is click-free.

## Known limitations

- Monophonic by design: on chords it fades the subs rather than following one of the notes.
- The octave-high lock on notes with a very weak fundamental (above).
- Lock takes about four periods: fast runs on the low strings (sixteenths at 150 bpm on E2) get
  their subs late or not at all.
