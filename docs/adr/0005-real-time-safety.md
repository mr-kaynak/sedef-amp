# ADR-0005: Real-time safety rules

Status: Accepted

## Decision

On the audio thread we never allocate or free memory, take locks, wait on
condition variables, perform I/O or logging, or throw. Cross-thread data moves
through atomics, a single-producer/single-consumer ring, or an atomic pointer
handoff whose garbage is reclaimed off the audio thread.

## Enforcement

- `Engine` tests run `process` under a counting global allocator and fail on
  any allocation after `prepare`.
- Code review checklist in `CONTRIBUTING.md`.
