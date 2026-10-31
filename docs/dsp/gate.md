# Gate

`engine/include/sedef/dsp/NoiseGate.h`, `engine/src/dsp/NoiseGate.cpp`

## What it models

A guitar noise gate that should be heard only when it is doing its job: hiss and hum vanish
between phrases, notes keep their natural decay, and nothing chatters when a note hovers around
the threshold. It is a downward expander with hysteresis and hold. The detector listens to the raw
DI (the sidechain), so the gate follows the guitar's own level whatever the input gain is.

## Signal flow

```
sidechain ─► DC block (10 Hz) ─► |x| ─► peak hold 15 ms ─► release (τ 5 ms) ─► envelope
                                                                                  │
          ┌── open when envelope ≥ T ─────────────────────────────────────────────┤
 state ───┤   stays open in the hysteresis band T-4 dB … T (hold refreshed)        │
          └── closes 25 ms after the envelope drops below T-4 dB                  │
                                                                                  ▼
 target gain: open → 0 dB;  closed → 1:7 expansion below T (−24 dB at T-4 dB), floor −80 dB
                                                                                  │
 gain smoother: attack τ 0.4 ms / release τ 25 ms (linear gain), then a 0.15 ms rounding pole
                                                                                  ▼
io ───────────────────────────────────────────────────────────────────────────► × ─► out
```

- **Hysteresis** (4 dB) and **hold** stop the gate from flapping on a decaying or wobbling note.
  The detector's 15 ms peak hold (longer than one period of a low B) keeps the envelope free of
  per-cycle ripple, and adds to the 25 ms gate hold: about 40 ms in total for a note fading through
  the threshold.
- **Closed is not a switch**: below the open threshold the gain follows the envelope through a
  steep 1:7 expander, so the transition into silence tracks the note instead of cutting it.
- **Release** is an exponential fade in the linear gain domain, i.e. a constant ~0.35 dB/ms:
  measured −40 dB about 115 ms after the gate closes. **Attack** is a 0.4 ms pole followed by a
  0.15 ms rounding pole, so the gain has no slope discontinuity when it opens (fast, but no tick).
- Once fully open the gain lands exactly on 1.0, so an open gate is bit-transparent.

## Control mapping

| Control | Mapping |
|---|---|
| `thresholdDb` | −96…0 dB, compared with the sidechain's peak envelope (dBFS peak). −96 dB (the bottom of the range) disables the gate: always open, 0 dB. |

Telemetry: `gainReductionDb()` is the gain at the end of the last block (0 … −80 dB);
`isOpen()` is the hysteresis state (true when disabled). Both are relaxed atomics.

## References

- J. D. Reiss, A. P. McPherson, *Audio Effects: Theory, Implementation and Application*, CRC
  Press, 2014 — noise gates, hysteresis and hold.
- U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley, 2011 — expanders and gates.
- Hysteresis/hold constants and the expander-below-threshold gain computer are our own design.

## Tests (`engine/tests/pedals/NoiseGateTests.cpp`)

- Finite output for random thresholds and full-scale noise at 44.1/48/96 kHz, block sizes
  1/17/64/512, mono and stereo; silence in gives silence out.
- A burst opens the gate: gain > −2 dB 1 ms after onset, > −0.1 dB after 3 ms.
- After the signal stops: fully open for at least 35 ms, closes 40–80 ms later, then fades
  (still > −26 dB at +90 ms, < −40 dB at +250 ms, at the −80 dB floor by +500 ms).
- A tone wobbling ±1.5 dB around the threshold opens the gate once and never closes it.
- The sidechain, not the gated signal, drives it; −96 dB is a bit-exact bypass.
- Sweeping the threshold through a steady tone never snaps the gain.

## Known limitations

- One detector for all channels (the rig gates a mono DI).
- Hold and release are fixed; no user controls for them.
- The detector is peak-based; very low-frequency content below ~60 Hz can show slight envelope
  ripple in the expansion region (hidden by the release smoothing).
