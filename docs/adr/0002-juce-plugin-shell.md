# ADR-0002: JUCE 9 for the plugin shell; VST3, AU, CLAP and Standalone

Status: Accepted

## Context

We need VST3, AU and a standalone app from one codebase on macOS, Windows
and Linux. CLAP is the modern open standard and well supported by
open-source hosts.

## Decision

Use JUCE 9 with `clap-juce-extensions` for CLAP. Do not ship AAX.

## Consequences

- JUCE 9 is AGPLv3 for open-source use, which sets the project licence
  (ADR-0007).
- AAX would need the Avid SDK and PACE signing under an NDA, which an AGPL
  project cannot redistribute. Pro Tools users are out of scope for v1.
