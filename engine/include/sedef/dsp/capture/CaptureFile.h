// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace sedef::dsp
{

/// What a capture's metadata says it contains, from its `gear_type`.
enum class CaptureKind
{
    Unknown, ///< no or unrecognised gear type: the cab toggle is left to the player
    AmpOnly, ///< an amp (or preamp, or pedal into an amp): wants a cab after it
    FullRig  ///< amp and cab, already miked: plays without Sedef's cab
};

/// Everything Sedef knows about a capture, cleaned up for display. Strings are at most 120 bytes
/// of printable UTF-8; numbers are finite and within sane ranges, otherwise absent.
struct CaptureInfo
{
    std::string name;      ///< metadata "name" (may be empty)
    std::string modeledBy; ///< metadata "modeled_by"
    std::string gearMake, gearModel, gearType, toneType;
    std::string architecture;    ///< "WaveNet", "LSTM", "ConvNet" or "Linear"
    std::string version;         ///< .nam format version
    double sampleRate = 48000.0; ///< the rate the model runs at (48 kHz when the file doesn't say)
    bool sampleRateKnown = false;
    std::optional<double> loudnessDb;     ///< metadata "loudness"
    std::optional<double> inputLevelDbu;  ///< interface level that read 0 dBFS during the capture
    std::optional<double> outputLevelDbu; ///< level at the capture's output for 0 dBFS
    CaptureKind kind = CaptureKind::Unknown;
    long numWeights = 0;
    long macsPerSample = 0;  ///< multiply-adds per model-rate sample (estimate)
    long receptiveField = 0; ///< model-rate samples of input history the model reads
};

/// Where one runnable instance plays: the engine's rate, block size and the capture stage's
/// fixed latency (CaptureStage::latencySamples()).
struct CaptureSpec
{
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
    int latencySamples = 0;

    bool operator== (const CaptureSpec&) const = default;
};

class CaptureModel;

/// A validated Neural Amp Modeler capture (.nam), built with NeuralAmpModelerCore.
///
/// Every file is untrusted: parse() checks the JSON's size and nesting, every field the core
/// reads, the weight count against the architecture, and caps memory and CPU before the core
/// sees anything; then it builds the model once, checks that it produces finite output and
/// measures its level. It never throws and returns a message a player can act on.
/// Non-real-time only (it allocates, and takes tens of milliseconds).
class CaptureFile : public std::enable_shared_from_this<CaptureFile>
{
public:
    static constexpr std::size_t kMaxBytes = 24u << 20;
    /// Output level a capture is normalised to for the reference strum: the built-in amps' level.
    static constexpr float kTargetDbfs = -15.0f;
    /// The interface level the normalisation assumes (see CaptureStage::Settings::interfaceDbu).
    static constexpr float kReferenceInterfaceDbu = 12.0f;
    static constexpr float kMaxNormalisationDb = 40.0f;

    struct Result
    {
        std::shared_ptr<const CaptureFile> file; ///< null on failure
        std::string error;                       ///< why, when file is null
    };

    /// `knownNormalisationDb`: the level measured when this exact file was loaded before (a
    /// session restore, checked against the file's hash by the caller). Skips the build and the
    /// measurement, which take most of the time; instantiate() still builds and checks it.
    static Result parse (std::string_view text,
                         std::optional<float> knownNormalisationDb = std::nullopt);

    ~CaptureFile();
    CaptureFile (const CaptureFile&) = delete;
    CaptureFile& operator= (const CaptureFile&) = delete;

    const CaptureInfo& info() const noexcept;
    /// Gain (dB) that puts the reference strum at kTargetDbfs RMS, measured at load with the
    /// input calibrated for kReferenceInterfaceDbu.
    float normalisationDb() const noexcept;
    /// Level the reference strum measured at the model's output before normalisation (dBFS).
    float measuredDbfs() const noexcept;

    /// A runnable, prewarmed instance for one chain, already resting where the model rests (its
    /// first block starts there). Null (and *error) when the spec's latency is too short for the
    /// rate conversion. CaptureStage's latency fits every capture rate the validator accepts, and
    /// the full filter for 44.1 to 192 kHz.
    std::unique_ptr<CaptureModel> instantiate (const CaptureSpec& spec,
                                               std::string* error = nullptr) const;

private:
    struct Impl;
    CaptureFile();
    std::unique_ptr<Impl> impl_;
    friend class CaptureModel;
};

/// One runnable capture: the model, the rate conversion around it and its level. Built off the
/// audio thread (CaptureFile::instantiate) and handed over through CaptureStage, which owns the
/// real-time side. A default-constructed model is empty and stands for "no capture".
class CaptureModel
{
public:
    CaptureModel();
    ~CaptureModel();
    CaptureModel (const CaptureModel&) = delete;
    CaptureModel& operator= (const CaptureModel&) = delete;

    bool empty() const noexcept;
    const CaptureSpec& spec() const noexcept;
    /// Null when empty.
    const CaptureFile* file() const noexcept;

    /// Input scale applied on top of the player's input trim: 1 / input level, when the capture
    /// knows its input level (then multiplied by the interface level), otherwise 1.
    bool calibrated() const noexcept;
    float inputScale() const noexcept;
    /// Normalisation gain (linear).
    float outputGain() const noexcept;
    /// Host samples of silence after which the model is back in its prewarmed state.
    int settleSamples() const noexcept;

    /// Audio thread. Mono, n <= spec().maxBlockSize, `in` may alias `out`. The output is
    /// normalised and exactly spec().latencySamples late. Non-finite model output becomes silence.
    void process (const float* in, float* out, int n) noexcept;

    /// Instrumentation for tests: models alive now, models destroyed by the calling thread, and
    /// calls with more frames than the model was built for (refused: the output is silence).
    static int liveInstances() noexcept;
    static int destroyedOnThisThread() noexcept;
    static int oversizedCalls() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class CaptureFile;
};

} // namespace sedef::dsp
