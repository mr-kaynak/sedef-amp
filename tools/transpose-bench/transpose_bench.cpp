// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Transpose evaluation bench: runs pitch-shifter candidates through the metrics of
// docs/dsp/transpose.md ("Quality bars") against an ideal synthesised reference.
//
//   sedef-transpose-bench [--candidates wsola,engine,...] [--shifts -1,-2,-5,-12,12]
//                         [--rate 48000] [--block 64] [--no-cpu] [--wav DIR] [--listen DIR]
//                         [--di FILE,FILE,...]
//
// Candidates:
//   wsola[:jump=ms][:fade=ms][:search=ms]  the 0.2.0 shifter, optionally retuned
//   engine                                 this build's dsp::PitchShifter
//   pv[:a=ms][:s=ms][:o=n][:sym=1][:nb=n][:tr=0][:clip=0][:floor=dB][:oc=x][:or=x]
//                                          the phase-vocoder candidate (SpectralShifter.h)
//   stretch-<blockMs>-<intervalMs>         Signalsmith Stretch (MIT), when built with
//                                          -DSEDEF_TRANSPOSE_BENCH_STRETCH=ON
// --wav writes each candidate's output for the test signals; --listen writes musical DI
// (strum, riff, line) as input, ideal and candidates, aligned to the input, and exits.
// --di measures recorded DI files instead (no ideal reference exists for them): the pitch of the
// output against the input's times the ratio, and where each attack lands against the input's,
// both after removing the reported latency.

#include "../WavIo.h"
#include "LegacyWsola.h"
#include "SpectralShifter.h"
#include "reverb/TransposeEval.h"
#include "sedef/dsp/PitchShifter.h"

#if SEDEF_HAVE_STRETCH
#include "signalsmith-stretch.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace sedef;
namespace tp = sedef::test::transpose;

namespace
{

std::vector<std::string> split (const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::stringstream ss (s);
    std::string item;
    while (std::getline (ss, item, sep))
        if (!item.empty())
            out.push_back (item);
    return out;
}

template <typename T>
tp::Shifter wrapSedef (std::shared_ptr<T> p)
{
    tp::Shifter s;
    s.prepare = [p] (double fs, int block) {
        p->prepare ({ fs, block, 1 });
    };
    s.setSemitones = [p] (int st) {
        p->setSettings ({ st });
    };
    s.process = [p] (float* x, int n) {
        p->process (AudioView (x, n));
    };
    s.latency = [p] {
        if constexpr (requires { p->latencySamples(); })
            return p->latencySamples();
        else
            return 0;
    };
    return s;
}

#if SEDEF_HAVE_STRETCH
/// Signalsmith Stretch as a fixed-latency pitch shifter: input and output run at the same rate,
/// the latency is inputLatency + outputLatency.
struct Stretch
{
    signalsmith::stretch::SignalsmithStretch<float> st;
    double blockMs = 120.0, intervalMs = 30.0;
    int semitones = 0;
    std::vector<float> in, out;

    void prepare (double fs, int block)
    {
        st.configure (1, static_cast<int> (blockMs * 0.001 * fs),
                      static_cast<int> (intervalMs * 0.001 * fs));
        st.reset();
        st.setTransposeSemitones (static_cast<float> (semitones));
        in.assign (static_cast<size_t> (block), 0.0f);
        out.assign (static_cast<size_t> (block), 0.0f);
    }
    void process (float* x, int n)
    {
        std::copy_n (x, n, in.data());
        float* ip[1] = { in.data() };
        float* op[1] = { out.data() };
        st.process (ip, n, op, n);
        std::copy_n (out.data(), n, x);
    }
};
#endif

/// pv[:key=value...]: the spectral shifter core with a config, e.g. pv:a=85:s=8:o=4.
tp::Shifter makeSpectral (const std::string& name)
{
    bench::SpectralShifter::Config cfg;
    for (const auto& kv : split (name, ':'))
    {
        const auto eq = kv.find ('=');
        if (eq == std::string::npos)
            continue;
        const auto key = kv.substr (0, eq);
        const double v = std::stod (kv.substr (eq + 1));
        if (key == "a")
            cfg.analysisMs = v;
        else if (key == "s")
            cfg.synthesisMs = v;
        else if (key == "o")
            cfg.overlap = static_cast<int> (v);
        else if (key == "sym")
            cfg.asymmetric = v == 0.0;
        else if (key == "tr")
            cfg.transientReset = v != 0.0;
        else if (key == "clip")
            cfg.clipRegions = v != 0.0;
        else if (key == "floor")
            cfg.peakFloorDb = v;
        else if (key == "nb")
            cfg.peakNeighbours = static_cast<int> (v);
        else if (key == "oc")
            cfg.onsetCentre = v;
        else if (key == "or")
            cfg.onsetRise = v;
        else
        {
            std::fprintf (stderr, "unknown pv key %s\n", key.c_str());
            std::exit (2);
        }
    }
    struct State
    {
        bench::SpectralShifter s;
        bench::SpectralShifter::Config cfg;
        int semitones = 0;
    };
    auto p = std::make_shared<State>();
    p->cfg = cfg;
    tp::Shifter s;
    s.prepare = [p] (double fs, int) {
        p->s.prepare (fs, p->cfg);
        p->s.setRatio (std::exp2 (p->semitones / 12.0), false);
    };
    s.setSemitones = [p] (int st) {
        p->semitones = st;
        p->s.setRatio (std::exp2 (st / 12.0), false);
    };
    s.process = [p] (float* x, int n) {
        p->s.process (x, n);
    };
    s.latency = [p] {
        return p->s.latencySamples();
    };
    return s;
}

tp::Shifter makeCandidate (const std::string& name)
{
    if (name.rfind ("pv", 0) == 0)
        return makeSpectral (name);
    if (name.rfind ("wsola", 0) == 0)
    {
        // wsola[:jump=ms][:fade=ms][:search=ms]: the 0.2.0 shifter, optionally retuned.
        auto p = std::make_shared<bench::LegacyWsola>();
        bench::LegacyWsola::Tuning t;
        for (const auto& kv : split (name, ':'))
        {
            const auto eq = kv.find ('=');
            if (eq == std::string::npos)
                continue;
            const auto key = kv.substr (0, eq);
            const double v = std::stod (kv.substr (eq + 1));
            if (key == "jump")
                t.jumpMinMs = v;
            else if (key == "fade")
                t.spliceFadeMs = v;
            else if (key == "search")
                t.searchMs = v;
        }
        p->setTuning (t);
        return wrapSedef (p);
    }
    if (name == "engine")
        return wrapSedef (std::make_shared<dsp::PitchShifter>());
#if SEDEF_HAVE_STRETCH
    if (name.rfind ("stretch", 0) == 0)
    {
        auto p = std::make_shared<Stretch>();
        const auto parts = split (name, '-');
        if (parts.size() >= 3)
        {
            p->blockMs = std::stod (parts[1]);
            p->intervalMs = std::stod (parts[2]);
        }
        tp::Shifter s;
        s.prepare = [p] (double fs, int block) {
            p->prepare (fs, block);
        };
        s.setSemitones = [p] (int st) {
            p->semitones = st;
            p->st.setTransposeSemitones (static_cast<float> (st));
        };
        s.process = [p] (float* x, int n) {
            p->process (x, n);
        };
        s.latency = [p] {
            return p->st.inputLatency() + p->st.outputLatency();
        };
        return s;
    }
#endif
    std::fprintf (stderr, "unknown candidate %s\n", name.c_str());
    std::exit (2);
}

void writeWav (const std::string& path, const std::vector<float>& x, double rate)
{
    std::ofstream f (path, std::ios::binary);
    const auto u32 = [&f] (std::uint32_t v) {
        f.write (reinterpret_cast<const char*> (&v), 4);
    };
    const auto u16 = [&f] (std::uint16_t v) {
        f.write (reinterpret_cast<const char*> (&v), 2);
    };
    const auto frames = static_cast<std::uint32_t> (x.size());
    f.write ("RIFF", 4);
    u32 (36 + frames * 4);
    f.write ("WAVEfmt ", 8);
    u32 (16);
    u16 (3);
    u16 (1);
    u32 (static_cast<std::uint32_t> (rate));
    u32 (static_cast<std::uint32_t> (rate) * 4);
    u16 (4);
    u16 (32);
    f.write ("data", 4);
    u32 (frames * 4);
    f.write (reinterpret_cast<const char*> (x.data()), static_cast<std::streamsize> (frames * 4));
}

// Listening material: additive, so the ideal shifted version can be rendered too.
tp::Score strum()
{
    tp::Score s;
    s.name = "strum";
    const double chords[4][6] = { { 82.41, 123.47, 164.81, 207.65, 246.94, 329.63 }, // E
                                  { 0.0, 110.0, 164.81, 220.0, 277.18, 329.63 },     // A
                                  { 0.0, 0.0, 146.83, 220.0, 293.66, 369.99 },       // D
                                  { 98.0, 123.47, 146.83, 196.0, 246.94, 392.0 } };  // G
    double t = 0.1;
    for (int c = 0; c < 4; ++c)
    {
        for (int i = 0; i < 6; ++i)
        {
            if (chords[c][i] <= 0.0)
                continue;
            tp::Note n;
            n.start = t + 0.012 * i;
            n.hz = chords[c][i];
            n.level = 0.09;
            n.decay = 2.2 - 0.2 * i;
            n.stopAt = c < 3 ? t + 1.2 : 1.0e9;
            n.inharmonicity = i < 3 ? 1.5e-4 : 0.6e-4;
            n.seed = static_cast<std::uint32_t> (c * 10 + i + 1);
            s.notes.push_back (n);
        }
        t += 1.2;
    }
    s.seconds = t + 1.0;
    return s;
}

tp::Score riffMusic()
{
    tp::Score s;
    s.name = "riff";
    // 0 = muted E5 chug, 1 = accented chug, 2 = open E5 ringing for an eighth, -1 = rest
    const int pattern[32] = { 1, 0, 0, 0, 1, 0, 0, 0, 2, -1, 0, 0, 1, 0, 2, -1,
                              1, 0, 0, 0, 1, 0, 0, 0, 2, -1, 0, 0, 1, 0, 2, -1 };
    for (int k = 0; k < 32; ++k)
    {
        const double t = 0.1 + 0.1 * k;
        if (pattern[k] < 0)
            continue;
        if (pattern[k] == 2)
        {
            const double hz[3] = { 82.41, 123.47, 164.81 };
            for (int i = 0; i < 3; ++i)
            {
                tp::Note n;
                n.start = t;
                n.hz = hz[i];
                n.level = 0.3;
                n.decay = 0.8;
                n.stopAt = t + 0.2;
                n.stopTau = 0.006;
                n.seed = static_cast<std::uint32_t> (500 + k * 3 + i);
                s.notes.push_back (n);
            }
        }
        else
            tp::scores::addMutedHit (s, t, pattern[k] == 1 ? 0.32 : 0.2, t + 0.1,
                                     static_cast<std::uint32_t> (k));
    }
    s.seconds = 3.6;
    return s;
}

void printHeader()
{
    std::printf ("%-22s %4s | %6s %6s %6s | %6s %6s %5s | %5s %5s | %6s %5s %5s %5s | %5s %5s | "
                 "%6s %5s | missed\n",
                 "candidate", "st", "lat", "range", "rep", "purity", "floor", "timbr", "pWarb",
                 "eWarb", "preEch", "flam", "attk", "rise", "pMed", "p95", "alias", "cpu%");
}

void printRow (const std::string& name, const tp::Evaluation& e)
{
    double lo = 1.0e9, hi = -1.0e9, sum = 0.0;
    int n = 0;
    for (const auto* t : { &e.hits, &e.riff })
        for (double l : t->latencyMs)
        {
            lo = std::min (lo, l);
            hi = std::max (hi, l);
            sum += l;
            ++n;
        }
    const auto missed = tp::missedBars (e);
    std::string m;
    for (const auto& s : missed)
        m += (m.empty() ? "" : ", ") + s.substr (0, s.find (' '));
    std::printf ("%-22s %+4d | %6.2f %6.2f %6.2f | %6.1f %6.1f %5.2f | %5.2f %5.2f | %6.1f %5.2f "
                 "%5.2f %5.2f | %5.1f %5.1f | %6.1f %5.2f | %s\n",
                 name.c_str(), e.semitones, n > 0 ? sum / n : 0.0, hi - lo, e.reportedMs,
                 e.chord.purityDb, e.chord.purityRefDb, e.chord.timbreDb, e.warble.partialDb,
                 e.warble.envelopeDb, e.hits.preEchoDb, std::max (e.hits.flamDb, e.riff.flamDb),
                 std::max (e.hits.attackDb, e.riff.attackDb),
                 std::max (e.hits.riseMs, e.riff.riseMs), e.pitch.medianCents, e.pitch.p95Cents,
                 e.aliasDb > -199.0 ? e.aliasDb : 0.0, e.cpuPercent,
                 m.empty() ? "none" : m.c_str());
    std::fflush (stdout);
}

/// Magnitude spectra of x on a log-frequency grid (`step` cents from 60 Hz to 5 kHz), one per hop.
std::vector<std::vector<float>> logSpectra (const std::vector<float>& x, double fs, int nfft,
                                            int hop, double step)
{
    PFFFT_Setup* setup = pffft_new_setup (nfft, PFFFT_REAL);
    const auto n = static_cast<size_t> (nfft);
    auto* in = static_cast<float*> (pffft_aligned_malloc (n * sizeof (float)));
    auto* out = static_cast<float*> (pffft_aligned_malloc (n * sizeof (float)));
    auto* work = static_cast<float*> (pffft_aligned_malloc (n * sizeof (float)));
    const auto cells = static_cast<size_t> (1200.0 * std::log2 (5000.0 / 60.0) / step);
    std::vector<std::vector<float>> frames;
    std::vector<double> mag (n / 2);
    for (size_t pos = 0; pos + n <= x.size(); pos += static_cast<size_t> (hop))
    {
        for (size_t i = 0; i < n; ++i)
            in[i] =
                x[pos + i] * static_cast<float> (
                                 0.5 - 0.5 * std::cos (kTwoPiD * static_cast<double> (i) / nfft));
        pffft_transform_ordered (setup, in, out, work, PFFFT_FORWARD);
        for (size_t k = 1; k < n / 2; ++k)
            mag[k] = std::hypot (out[2 * k], out[2 * k + 1]);
        std::vector<float> f (cells);
        for (size_t c = 0; c < cells; ++c)
        {
            const double bin =
                60.0 * std::exp2 (static_cast<double> (c) * step / 1200.0) * nfft / fs;
            const auto b = static_cast<size_t> (bin);
            const double t = bin - static_cast<double> (b);
            f[c] = b + 1 < n / 2 ? static_cast<float> ((1.0 - t) * mag[b] + t * mag[b + 1]) : 0.0f;
        }
        frames.push_back (std::move (f));
    }
    pffft_aligned_free (in);
    pffft_aligned_free (out);
    pffft_aligned_free (work);
    pffft_destroy_setup (setup);
    return frames;
}

/// Recorded DI: pitch error and attack placement against the input itself (see --di above).
void measureDi (const std::vector<std::string>& files, const std::vector<std::string>& candidates,
                const std::vector<int>& shifts, int block)
{
    std::printf ("Recorded DI. shift: per frame, the lag (cents) that best matches the output's "
                 "log-frequency spectrum to the input's, minus the setting, over frames within "
                 "40 dB of the loudest (chords too); attack: |output - input| 50 %% crossing, ms, "
                 "after the reported latency.\n");
    std::printf ("%-24s %-22s %4s | %6s %6s %6s %6s | %6s %6s %6s %6s\n", "file", "candidate", "st",
                 "frames", "sMed", "s95", "<5c%", "onsets", "attMed", "attP95", "<1ms%");
    for (const auto& path : files)
    {
        const auto wav = tools::readWav (path);
        if (!wav)
        {
            std::fprintf (stderr, "cannot read %s\n", path.c_str());
            continue;
        }
        const double fs = wav->sampleRate;
        const auto& in = wav->mono;
        std::string name = path.substr (path.find_last_of ("/\\") + 1);
        const auto quantile = [] (std::vector<double> v, double q) {
            if (v.empty())
                return 0.0;
            std::sort (v.begin(), v.end());
            return v[std::min (v.size() - 1,
                               static_cast<size_t> (q * static_cast<double> (v.size())))];
        };
        const auto share = [] (const std::vector<double>& v, double limit) {
            if (v.empty())
                return 0.0;
            return 100.0 *
                   static_cast<double> (
                       std::count_if (v.begin(), v.end(), [&] (double x) { return x < limit; })) /
                   static_cast<double> (v.size());
        };
        // Attacks of the input: the 1 ms envelope of everything above 2 kHz rising 15 dB over
        // its minimum of the last 30 ms, within 50 dB of the loudest, 60 ms apart.
        const int win = static_cast<int> (0.001 * fs);
        const auto hf = tp::rmsEnvelope (tp::highpass (in, fs, 2000.0), win);
        const auto envIn = tp::rmsEnvelope (in, win);
        double loudest = 0.0;
        for (double v : hf)
            loudest = std::max (loudest, v);
        std::vector<size_t> onsets;
        const auto back = static_cast<size_t> (0.030 * fs);
        for (size_t i = back; i < hf.size(); ++i)
        {
            if (!onsets.empty() && i < onsets.back() + static_cast<size_t> (0.060 * fs))
                continue;
            double low = hf[i];
            for (size_t k = i - back; k < i; k += 8)
                low = std::min (low, hf[k]);
            if (hf[i] > 5.6 * (low + 1.0e-9) && hf[i] > 0.00316 * loudest)
                onsets.push_back (i);
        }
        const int nfft = fs > 60000.0 ? 16384 : 8192;
        constexpr double kStep = 2.0; // cents per log-frequency cell
        const auto specIn = logSpectra (in, fs, nfft, nfft / 4, kStep);
        std::vector<double> energyIn;
        double loudestFrame = 0.0;
        for (const auto& f : specIn)
        {
            double e = 0.0;
            for (float v : f)
                e += static_cast<double> (v) * v;
            energyIn.push_back (e);
            loudestFrame = std::max (loudestFrame, e);
        }
        for (const auto& cand : candidates)
            for (int st : shifts)
            {
                auto shifter = makeCandidate (cand);
                auto out = tp::runShifter (shifter, in, fs, st, block);
                out = tp::advance (out, shifter.latency());
                std::vector<double> cents;
                const auto specOut = logSpectra (out, fs, nfft, nfft / 4, kStep);
                const int want = static_cast<int> (std::lround (100.0 * st / kStep));
                const int reach = static_cast<int> (150.0 / kStep);
                for (size_t fr = 0; fr < specIn.size() && fr < specOut.size(); ++fr)
                {
                    if (energyIn[fr] < 1.0e-4 * loudestFrame)
                        continue;
                    const auto& a = specIn[fr];
                    const auto& b = specOut[fr];
                    // Output cell c + lag against input cell c.
                    const auto score = [&] (int lag) {
                        double acc = 0.0;
                        for (int c = std::max (0, -lag); c < static_cast<int> (a.size()) &&
                                                         c + lag < static_cast<int> (b.size());
                             ++c)
                            acc += static_cast<double> (a[static_cast<size_t> (c)]) *
                                   b[static_cast<size_t> (c + lag)];
                        return acc;
                    };
                    int best = want;
                    double bestScore = -1.0;
                    for (int lag = want - reach; lag <= want + reach; ++lag)
                        if (const double sc = score (lag); sc > bestScore)
                        {
                            bestScore = sc;
                            best = lag;
                        }
                    const double l = score (best - 1), m = bestScore, r = score (best + 1);
                    const double den = l - 2.0 * m + r;
                    const double frac = den < 0.0 ? 0.5 * (l - r) / den : 0.0;
                    cents.push_back (std::abs ((best + frac - want) * kStep));
                }
                const auto envOut = tp::rmsEnvelope (out, win);
                std::vector<double> attack;
                for (size_t t : onsets)
                {
                    const size_t from =
                        t > 4 * static_cast<size_t> (win) ? t - 4 * static_cast<size_t> (win) : 0;
                    const size_t to = std::min (envIn.size(), t + 25 * static_cast<size_t> (win));
                    const auto ai = tp::attackShape (envIn, from, to, fs);
                    const auto ao = tp::attackShape (envOut, from, to, fs);
                    if (ai.found && ao.found)
                        attack.push_back (std::abs (ao.crossing - ai.crossing) / fs * 1000.0);
                }
                std::printf ("%-24.24s %-22.22s %+4d | %6zu %6.1f %6.1f %6.1f | %6zu %6.2f %6.2f "
                             "%6.1f\n",
                             name.c_str(), cand.c_str(), st, cents.size(), quantile (cents, 0.5),
                             quantile (cents, 0.95), share (cents, 5.0), attack.size(),
                             quantile (attack, 0.5), quantile (attack, 0.95), share (attack, 1.0));
                std::fflush (stdout);
            }
    }
}

} // namespace

int main (int argc, char** argv)
{
    std::vector<std::string> candidates { "wsola", "engine" };
    std::vector<int> shifts { -1, -2, -3, -5, -12, 12 };
    double rate = 48000.0;
    int block = 64;
    bool cpu = true;
    std::string wavDir, listenDir;
    std::vector<std::string> diFiles;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&] {
            return i + 1 < argc ? std::string (argv[++i]) : std::string();
        };
        if (a == "--candidates")
            candidates = split (next(), ',');
        else if (a == "--shifts")
        {
            shifts.clear();
            for (const auto& s : split (next(), ','))
                shifts.push_back (std::stoi (s));
        }
        else if (a == "--rate")
            rate = std::stod (next());
        else if (a == "--block")
            block = std::stoi (next());
        else if (a == "--no-cpu")
            cpu = false;
        else if (a == "--wav")
            wavDir = next();
        else if (a == "--listen")
            listenDir = next();
        else if (a == "--di")
            diFiles = split (next(), ',');
        else
        {
            std::fprintf (stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    if (!diFiles.empty())
    {
        measureDi (diFiles, candidates, shifts, block);
        return 0;
    }
    if (!listenDir.empty())
    {
        // DI files for ears: the input, the ideal shifted reference and each candidate.
        for (const auto& score : { strum(), riffMusic(), tp::scores::line() })
        {
            const auto in = tp::render (score, rate);
            writeWav (listenDir + "/" + score.name + "_input.wav", in, rate);
            for (int st : shifts)
            {
                char tag[32];
                std::snprintf (tag, sizeof (tag), "%+d", st);
                writeWav (listenDir + "/" + score.name + "_" + tag + "_ideal.wav",
                          tp::render (score, rate, std::exp2 (st / 12.0)), rate);
                for (const auto& name : candidates)
                {
                    auto shifter = makeCandidate (name);
                    auto out = tp::runShifter (shifter, in, rate, st, block);
                    // Remove the reported latency so every file lines up with the input.
                    out = tp::advance (out, shifter.latency());
                    std::string file = name;
                    for (auto& ch : file)
                        if (ch == ':' || ch == '=')
                            ch = '_';
                    writeWav (listenDir + "/" + score.name + "_" + tag + "_" + file + ".wav", out,
                              rate);
                }
            }
        }
        return 0;
    }

    std::printf ("Transpose bench at %.0f Hz, %d-sample blocks. lat/range/rep: ms; purity, floor, "
                 "preEch, alias: dB; timbr, warble, flam, attk: dB; rise: ms; pitch: cents.\n",
                 rate, block);
    printHeader();
    for (const auto& name : candidates)
    {
        auto shifter = makeCandidate (name);
        for (int st : shifts)
        {
            const auto e = tp::evaluate (shifter, rate, st, cpu, block);
            printRow (name, e);
            if (!wavDir.empty())
                for (const auto& score :
                     { tp::scores::chord(), tp::scores::riff(), tp::scores::line() })
                {
                    const auto in = tp::render (score, rate);
                    const auto out = tp::runShifter (shifter, in, rate, st, block);
                    char file[512];
                    std::snprintf (file, sizeof (file), "%s/%s_%s_%+d.wav", wavDir.c_str(),
                                   name.c_str(), score.name.c_str(), st);
                    writeWav (file, out, rate);
                }
        }
    }
    return 0;
}
