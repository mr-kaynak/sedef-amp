# Contributing to Sedef Amp

Thanks for helping! This guide gets you from clone to pull request.

## Ground rules

- **Clean room.** Read [ADR-0006](docs/adr/0006-clean-room-sources.md). We accept code and data
  based on public literature, compatible open-source code, and measurements of hardware you own.
  We reject anything derived from decompiled or extracted commercial products, and captures of
  other plugins shipped as Sedef's sound.
- **Real-time safety.** Read [ADR-0005](docs/adr/0005-real-time-safety.md). No allocation, locks,
  I/O, logging or exceptions on the audio thread.
- **One schema.** Parameters live in `engine/include/sedef/params/ParameterList.inc`. Ids are
  forever: append, never rename or reorder.

## Build

Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler (Xcode CLT 15+, MSVC 2022, GCC 13+/Clang 17+),
Node 22+ with pnpm for the UI.

```sh
cmake --preset engine && cmake --build --preset engine && ctest --preset engine   # DSP only, fast
cmake --preset dev && cmake --build --preset dev                                   # everything
```

The test presets run four tests at a time (`ctest --preset engine -j 1` runs them one by one).
So a test that writes a file writes it under a name of its own (a temporary folder named with a
`juce::Uuid`, or a folder in the build tree named for the test); one that must use a fixed path
(the system's install folders, say) gets a `RESOURCE_LOCK` in its `catch_discover_tests`.

UI development against the browser mock:

```sh
cd ui && pnpm install && pnpm dev
```

UI development inside the real plugin: run `pnpm dev`, then launch the standalone app with
`SEDEF_UI_DEV_URL=http://localhost:5173/`.

## Pull requests

- One topic per PR, with tests. DSP changes need tests that assert behaviour (frequency response,
  levels, timing), not just "it runs".
- New DSP modules get a page in `docs/dsp/` listing their references.
- `clang-format` (config in the repo) and `pnpm lint` must pass. CI runs both. CI pins
  clang-format 23.1.3; to get exactly the same output locally, run
  `uvx --from clang-format==23.1.3 clang-format -i <files>` (or `pip install clang-format==23.1.3`).
- Explain *why* in the PR description; link issues.

## Review checklist (real-time code)

- [ ] No `new`/`delete`/`std::vector::push_back`/`std::string` in `process()` paths
- [ ] Parameter changes are smoothed; automation sweeps don't click
- [ ] Feedback paths can't blow up or produce denormals
- [ ] Works at 44.1–192 kHz and for block sizes 1…4096
