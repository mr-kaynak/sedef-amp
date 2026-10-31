# ADR-0004: One parameter schema for engine, host, presets and UI

Status: Accepted

## Context

Amp sims expose 150+ parameters. Keeping engine indices, host parameter
definitions, preset keys and UI controls in sync by hand is the most common
source of bugs in this kind of product.

## Decision

Declare every parameter once in `ParameterList.inc` (X-macro). Generate the
engine enum, the APVTS layout and the UI metadata from it. Persist presets by
string id.

## Consequences

- Adding a parameter is a one-line change plus its DSP use.
- IDs become a compatibility contract; they are never renamed.
