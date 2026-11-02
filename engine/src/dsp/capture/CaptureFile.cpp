// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/dsp/capture/CaptureFile.h"

#include "CaptureValidation.h"
#include "RtConvNet.h"
#include "sedef/core/Math.h"
#include "sedef/dsp/capture/RateBridge.h"
#include "sedef/dsp/capture/ReferenceStrum.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <convnet.h> // NeuralAmpModelerCore
#include <get_dsp.h>
#include <linear.h>
#include <lstm.h>
#include <mutex>
#include <wavenet/model.h>

namespace sedef::dsp
{
namespace
{
using capture::Limits;

std::atomic<int> gLiveModels { 0 };
thread_local int tDestroyedModels = 0;

/// Output beyond +36 dBFS at the model is a broken capture, not a loud one: it is clipped there,
/// and non-finite samples become silence, so nothing downstream ever sees them.
constexpr float kModelLimit = 64.0f;

/// The core registers each architecture from a static object in its source file, and a static
/// library only links the files something refers to. Referring to every parser here links them
/// (whose registrations then run at load time, before any of this); registering whatever is still
/// missing covers a toolchain that drops them anyway.
void linkArchitectures()
{
    static std::once_flag once;
    std::call_once (once, [] {
        auto& registry = nam::ConfigParserRegistry::instance();
        const std::pair<const char*, nam::ConfigParserFunction> parsers[] = {
            { "WaveNet", &nam::wavenet::create_config },
            { "LSTM", &nam::lstm::create_config },
            { "ConvNet", &nam::convnet::create_config },
            { "Linear", &nam::linear::create_config },
        };
        for (const auto& [name, parser] : parsers)
            if (!registry.has (name))
                registry.registerParser (name, parser);
    });
}

/// Builds the network the validated data describes. ConvNets use Sedef's real-time safe copy of
/// the core's ConvNet; everything else is the core's own implementation.
std::unique_ptr<nam::DSP> build (const nam::dspData& source)
{
    linkArchitectures();
    nam::dspData data = source; // get_dsp consumes the weights
    if (data.architecture == "ConvNet")
        return std::make_unique<capture::RtConvNet> (data.config, std::move (data.weights),
                                                     data.expected_sample_rate);
    nam::DspLoadOptions options;
    options.prewarm = false; // Reset() prewarms, once the buffers have their final size
    return nam::get_dsp (data, options);
}

/// Runs `dsp` over n samples in blocks the core was prepared for.
void runModel (nam::DSP& dsp, const float* in, float* out, int n)
{
    for (int start = 0; start < n; start += Limits::kModelBlock)
    {
        const int count = std::min (Limits::kModelBlock, n - start);
        auto* ip = const_cast<float*> (in + start);
        float* op = out + start;
        dsp.process (&ip, &op, count);
    }
}

std::string shortened (const char* what)
{
    std::string s (what != nullptr ? what : "");
    if (s.size() > 160)
        s = s.substr (0, 157) + "...";
    return s;
}
} // namespace

// -------------------------------------------------------------------------------------------------

struct CaptureFile::Impl
{
    capture::Validated v;
    float normalisationDb = 0.0f;
    float measuredDbfs = 0.0f;
};

CaptureFile::CaptureFile() : impl_ (std::make_unique<Impl>()) {}
CaptureFile::~CaptureFile() = default;

const CaptureInfo& CaptureFile::info() const noexcept
{
    return impl_->v.info;
}

float CaptureFile::normalisationDb() const noexcept
{
    return impl_->normalisationDb;
}

float CaptureFile::measuredDbfs() const noexcept
{
    return impl_->measuredDbfs;
}

CaptureFile::Result CaptureFile::parse (std::string_view text,
                                        std::optional<float> knownNormalisationDb)
{
    std::shared_ptr<CaptureFile> file (new CaptureFile());
    auto& d = *file->impl_;
    std::string error;
    if (!capture::validate (text, d.v, error))
        return { nullptr, error };

    if (knownNormalisationDb && std::isfinite (*knownNormalisationDb))
    {
        d.normalisationDb =
            std::clamp (*knownNormalisationDb, -kMaxNormalisationDb, kMaxNormalisationDb);
        d.measuredDbfs = kTargetDbfs - d.normalisationDb;
        return { file, {} };
    }

    const auto& info = d.v.info;
    try
    {
        auto dsp = build (d.v.data);
        dsp->Reset (info.sampleRate, Limits::kModelBlock); // sizes the buffers, prewarms

        // Level: the reference strum the built-in amps are matched with, at the model's own rate,
        // with the input calibrated for the reference interface.
        auto strum = capture::referenceStrum (info.sampleRate, capture::kReferenceSeconds);
        if (info.inputLevelDbu)
        {
            const float g =
                dbToGain (kReferenceInterfaceDbu - static_cast<float> (*info.inputLevelDbu));
            for (auto& s : strum)
                s *= g;
        }
        std::vector<float> out (strum.size());
        runModel (*dsp, strum.data(), out.data(), static_cast<int> (out.size()));

        const auto skip =
            static_cast<std::size_t> (capture::kReferenceSkipSeconds * info.sampleRate);
        double power = 0.0;
        bool finite = true;
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            finite = finite && std::isfinite (out[i]);
            if (i >= skip)
                power += static_cast<double> (out[i]) * out[i];
        }
        if (!finite)
            return { nullptr, "This capture produces invalid output; the file may be damaged." };
        const double rms =
            std::sqrt (power / static_cast<double> (std::max<std::size_t> (1, out.size() - skip)));
        const double level = 20.0 * std::log10 (std::max (rms, 1.0e-30));
        if (level < -120.0)
            return { nullptr, "This capture is silent: it makes no sound from a guitar signal." };
        if (level > 40.0)
            return { nullptr,
                     "This capture is far louder than any real amp; the file may be damaged." };
        d.measuredDbfs = static_cast<float> (level);
        d.normalisationDb = std::clamp (kTargetDbfs - static_cast<float> (level),
                                        -kMaxNormalisationDb, kMaxNormalisationDb);
    }
    catch (const std::bad_alloc&)
    {
        return { nullptr, "This capture is too large: there isn't enough memory to build it." };
    }
    catch (const std::exception& e)
    {
        return { nullptr, "The capture file is damaged: its model could not be built (" +
                              shortened (e.what()) + ")." };
    }
    catch (...)
    {
        return { nullptr, "The capture file is damaged: its model could not be built." };
    }
    return { file, {} };
}

// -------------------------------------------------------------------------------------------------

struct CaptureModel::Impl
{
    std::shared_ptr<const CaptureFile> file;
    std::unique_ptr<nam::DSP> dsp;
    capture::RateBridge bridge;
    std::vector<float> modelIn, modelOut;
    CaptureSpec spec;
    bool calibrated = false;
    float inputScale = 1.0f;
    float outputGain = 1.0f;
    int settleSamples = 0;
};

CaptureModel::CaptureModel() : impl_ (std::make_unique<Impl>())
{
    gLiveModels.fetch_add (1, std::memory_order_relaxed);
}

CaptureModel::~CaptureModel()
{
    impl_.reset();
    gLiveModels.fetch_sub (1, std::memory_order_relaxed);
    ++tDestroyedModels;
}

int CaptureModel::liveInstances() noexcept
{
    return gLiveModels.load (std::memory_order_relaxed);
}

int CaptureModel::destroyedOnThisThread() noexcept
{
    return tDestroyedModels;
}

bool CaptureModel::empty() const noexcept
{
    return impl_->dsp == nullptr;
}

const CaptureSpec& CaptureModel::spec() const noexcept
{
    return impl_->spec;
}

const CaptureFile* CaptureModel::file() const noexcept
{
    return impl_->file.get();
}

bool CaptureModel::calibrated() const noexcept
{
    return impl_->calibrated;
}

float CaptureModel::inputScale() const noexcept
{
    return impl_->inputScale;
}

float CaptureModel::outputGain() const noexcept
{
    return impl_->outputGain;
}

int CaptureModel::settleSamples() const noexcept
{
    return impl_->settleSamples;
}

std::unique_ptr<CaptureModel> CaptureFile::instantiate (const CaptureSpec& spec,
                                                        std::string* error) const
{
    const auto& info = impl_->v.info;
    const int zeroCrossings = capture::RateBridge::zeroCrossingsFor (
        spec.sampleRate, info.sampleRate, spec.latencySamples);
    const bool sameRate = std::llround (spec.sampleRate) == std::llround (info.sampleRate);
    if (!sameRate && zeroCrossings == 0)
    {
        if (error != nullptr)
            *error = "This capture can't play at " +
                     std::to_string (std::llround (spec.sampleRate)) + " Hz.";
        return nullptr;
    }
    try
    {
        auto model = std::make_unique<CaptureModel>();
        auto& m = *model->impl_;
        m.file = shared_from_this();
        m.spec = spec;
        m.bridge.prepare (spec.sampleRate, info.sampleRate, spec.maxBlockSize, spec.latencySamples,
                          sameRate ? capture::RateBridge::kDefaultZeroCrossings : zeroCrossings);
        m.modelIn.assign (static_cast<std::size_t> (m.bridge.maxModelBlock()), 0.0f);
        m.modelOut.assign (m.modelIn.size(), 0.0f);
        m.dsp = build (impl_->v.data);
        m.dsp->Reset (info.sampleRate, Limits::kModelBlock);
        m.calibrated = info.inputLevelDbu.has_value();
        m.inputScale = m.calibrated ? dbToGain (-static_cast<float> (*info.inputLevelDbu)) : 1.0f;
        m.outputGain = dbToGain (impl_->normalisationDb);
        const double hostPerModel = spec.sampleRate / info.sampleRate;
        m.settleSamples = static_cast<int> (std::ceil (static_cast<double> (info.receptiveField) *
                                                       hostPerModel)) +
                          spec.latencySamples + 64;
        return model;
    }
    catch (const std::exception& e)
    {
        if (error != nullptr)
            *error = "The capture could not be prepared (" + shortened (e.what()) + ").";
    }
    catch (...)
    {
        if (error != nullptr)
            *error = "The capture could not be prepared.";
    }
    return nullptr;
}

void CaptureModel::process (const float* in, float* out, int n) noexcept
{
    auto& d = *impl_;
    if (d.dsp == nullptr)
    {
        std::fill_n (out, n, 0.0f);
        return;
    }
    const int m = d.bridge.toModel (in, n, d.modelIn.data());
    runModel (*d.dsp, d.modelIn.data(), d.modelOut.data(), m);
    for (int i = 0; i < m; ++i)
    {
        float& y = d.modelOut[static_cast<std::size_t> (i)];
        if (!(std::abs (y) <= kModelLimit)) // also catches NaN
            y = std::isfinite (y) ? std::copysign (kModelLimit, y) : 0.0f;
    }
    d.bridge.fromModel (d.modelOut.data(), m, out, n);
    for (int i = 0; i < n; ++i)
        out[i] *= d.outputGain;
}

} // namespace sedef::dsp
