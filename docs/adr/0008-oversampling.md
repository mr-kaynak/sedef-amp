# ADR-0008: Oversampling strategy

Status: Accepted

## Context

Waveshapers create harmonics far above Nyquist. At 44.1/48 kHz, high-gain
stages alias audibly.

## Decision

Nonlinear modules (Pearl, Coral, amps) run inside `core::Oversampler`:
cascaded linear-phase half-band FIR stages (2× per stage), Kaiser-windowed,
≥ 90 dB stopband. Default factor 4× at base rates ≤ 48 kHz, 2× at ≤ 96 kHz,
1× above. Latency is reported to the host.

## Consequences

37 samples (0.77 ms) of added latency at 48 kHz with 4×, 31 samples at 2×. Linear phase keeps the
amp's transient shape and avoids pre-ringing artefacts being phase-smeared
across the chain. A minimum-phase option can be added later for low-latency
live use.
