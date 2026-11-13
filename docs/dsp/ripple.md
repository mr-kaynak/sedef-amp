# Ripple (modulation)

`engine/include/sedef/dsp/Ripple.h`, `engine/src/dsp/Ripple.cpp`

## What it models

A three-mode modulation pedal: a smooth, throbbing optical-style **tremolo**, a lush
bucket-brigade-style **chorus**, and a **vibrato** whose depth sets the pitch swing directly.

## Signal flow

```
                       ┌─ tremolo: × (1 − m · dip(φ)) ──────────────────────────────┐
in ─► delay line ──────┼─ chorus:  0.5 · dry + 0.5 · LP 6 kHz(read 9 ms ± swing) ───┼─ Σ w_mode ─► out
     (always written)  └─ vibrato: read (0.3 ms + swing · (1 + sin 2πφ)) ───────────┘
LFO phase φ, shared; the second channel's chorus LFO is 90° ahead.
```

- **Tremolo**: lamp-and-photocell attenuators darken quickly and recover slowly, so the dip takes
  40 % of the cycle and the recovery 60 % (a phase warp of a raised cosine, C1-continuous). The dip
  is raised to the power 1.5, so more time is spent near full volume and the trough is rounder. The
  shape is computed from the LFO phase (not by filtering), so full depth reaches true silence at
  every speed: 100 % amplitude modulation. Peaks are exactly unity.
- **Chorus**: one modulated voice, 9 ms centre, Hermite-interpolated read, mixed 50/50 with the dry
  signal (unity at low frequencies). The wet path goes through a 6 kHz second-order low-pass, like
  a bucket-brigade line's anti-imaging filters, and gets a slow (0.4 Hz) random wander of 15 % of
  the swing for an analog, less mechanical movement. With two channels the second LFO is offset by
  a quarter cycle.
- **Vibrato**: 100 % wet. The delay swing is computed from the wanted pitch deviation: a
  sine-modulated delay of peak swing A at f Hz bends pitch by up to 2πfA, so
  A = (2^(cents/1200) − 1) / (2πf). The minimum delay is 0.3 ms, keeping the added delay as small
  as possible (0.3 ms + A on average).
- **Mode switches** crossfade over 25 ms (one weight per mode, so any sequence of switches is
  click-free). The delay line is always written, so the incoming mode has history.

## Control mapping

| Control | Mapping |
|---|---|
| `mode` | 0 tremolo, 1 chorus, 2 vibrato (25 ms crossfade). |
| `speedHz` | LFO rate, 0.69…7.19 Hz (glides with a 50 ms time constant). |
| `depth` (tremolo) | Modulation index m = 1 − (1 − depth)²: −12 dB dips at noon, silence at full. |
| `depth` (chorus) | Peak swing = depth × min(3 ms, swing that bends 30 cents at the current speed): up to ±3 ms at slow speeds; at fast speeds limited so it shimmers instead of going seasick. |
| `depth` (vibrato) | Peak pitch deviation = 45 cents × depth, independent of speed. |

Depth and swing are smoothed per sample (30–60 ms), so turning the knobs never zippers.

## References

- J. Dattorro, "Effect Design, Part 2: Delay-Line Modulation and Chorus", *J. Audio Eng. Soc.*
  45(10), 1997 — modulated delay lines, interpolation, chorus and vibrato.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011 — tremolo, vibrato, chorus.
- J. D. Reiss, A. P. McPherson, *Audio Effects: Theory, Implementation and Application*, CRC
  Press, 2014 — modulation effects.
- The tremolo shape, the speed-dependent swing limits and the drift are our own design.

## Tests (`engine/tests/pedals/RippleTests.cpp`)

- Finite output for random modes/settings and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- Tremolo: the period of the gain curve matches the speed within 1 % at 0.69/3.92/7.19 Hz; peaks
  at unity, troughs below −60 dB at full depth; depth 0 is bit-transparent.
- Pitch modulation (zero-crossing period spread of a 1 kHz tone): none for tremolo; present for
  chorus; 4–7 % for full vibrato (±45 cents), and the same at 0.69 and 7.19 Hz.
- Chorus keeps the dry signal: an impulse comes out at 0.5 immediately, the wet copy after ~6 ms.
- Stereo chorus channels differ.
- Speed and depth sweeps in every mode and mode switches are click-free (the switch test fails
  without the crossfade).

## Known limitations

- Single-voice chorus; no stereo tremolo/vibrato (both channels share the LFO phase), on purpose
  after the amp too: an offset vibrato's mono sum would be a comb ([board.md](../board.md#63-the-mono-sum-of-a-flanger-or-chorus)).
- Vibrato's average delay follows the swing to keep latency minimal, so turning depth or speed
  causes a brief, gentle pitch glide while the knob moves.
- No tempo sync and no waveform choice.
