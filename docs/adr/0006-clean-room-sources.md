# ADR-0006: Clean-room sources only

Status: Accepted

## Context

Sedef matches a familiar feature set: three amps, two drives, compressor,
modulation, tape echo, reverb, cab mic'ing, and a graphic EQ. That feature set
and its controls are common across the genre. The protected parts of any one
product are its code, data (coefficients, impulse responses, presets), art,
and trademarks.

## Decision

Allowed sources: public papers and textbooks, open-source code under
compatible licences, product manuals and public marketing material for feature
ideas, and our own measurements of hardware we own.

Not allowed: decompiled or disassembled code from any product, data extracted
from another product's binaries (coefficient tables, IRs, presets, art), or
captures of another plugin's output shipped as our sound. Contributions
derived from such material are rejected.

## Consequences

Every DSP page in `docs/dsp/` lists its references. Reviewers check them.
