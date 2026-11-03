# Third-party software

| Component | Use | Licence |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 9 | Plugin framework, WebView | AGPLv3 (open-source option) |
| [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) | CLAP format | MIT |
| [PFFFT](https://github.com/marton78/pffft) | FFT for convolution and pitch analysis | BSD-like (FFTPACK) |
| [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) 0.6.0 | Plays .nam captures (unmodified) | MIT |
| [Eigen](https://gitlab.com/libeigen/eigen) 5.0.1 | Linear algebra for NeuralAmpModelerCore | MPL-2.0, a few files BSD or Apache-2.0 (see its `COPYING.*`); used unmodified, sources at the link |
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | JSON parsing for .nam files | MIT |
| [Catch2](https://github.com/catchorg/Catch2) | Unit tests (not shipped) | BSL-1.0 |
| [libcurl](https://curl.se/libcurl/) | HTTPS for the update check on Linux: the system's copy, loaded at run time (not shipped) | curl (MIT-style) |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | Dependency fetching (build only) | MIT |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) 1.4.0 and [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) 0.6.4 | Reference candidate in the transpose bench, only with `-DSEDEF_TRANSPOSE_BENCH_STRETCH=ON` (not shipped) | MIT |

UI dependencies and their licences are listed in `ui/package.json`; run `pnpm licenses list` in
`ui/` for the full report.

## Notices

- NeuralAmpModelerCore: Copyright (c) 2023 Steven Atkinson. MIT licence.
- nlohmann/json: Copyright (c) 2013-2025 Niels Lohmann. MIT licence.
- Eigen: Mozilla Public License 2.0; the source of the version used is at
  <https://gitlab.com/libeigen/eigen/-/tree/5.0.1>.

The MIT licence text: Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"), to deal in the
Software without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
to whom the Software is furnished to do so, subject to the following conditions: The above
copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.
