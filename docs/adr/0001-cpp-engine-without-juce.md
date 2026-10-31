# ADR-0001: The DSP engine is pure C++20 with no JUCE dependency

Status: Accepted

## Context

Real-time audio needs deterministic memory and timing. Garbage-collected
languages are out. The serious open-source building blocks for guitar DSP
(chowdsp, RTNeural, Neural Amp Modeler core) are C++. Rust was considered: it
would buy memory safety at the cost of rewriting or wrapping those libraries
and of thinner plugin-format support (no AU or AAX without C++ shims).

## Decision

Write the engine in C++20 as a standalone static library (`sedef_engine`)
that does not include JUCE. The plugin shell adapts JUCE types to engine types
at one boundary (`PluginProcessor::processBlock`).

## Consequences

- Engine tests build in seconds and run in CI on every OS without JUCE.
- The engine can be reused in other hosts (CLI renderer, a future mobile app,
  a different plugin framework).
- We maintain a small set of primitives JUCE would otherwise provide
  (biquads, delay lines, oversampling). They are tested and documented.
