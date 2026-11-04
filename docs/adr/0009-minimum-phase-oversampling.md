# ADR-0009: Minimum-phase oversampling by default

Status: Accepted

## Context

By 0.4.0 eight stages oversample in series (seven pedals and the amp), each with its own
linear-phase FIR round trip ([ADR-0008](0008-oversampling.md)). Every stage reports its latency
whether it is on or not, so the host's compensation never changes, and the rig reached 298 samples
(6.2 ms) at 48 kHz. Playing live through the plug-in, that is the delay of standing two metres
further from the amp, on top of the interface's buffers.

A linear-phase filter's delay is half its length, fixed by the transition band and the stopband
it must reach. A minimum-phase filter with the same magnitude response delays low frequencies by
far less. The gain–phase relation sets a floor: a half-band with a stopband near 90 dB and the
transition needed to keep 20 kHz flat delays low frequencies by about 4.5 samples per stage at
the base rate, whatever its structure.

## Decision

The oversampler gets a minimum-phase mode, the default ("Low latency"), built from polyphase IIR
all-pass half-bands (Valenzuela and Constantinides 1983; Regalia, Mitra and Vaidyanathan 1988;
harris, "Multirate Signal Processing", ch. 10): elliptic designs from a closed form, 90 dB or more
of stopband, flat to 20 kHz at 44.1 kHz, with the transition tuned so each round trip's group
delay at DC is a whole number of samples (5 at 4×, 3 at 2×). That figure is what the stage
reports; bypass paths delay by it, so the host's compensation still never changes.

The 0.4.0 FIRs stay as the "Linear phase" option. One appended, non-automatable session
parameter (`oversampling`, ADR-0004) picks the mode for every stage; changing it re-prepares the
engine off the audio thread and reports the new latency. The capture stage's rate bridge follows
the mode with a minimum-phase version of its sinc kernel (from the real cepstrum).

Modules that sum a base-rate signal with an oversampled one (Pearl's and Kelp's clean term) pass
the base-rate signal through the same up and down conversion, so the two stay in phase at every
frequency.

## Consequences

The rig's latency at 48 kHz drops from 298 samples (6.2 ms) to 41 (0.85 ms), and from 296 to 40
at 44.1 kHz. Each stage still costs about 5 samples, so eight stages cannot reach "a few samples";
that would take one oversampled domain shared by all stages, a larger change left for later.

The delay is no longer the same at every frequency: per stage at 48 kHz it is 5.00 samples up to
1 kHz, 5.07 at 5 kHz, 5.29 at 10 kHz and 5.76 at 15 kHz. So a bypass crossfade, which sums a
stage's dry and wet paths for 10 ms, dips above 10 kHz mid-fade; the dips are measured and
bounded in the tests ([oversampling.md](../dsp/oversampling.md), §8). Small-signal response and
harmonic levels match the linear-phase mode to 0.05 dB in every stage, and the filters fold less
energy back into the audio band than 0.4.0's FIRs did.

Sessions saved by 0.4.0 have no value for the new parameter and open in Low latency, 5.4 ms
earlier than before. Mixing work that needs every frequency in time can switch to Linear phase.
