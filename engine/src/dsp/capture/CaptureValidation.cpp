// SPDX-License-Identifier: AGPL-3.0-or-later
#include "CaptureValidation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Every rule here mirrors what NeuralAmpModelerCore v0.6.0 reads while it builds a model
// (NAM/get_dsp.cpp, wavenet/model.cpp, lstm.cpp, convnet.cpp, linear.cpp, activations.cpp): which
// keys it looks up (some through const operator[], which is undefined behaviour for a missing key),
// which counts it derives from them, and in which order it consumes weights (most loops do not
// check the end of the array). A file that passes cannot make the core read out of bounds, divide
// by zero, convert an out-of-range number or allocate without bound.

namespace sedef::dsp::capture
{
namespace
{
using json = nlohmann::json;
using std::int64_t;

struct Invalid
{
    std::string message;
};

[[noreturn]] void damaged (const std::string& what)
{
    throw Invalid { "The capture file is damaged: " + what + "." };
}

[[noreturn]] void unsupported (const std::string& sentence)
{
    throw Invalid { sentence };
}

[[noreturn]] void tooLarge (const std::string& what)
{
    throw Invalid { "This capture is too large to play in real time: " + what + "." };
}

std::string quoted (const std::string& s)
{
    return "\"" + s + "\"";
}

// --- the text ------------------------------------------------------------------------------------

/// Nesting depth and value count before parsing: nlohmann::json parses recursively and stores
/// every value, so both are bounded first.
void prescan (std::string_view text)
{
    int depth = 0;
    std::size_t separators = 0;
    bool inString = false, escaped = false;
    for (const char c : text)
    {
        if (inString)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        switch (c)
        {
        case '"':
            inString = true;
            break;
        case '[':
        case '{':
            if (++depth > Limits::kMaxDepth)
                damaged ("its JSON is nested more than " + std::to_string (Limits::kMaxDepth) +
                         " levels deep");
            break;
        case ']':
        case '}':
            --depth;
            break;
        case ',':
            if (++separators > Limits::kMaxValues)
                tooLarge ("it holds more than " + std::to_string (Limits::kMaxValues) + " numbers");
            break;
        default:
            break;
        }
    }
}

// --- fields --------------------------------------------------------------------------------------

const json* member (const json& object, const char* key)
{
    const auto it = object.find (key);
    return it == object.end() ? nullptr : &*it;
}

std::string field (const std::string& where, const char* key)
{
    return where.empty() ? quoted (key) : where + ": " + quoted (key);
}

int64_t wholeNumber (const json& v, const std::string& name, int64_t lo, int64_t hi,
                     bool limitIsResource)
{
    int64_t x = 0;
    if (v.is_number_unsigned())
    {
        const auto u = v.get<std::uint64_t>();
        x = u > static_cast<std::uint64_t> (std::numeric_limits<int64_t>::max() / 2)
                ? std::numeric_limits<int64_t>::max() / 2
                : static_cast<int64_t> (u);
    }
    else if (v.is_number_integer())
    {
        x = v.get<int64_t>();
    }
    else if (v.is_number_float())
    {
        const double d = v.get<double>();
        if (!std::isfinite (d) || d != std::floor (d) || std::abs (d) > 1.0e15)
            damaged (name + " must be a whole number");
        x = static_cast<int64_t> (d);
    }
    else
    {
        damaged (name + " must be a whole number");
    }
    if (x > hi && limitIsResource)
        tooLarge (name + " is " + std::to_string (x) + " (Sedef's limit is " + std::to_string (hi) +
                  ")");
    if (x < lo || x > hi)
        damaged (name + " must be from " + std::to_string (lo) + " to " + std::to_string (hi));
    return x;
}

int requiredInt (const json& object, const char* key, const std::string& where, int lo, int hi,
                 bool limitIsResource = true)
{
    const json* v = member (object, key);
    if (v == nullptr)
        damaged (field (where, key) + " is missing");
    return static_cast<int> (wholeNumber (*v, field (where, key), lo, hi, limitIsResource));
}

int optionalInt (const json& object, const char* key, int fallback, const std::string& where,
                 int lo, int hi, bool limitIsResource = true)
{
    const json* v = member (object, key);
    return v == nullptr
               ? fallback
               : static_cast<int> (wholeNumber (*v, field (where, key), lo, hi, limitIsResource));
}

bool requiredBool (const json& object, const char* key, const std::string& where)
{
    const json* v = member (object, key);
    if (v == nullptr || !v->is_boolean())
        damaged (field (where, key) + " must be true or false");
    return v->get<bool>();
}

bool optionalBool (const json& object, const char* key, bool fallback, const std::string& where)
{
    const json* v = member (object, key);
    if (v == nullptr)
        return fallback;
    if (!v->is_boolean())
        damaged (field (where, key) + " must be true or false");
    return v->get<bool>();
}

/// A number the core reads as float: finite and well inside float's range.
double floatNumber (const json& v, const std::string& name)
{
    if (!v.is_number())
        damaged (name + " must be a number");
    const double d = v.get<double>();
    if (!std::isfinite (d) || std::abs (d) > Limits::kMaxAbsWeight)
        damaged (name + " is out of range");
    return d;
}

std::vector<int> intArray (const json& object, const char* key, const std::string& where,
                           std::size_t maxCount, int lo, int hi)
{
    const json* v = member (object, key);
    if (v == nullptr || !v->is_array() || v->empty())
        damaged (field (where, key) + " must be a non-empty list");
    if (v->size() > maxCount)
        tooLarge (field (where, key) + " has " + std::to_string (v->size()) +
                  " entries (Sedef's limit is " + std::to_string (maxCount) + ")");
    std::vector<int> out;
    for (const auto& e : *v)
        out.push_back (static_cast<int> (wholeNumber (e, field (where, key), lo, hi, true)));
    return out;
}

// --- activations ---------------------------------------------------------------------------------

bool knownActivation (const std::string& name)
{
    static const char* const kNames[] = {
        "Tanh",    "Hardtanh", "Fasttanh",  "ReLU",          "LeakyReLU",     "PReLU",
        "Sigmoid", "SiLU",     "Hardswish", "LeakyHardtanh", "LeakyHardTanh", "Softsign"
    };
    return std::any_of (std::begin (kNames), std::end (kNames),
                        [&name] (const char* n) { return name == n; });
}

/// One activation config as ActivationConfig::from_json reads it. `channels`: how many rows it
/// is applied to (a per-channel PReLU must have one slope per row, or a single slope).
void activation (const json& a, int channels, const std::string& where)
{
    std::string type;
    if (a.is_string())
        type = a.get<std::string>();
    else if (a.is_object())
    {
        const json* t = member (a, "type");
        if (t == nullptr || !t->is_string())
            damaged (where + ": an activation has no \"type\"");
        type = t->get<std::string>();
    }
    else
        damaged (where + ": an activation must be a name or an object");

    if (!knownActivation (type))
        unsupported ("This capture uses the " + quoted (type.substr (0, 40)) +
                     " activation, which Sedef doesn't know.");
    if (!a.is_object())
        return;
    if (type == "PReLU")
    {
        if (const json* s = member (a, "negative_slope"))
            floatNumber (*s, where + ": the PReLU slope");
        else if (const json* list = member (a, "negative_slopes"))
        {
            if (!list->is_array() || list->empty())
                damaged (where + ": the PReLU slopes must be a non-empty list");
            if (list->size() != 1 && list->size() != static_cast<std::size_t> (channels))
                damaged (where + ": the PReLU has " + std::to_string (list->size()) +
                         " slopes for " + std::to_string (channels) + " channels");
            for (const auto& slope : *list)
                floatNumber (slope, where + ": a PReLU slope");
        }
    }
    else if (type == "LeakyReLU")
    {
        if (const json* s = member (a, "negative_slope"))
            floatNumber (*s, where + ": the LeakyReLU slope");
    }
    else if (type == "LeakyHardtanh" || type == "LeakyHardTanh")
    {
        for (const char* key : { "min_val", "max_val", "min_slope", "max_slope" })
            if (const json* s = member (a, key))
                floatNumber (*s, field (where, key));
    }
}

// --- cost ----------------------------------------------------------------------------------------

struct Cost
{
    int64_t weights = 0;       ///< what this model's own "weights" array must hold
    int64_t nestedWeights = 0; ///< a conditioning model's, which carries its own array
    int64_t macs = 0;
    int64_t bufferFloats = 0;
    int64_t receptiveField = 0;

    /// A nested model: its weights live in its own array, everything else adds up.
    void addNested (const Cost& o)
    {
        nestedWeights += o.weights + o.nestedWeights;
        macs += o.macs;
        bufferFloats += o.bufferFloats;
        receptiveField += o.receptiveField;
    }
};

void checkGroups (int in, int out, int groups, const std::string& what)
{
    if (groups < 1 || in % groups != 0 || out % groups != 0)
        damaged (what + ": " + std::to_string (groups) + " groups don't divide " +
                 std::to_string (in) + " inputs and " + std::to_string (out) + " outputs");
}

/// nam::Conv1D: kernel * out * in / groups weights (the same count when depthwise), then bias.
void conv1d (int in, int out, int kernel, bool bias, int dilation, int groups,
             const std::string& what, Cost& c)
{
    checkGroups (in, out, groups, what);
    const int64_t w = static_cast<int64_t> (kernel) * out * in / groups + (bias ? out : 0);
    c.weights += w;
    c.macs += w;
    const int64_t lookback = static_cast<int64_t> (kernel - 1) * dilation;
    c.bufferFloats += static_cast<int64_t> (in) * (2 * lookback + Limits::kModelBlock) +
                      static_cast<int64_t> (out) * Limits::kModelBlock;
}

/// nam::Conv1x1: out * in / groups weights, then bias.
void conv1x1 (int in, int out, bool bias, int groups, const std::string& what, Cost& c)
{
    checkGroups (in, out, groups, what);
    const int64_t w = static_cast<int64_t> (out) * in / groups + (bias ? out : 0);
    c.weights += w;
    c.macs += w;
    c.bufferFloats += static_cast<int64_t> (out) * Limits::kModelBlock;
}

// --- architectures -------------------------------------------------------------------------------

struct Model
{
    std::string version, architecture;
    json config;
    std::vector<float> weights;
    double sampleRate = 48000.0;
    bool sampleRateKnown = false;
    Cost cost;
};

void validateModel (const json& j, int expectIn, int expectOut, int depth, const char* role,
                    Model& out);

/// CPU, history and memory caps; checked as the cost accumulates, so a hostile config is turned
/// away before anything is sized from it.
void checkCost (const Model& m)
{
    const auto& c = m.cost;
    if (c.macs > Limits::kMaxMacsPerSample)
        tooLarge ("it needs about " + std::to_string (c.macs / 1000) +
                  " thousand operations per sample (Sedef's limit is " +
                  std::to_string (Limits::kMaxMacsPerSample / 1000) + " thousand)");
    const auto maxField =
        m.architecture == "Linear" ? Limits::kMaxLinearTaps : Limits::kMaxReceptiveField;
    if (m.architecture != "LSTM" && c.receptiveField > maxField)
        tooLarge ("it looks " + std::to_string (c.receptiveField) +
                  " samples back (Sedef's limit is " + std::to_string (maxField) + ")");
    if (c.bufferFloats > Limits::kMaxBufferFloats)
        tooLarge ("it needs " + std::to_string (c.bufferFloats * 4 / (1 << 20)) +
                  " MB of buffers (Sedef's limit is " +
                  std::to_string (Limits::kMaxBufferFloats * 4 / (1 << 20)) + " MB)");
    if (c.weights + c.nestedWeights > Limits::kMaxWeights)
        tooLarge ("it needs " + std::to_string (c.weights + c.nestedWeights) +
                  " weights (Sedef's limit is " + std::to_string (Limits::kMaxWeights) + ")");
}

struct Film
{
    bool active = false, shift = true;
    int groups = 1;
};

Film filmParams (const json& layer, const char* key, const std::string& where)
{
    const json* f = member (layer, key);
    if (f == nullptr || (f->is_boolean() && !f->get<bool>()))
        return {};
    if (!f->is_object())
        damaged (field (where, key) + " must be false or an object");
    Film p;
    p.active = optionalBool (*f, "active", true, where);
    p.shift = optionalBool (*f, "shift", true, where);
    p.groups = optionalInt (*f, "groups", 1, where, 1, Limits::kMaxChannels, false);
    return p;
}

void film (int condition, int dim, const Film& p, const std::string& what, Cost& c)
{
    if (p.active)
        conv1x1 (condition, (p.shift ? 2 : 1) * dim, true, p.groups, what, c);
}

enum class Gating
{
    None,
    Gated,
    Blended
};

Gating gatingFrom (const json& v, const std::string& where)
{
    if (!v.is_string())
        damaged (where + ": \"gating_mode\" must be \"none\", \"gated\" or \"blended\"");
    const auto s = v.get<std::string>();
    if (s == "gated")
        return Gating::Gated;
    if (s == "blended")
        return Gating::Blended;
    if (s == "none")
        return Gating::None;
    damaged (where + ": \"gating_mode\" must be \"none\", \"gated\" or \"blended\"");
}

void waveNet (const json& config, int expectIn, int expectOut, int depth, Model& m)
{
    const json* layers = member (config, "layers");
    if (layers == nullptr || !layers->is_array() || layers->empty())
        damaged ("the WaveNet has no layer arrays");
    if (layers->size() > static_cast<std::size_t> (Limits::kMaxLayerArrays))
        tooLarge ("it has " + std::to_string (layers->size()) + " layer arrays (Sedef's limit is " +
                  std::to_string (Limits::kMaxLayerArrays) + ")");
    const json* headScale = member (config, "head_scale");
    if (headScale == nullptr)
        damaged ("\"head_scale\" is missing");
    floatNumber (*headScale, "\"head_scale\"");

    const int inChannels = optionalInt (config, "in_channels", 1, "", 1, Limits::kMaxChannels);
    if (inChannels != expectIn)
        unsupported ("This capture has " + std::to_string (inChannels) +
                     " inputs; Sedef plays captures with one input and one output.");

    for (const auto& layer : *layers)
        if (layer.is_object() && member (layer, "slimmable") != nullptr)
            unsupported ("This capture is a slimmable WaveNet, which Sedef can't load yet.");

    Cost& cost = m.cost;
    int conditionDim = inChannels;
    if (const json* cd = member (config, "condition_dsp"); cd != nullptr && !cd->is_null())
    {
        if (depth > 0)
            unsupported ("This capture nests conditioning models, which Sedef can't load.");
        const json& first = (*layers)[0];
        if (!first.is_object())
            damaged ("layer array 1 is not an object");
        const int conditionSize =
            requiredInt (first, "condition_size", "layer array 1", 1, Limits::kMaxChannels);
        Model nested;
        validateModel (*cd, inChannels, conditionSize, depth + 1, "its conditioning model", nested);
        if (std::llround (nested.sampleRate) != std::llround (m.sampleRate) ||
            nested.sampleRateKnown != m.sampleRateKnown)
            damaged ("its conditioning model runs at a different sample rate");
        cost.addNested (nested.cost);
        // The core builds the conditioning model through its own unchecked path: hand it the
        // validated copy instead.
        json clean = json::object();
        clean["version"] = nested.version;
        clean["architecture"] = nested.architecture;
        clean["config"] = std::move (nested.config);
        clean["weights"] = nested.weights;
        clean["sample_rate"] = m.sampleRate;
        m.config["condition_dsp"] = std::move (clean);
        conditionDim = conditionSize;
    }

    int prevChannels = 0, prevHeadSize = 0;
    for (std::size_t a = 0; a < layers->size(); ++a)
    {
        const json& L = (*layers)[a];
        const std::string where = "layer array " + std::to_string (a + 1);
        if (!L.is_object())
            damaged (where + " is not an object");

        const int channels = requiredInt (L, "channels", where, 1, Limits::kMaxChannels);
        const int bottleneck =
            optionalInt (L, "bottleneck", channels, where, 1, Limits::kMaxChannels);
        const int input = requiredInt (L, "input_size", where, 1, Limits::kMaxChannels);
        const int condition = requiredInt (L, "condition_size", where, 1, Limits::kMaxChannels);
        const int groupsIn =
            optionalInt (L, "groups_input", 1, where, 1, Limits::kMaxChannels, false);
        const int groupsMixin =
            optionalInt (L, "groups_input_mixin", 1, where, 1, Limits::kMaxChannels, false);

        bool layer1x1 = true;
        int layer1x1Groups = 1;
        if (const json* o = member (L, "layer1x1"))
        {
            if (!o->is_object())
                damaged (field (where, "layer1x1") + " must be an object");
            layer1x1 = requiredBool (*o, "active", where + " layer1x1");
            layer1x1Groups =
                requiredInt (*o, "groups", where + " layer1x1", 1, Limits::kMaxChannels, false);
        }

        int headSize = 0, headDilation = 1, headKernel = 1;
        bool headBias = false;
        if (const json* h = member (L, "head"); h != nullptr && !h->is_null())
        {
            if (!h->is_object())
                damaged (field (where, "head") + " must be an object");
            headSize = requiredInt (*h, "out_channels", where + " head", 1, Limits::kMaxChannels);
            headDilation =
                optionalInt (*h, "head_dilation", 1, where + " head", 1, Limits::kMaxDilation);
            headKernel = requiredInt (*h, "kernel_size", where + " head", 1, Limits::kMaxKernel);
            headBias = requiredBool (*h, "bias", where + " head");
        }
        else if (member (L, "head_size") != nullptr)
        {
            headSize = requiredInt (L, "head_size", where, 1, Limits::kMaxChannels);
            headBias = requiredBool (L, "head_bias", where);
        }
        else
            damaged (where + " has no head");

        const auto dilations =
            intArray (L, "dilations", where, Limits::kMaxLayers, 1, Limits::kMaxDilation);
        const std::size_t n = dilations.size();

        std::vector<int> kernels;
        const json* k1 = member (L, "kernel_size");
        const json* kn = member (L, "kernel_sizes");
        if ((k1 != nullptr) == (kn != nullptr))
            damaged (where + " needs exactly one of \"kernel_size\" and \"kernel_sizes\"");
        if (k1 != nullptr)
            kernels.assign (n, static_cast<int> (wholeNumber (*k1, field (where, "kernel_size"), 1,
                                                              Limits::kMaxKernel, true)));
        else
        {
            kernels =
                intArray (L, "kernel_sizes", where, Limits::kMaxLayers, 1, Limits::kMaxKernel);
            if (kernels.size() != n)
                damaged (field (where, "kernel_sizes") + " doesn't match the dilations");
        }

        // Activations: one, or one per layer; applied to the bottleneck rows.
        const json* act = member (L, "activation");
        if (act == nullptr)
            damaged (field (where, "activation") + " is missing");
        if (act->is_array())
        {
            if (act->size() != n)
                damaged (field (where, "activation") + " doesn't match the dilations");
            for (const auto& e : *act)
                activation (e, bottleneck, where);
        }
        else
            activation (*act, bottleneck, where);

        // Gating, as parse_config_json reads it (gating_mode, else the legacy "gated").
        std::vector<Gating> gating (n, Gating::None);
        const json* secondary = member (L, "secondary_activation");
        if (const json* g = member (L, "gating_mode"))
        {
            if (g->is_array())
            {
                if (g->size() != n)
                    damaged (field (where, "gating_mode") + " doesn't match the dilations");
                for (std::size_t i = 0; i < n; ++i)
                    gating[i] = gatingFrom ((*g)[i], where);
                if (secondary != nullptr && secondary->is_array())
                {
                    if (secondary->size() != n)
                        damaged (field (where, "secondary_activation") +
                                 " doesn't match the dilations");
                    for (std::size_t i = 0; i < n; ++i)
                        if (gating[i] != Gating::None)
                            activation ((*secondary)[i], bottleneck, where);
                }
                else if (secondary != nullptr &&
                         std::any_of (gating.begin(), gating.end(),
                                      [] (Gating x) { return x != Gating::None; }))
                    activation (*secondary, bottleneck, where);
            }
            else
            {
                gating.assign (n, gatingFrom (*g, where));
                if (secondary != nullptr && gating[0] != Gating::None)
                {
                    if (secondary->is_array())
                        damaged (field (where, "secondary_activation") +
                                 " must be a single activation here");
                    activation (*secondary, bottleneck, where);
                }
            }
        }
        else if (member (L, "gated") != nullptr)
        {
            gating.assign (n, requiredBool (L, "gated", where) ? Gating::Gated : Gating::None);
        }

        bool head1x1 = false;
        int head1x1Out = channels, head1x1Groups = 1;
        if (const json* o = member (L, "head1x1"))
        {
            if (!o->is_object())
                damaged (field (where, "head1x1") + " must be an object");
            head1x1 = requiredBool (*o, "active", where + " head1x1");
            head1x1Out =
                requiredInt (*o, "out_channels", where + " head1x1", 1, Limits::kMaxChannels);
            head1x1Groups =
                requiredInt (*o, "groups", where + " head1x1", 1, Limits::kMaxChannels, false);
        }

        const Film convPre = filmParams (L, "conv_pre_film", where);
        const Film convPost = filmParams (L, "conv_post_film", where);
        const Film mixinPre = filmParams (L, "input_mixin_pre_film", where);
        const Film mixinPost = filmParams (L, "input_mixin_post_film", where);
        const Film actPre = filmParams (L, "activation_pre_film", where);
        const Film actPost = filmParams (L, "activation_post_film", where);
        const Film layer1x1Post = filmParams (L, "layer1x1_post_film", where);
        const Film head1x1Post = filmParams (L, "head1x1_post_film", where);

        // Shapes the core asserts on (or silently gets wrong in release builds).
        if (!layer1x1 && bottleneck != channels)
            damaged (where + ": without layer1x1 the bottleneck must equal the channels");
        if (layer1x1Post.active && !layer1x1)
            damaged (where + ": layer1x1_post_film needs layer1x1");
        if (head1x1Post.active && !head1x1)
            damaged (where + ": head1x1_post_film needs head1x1");
        if (condition != conditionDim)
            damaged (where + ": \"condition_size\" is " + std::to_string (condition) +
                     " but the condition has " + std::to_string (conditionDim) + " channels");
        const int headRows = head1x1 ? head1x1Out : bottleneck;
        if (a == 0)
        {
            if (input != inChannels)
                damaged (where + ": \"input_size\" doesn't match the model's inputs");
        }
        else
        {
            if (input != prevChannels)
                damaged (where + ": \"input_size\" doesn't match the previous array's channels");
            if (channels != prevHeadSize || headRows != prevHeadSize)
                damaged (where + " doesn't match the previous array's head size");
        }

        // Weights, in the order LayerArray::set_weights_ consumes them.
        conv1x1 (input, channels, false, 1, where + " rechannel", cost);
        for (std::size_t l = 0; l < n; ++l)
        {
            const std::string what = where + " layer " + std::to_string (l + 1);
            const int z = gating[l] != Gating::None ? 2 * bottleneck : bottleneck;
            conv1d (channels, z, kernels[l], true, dilations[l], groupsIn, what, cost);
            conv1x1 (condition, z, false, groupsMixin, what + " input mixin", cost);
            if (layer1x1)
                conv1x1 (bottleneck, channels, true, layer1x1Groups, what + " layer1x1", cost);
            if (head1x1)
                conv1x1 (bottleneck, head1x1Out, true, head1x1Groups, what + " head1x1", cost);
            film (condition, channels, convPre, what + " FiLM", cost);
            film (condition, z, convPost, what + " FiLM", cost);
            film (condition, condition, mixinPre, what + " FiLM", cost);
            film (condition, z, mixinPost, what + " FiLM", cost);
            film (condition, z, actPre, what + " FiLM", cost);
            film (condition, bottleneck, actPost, what + " FiLM", cost);
            if (layer1x1)
                film (condition, channels, layer1x1Post, what + " FiLM", cost);
            if (head1x1)
                film (condition, head1x1Out, head1x1Post, what + " FiLM", cost);
            cost.receptiveField += static_cast<int64_t> (kernels[l] - 1) * dilations[l];
            cost.bufferFloats += static_cast<int64_t> (3 * z + 2 * channels) * Limits::kModelBlock;
        }
        conv1d (headRows, headSize, headKernel, headBias, headDilation, 1, where + " head", cost);
        cost.receptiveField += static_cast<int64_t> (headKernel - 1) * headDilation;
        prevChannels = channels;
        prevHeadSize = headSize;
        checkCost (m);
    }

    // Optional post-stack head.
    int lastOut = prevHeadSize;
    if (const json* h = member (config, "head"); h != nullptr && !h->is_null())
    {
        if (!h->is_object())
            damaged ("\"head\" must be an object");
        if (const json* in = member (*h, "in_channels"); in != nullptr && !in->is_null())
            if (wholeNumber (*in, "\"head\": \"in_channels\"", 1, Limits::kMaxChannels, true) !=
                prevHeadSize)
                damaged ("the head's inputs don't match the last layer array");
        const int channels = requiredInt (*h, "channels", "head", 1, Limits::kMaxChannels);
        const int outChannels = requiredInt (*h, "out_channels", "head", 1, Limits::kMaxChannels);
        const auto kernels =
            intArray (*h, "kernel_sizes", "head", Limits::kMaxHeadConvs, 1, Limits::kMaxKernel);
        const json* act = member (*h, "activation");
        if (act == nullptr)
            damaged ("the head has no activation");
        int cin = prevHeadSize;
        for (std::size_t i = 0; i < kernels.size(); ++i)
        {
            const int cout = i + 1 == kernels.size() ? outChannels : channels;
            activation (*act, cin, "head");
            conv1d (cin, cout, kernels[i], true, 1, 1, "head", cost);
            cost.receptiveField += kernels[i] - 1;
            cin = cout;
        }
        lastOut = outChannels;
    }
    if (lastOut != expectOut)
        unsupported ("This capture has " + std::to_string (lastOut) +
                     " outputs; Sedef plays captures with one input and one output.");
    cost.weights += 1; // head_scale
    cost.receptiveField += 1;
}

void lstm (const json& config, int expectIn, int expectOut, Model& m)
{
    const int layers = requiredInt (config, "num_layers", "", 1, Limits::kMaxLstmLayers);
    const int inputSize = requiredInt (config, "input_size", "", 1, Limits::kMaxChannels);
    const int hidden = requiredInt (config, "hidden_size", "", 1, Limits::kMaxChannels);
    const int in = optionalInt (config, "in_channels", 1, "", 1, Limits::kMaxChannels);
    const int out = optionalInt (config, "out_channels", 1, "", 1, Limits::kMaxChannels);
    if (in != expectIn || out != expectOut)
        unsupported ("This capture has " + std::to_string (in) + " inputs and " +
                     std::to_string (out) +
                     " outputs; Sedef plays captures with one input and one output.");
    // LSTM::process copies in_channels values into an input vector of input_size.
    if (inputSize != in)
        damaged ("\"input_size\" doesn't match the model's inputs");
    Cost& c = m.cost;
    for (int l = 0; l < layers; ++l)
    {
        const int64_t cellIn = l == 0 ? inputSize : hidden;
        const int64_t gates = 4 * static_cast<int64_t> (hidden) * (cellIn + hidden);
        c.weights += gates + 4 * static_cast<int64_t> (hidden) + 2 * hidden; // W, b, h0, c0
        c.macs += gates + 12 * static_cast<int64_t> (hidden);
    }
    c.weights += static_cast<int64_t> (out) * hidden + out;
    c.macs += static_cast<int64_t> (out) * hidden;
    // No receptive field; LSTM::GetPrewarmSamples settles for half a second.
    c.receptiveField = static_cast<int64_t> (0.5 * m.sampleRate);
}

void convNet (const json& config, int expectIn, int expectOut, Model& m)
{
    const int channels = requiredInt (config, "channels", "", 1, Limits::kMaxChannels);
    const auto dilations =
        intArray (config, "dilations", "", Limits::kMaxLayers, 1, Limits::kMaxDilation);
    const bool batchnorm = requiredBool (config, "batchnorm", "");
    const json* act = member (config, "activation");
    if (act == nullptr)
        damaged ("\"activation\" is missing");
    activation (*act, channels, "");
    const int groups = optionalInt (config, "groups", 1, "", 1, Limits::kMaxChannels, false);
    const int in = optionalInt (config, "in_channels", 1, "", 1, Limits::kMaxChannels);
    const int out = optionalInt (config, "out_channels", 1, "", 1, Limits::kMaxChannels);
    if (in != expectIn || out != expectOut)
        unsupported ("This capture has " + std::to_string (in) + " inputs and " +
                     std::to_string (out) +
                     " outputs; Sedef plays captures with one input and one output.");
    Cost& c = m.cost;
    c.receptiveField = 1;
    for (std::size_t b = 0; b < dilations.size(); ++b)
    {
        const int blockIn = b == 0 ? in : channels;
        conv1d (blockIn, channels, 2, !batchnorm, dilations[b], groups,
                "block " + std::to_string (b + 1), c);
        if (batchnorm)
            c.weights += 4 * static_cast<int64_t> (channels) + 1; // mean, var, weight, bias, eps
        c.receptiveField += dilations[b];
    }
    c.weights += static_cast<int64_t> (out) * channels + out;
    c.macs += static_cast<int64_t> (out) * channels;
}

void linear (const json& config, int expectIn, int expectOut, Model& m)
{
    const int taps =
        requiredInt (config, "receptive_field", "", 1, static_cast<int> (Limits::kMaxLinearTaps));
    const bool bias = requiredBool (config, "bias", "");
    const int in = optionalInt (config, "in_channels", 1, "", 1, Limits::kMaxChannels);
    const int out = optionalInt (config, "out_channels", 1, "", 1, Limits::kMaxChannels);
    if (in != expectIn || out != expectOut)
        unsupported ("This capture has " + std::to_string (in) + " inputs and " +
                     std::to_string (out) +
                     " outputs; Sedef plays captures with one input and one output.");
    if (const json* impl = member (config, "implementation"))
    {
        static const char* const kNames[] = { "auto", "direct",          "legacy",         "old",
                                              "fft",  "partitioned_fft", "partitioned-fft" };
        if (!impl->is_string() ||
            std::none_of (std::begin (kNames), std::end (kNames),
                          [impl] (const char* s) { return impl->get<std::string>() == s; }))
            damaged ("\"implementation\" is not one Sedef knows");
    }
    Cost& c = m.cost;
    c.weights = taps + (bias ? 1 : 0);
    // Direct up to 1024 taps, partitioned FFT above (linear.cpp's dispatch table).
    c.macs = std::min<int64_t> (taps, 4096);
    c.bufferFloats = 6 * static_cast<int64_t> (taps) + 32 * Limits::kModelBlock;
    c.receptiveField = taps;
}

// --- the envelope --------------------------------------------------------------------------------

void checkVersion (const json* v, std::string& out)
{
    if (v == nullptr || !v->is_string())
        damaged ("it has no format \"version\"");
    out = v->get<std::string>();
    int part[3] = { -1, -1, -1 };
    std::size_t i = 0;
    for (int p = 0; p < 3; ++p)
    {
        int value = 0, digits = 0;
        while (i < out.size() && out[i] >= '0' && out[i] <= '9' && digits < 6)
        {
            value = value * 10 + (out[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0 || (p < 2 && (i >= out.size() || out[i] != '.')))
            damaged ("its format version " + quoted (out.substr (0, 20)) + " isn't valid");
        part[p] = value;
        if (p < 2)
            ++i;
    }
    if (i != out.size())
        damaged ("its format version " + quoted (out.substr (0, 20)) + " isn't valid");
    // NeuralAmpModelerCore v0.6.0 reads 0.5.0 up to 0.7.x (EARLIEST_ / LATEST_FULLY_SUPPORTED_).
    if (part[0] == 0 && part[1] < 5)
        unsupported ("This capture uses .nam format " + out +
                     ", which is older than Sedef reads (0.5.0 and later).");
    if (part[0] > 0 || part[1] > 7)
        unsupported ("This capture uses .nam format " + out +
                     ", which is newer than Sedef reads (up to 0.7). A Sedef update may read it.");
}

void validateModel (const json& j, int expectIn, int expectOut, int depth, const char* role,
                    Model& m)
{
    if (!j.is_object())
        damaged (std::string (role) + " is not a JSON object");
    checkVersion (member (j, "version"), m.version);

    const json* arch = member (j, "architecture");
    if (arch == nullptr || !arch->is_string())
        damaged (std::string (role) + " has no \"architecture\"");
    m.architecture = arch->get<std::string>();
    const bool known = m.architecture == "WaveNet" || m.architecture == "LSTM" ||
                       m.architecture == "ConvNet" || m.architecture == "Linear";
    if (!known)
    {
        if (m.architecture == "Sequential" || m.architecture == "SlimmableContainer")
            unsupported ("This capture uses the " + m.architecture +
                         " architecture, which Sedef can't load yet.");
        unsupported ("This capture uses an architecture Sedef doesn't know (" +
                     quoted (m.architecture.substr (0, 40)) + ").");
    }

    if (const json* rate = member (j, "sample_rate"); rate != nullptr && !rate->is_null())
    {
        if (!rate->is_number())
            damaged ("\"sample_rate\" must be a number");
        const double r = rate->get<double>();
        if (r != -1.0)
        {
            if (!std::isfinite (r) || r < Limits::kMinSampleRate || r > Limits::kMaxSampleRate)
                unsupported ("This capture was made at " + std::to_string (std::llround (r)) +
                             " Hz; Sedef plays captures made at 16 to 192 kHz.");
            m.sampleRate = r;
            m.sampleRateKnown = true;
        }
    }

    const json* config = member (j, "config");
    if (config == nullptr || !config->is_object())
        damaged (std::string (role) + " has no \"config\"");
    m.config = *config;

    const json* weights = member (j, "weights");
    if (weights == nullptr || !weights->is_array())
        damaged (std::string (role) + " has no \"weights\"");
    if (weights->size() > static_cast<std::size_t> (Limits::kMaxWeights))
        tooLarge ("it has " + std::to_string (weights->size()) + " weights (Sedef's limit is " +
                  std::to_string (Limits::kMaxWeights) + ")");
    m.weights.reserve (weights->size());
    for (const auto& w : *weights)
    {
        if (!w.is_number())
            damaged ("a weight is not a number");
        const double d = w.get<double>();
        if (!std::isfinite (d) || std::abs (d) > Limits::kMaxAbsWeight)
            damaged ("a weight is out of range");
        m.weights.push_back (static_cast<float> (d));
    }

    if (m.architecture == "WaveNet")
        waveNet (*config, expectIn, expectOut, depth, m);
    else if (m.architecture == "LSTM")
        lstm (*config, expectIn, expectOut, m);
    else if (m.architecture == "ConvNet")
        convNet (*config, expectIn, expectOut, m);
    else
        linear (*config, expectIn, expectOut, m);

    checkCost (m);
    if (static_cast<int64_t> (m.weights.size()) != m.cost.weights)
        damaged ("it has " + std::to_string (m.weights.size()) + " weights, but its " +
                 m.architecture + " needs " + std::to_string (m.cost.weights));
}

// --- metadata ------------------------------------------------------------------------------------

/// Printable UTF-8, at most 120 bytes, cut on a character boundary.
std::string cleanString (const json* v)
{
    if (v == nullptr || !v->is_string())
        return {};
    const auto& s = v->get_ref<const std::string&>();
    std::string out;
    std::size_t i = 0;
    while (i < s.size() && out.size() < 120)
    {
        const auto c = static_cast<unsigned char> (s[i]);
        std::size_t len = c < 0x80           ? 1
                          : (c >> 5) == 0x6  ? 2
                          : (c >> 4) == 0xE  ? 3
                          : (c >> 3) == 0x1E ? 4
                                             : 0;
        if (len == 0 || i + len > s.size())
        {
            ++i; // invalid lead byte or truncated sequence: dropped
            continue;
        }
        bool valid = true;
        for (std::size_t k = 1; k < len; ++k)
            valid = valid && (static_cast<unsigned char> (s[i + k]) & 0xC0) == 0x80;
        if (!valid)
        {
            ++i;
            continue;
        }
        if (len == 1 && (c < 0x20 || c == 0x7F))
            out += ' ';
        else if (out.size() + len <= 120)
            out.append (s, i, len);
        else
            break;
        i += len;
    }
    const auto first = out.find_first_not_of (' ');
    if (first == std::string::npos)
        return {};
    return out.substr (first, out.find_last_not_of (' ') - first + 1);
}

std::optional<double> cleanNumber (const json* v, double lo, double hi)
{
    if (v == nullptr || !v->is_number())
        return std::nullopt;
    const double d = v->get<double>();
    if (!std::isfinite (d) || d < lo || d > hi)
        return std::nullopt;
    return d;
}

void readMetadata (const json* m, CaptureInfo& info, json& clean)
{
    clean = json::object();
    if (m == nullptr || !m->is_object())
        return;
    info.name = cleanString (member (*m, "name"));
    info.modeledBy = cleanString (member (*m, "modeled_by"));
    info.gearMake = cleanString (member (*m, "gear_make"));
    info.gearModel = cleanString (member (*m, "gear_model"));
    info.gearType = cleanString (member (*m, "gear_type"));
    info.toneType = cleanString (member (*m, "tone_type"));
    info.loudnessDb = cleanNumber (member (*m, "loudness"), -120.0, 60.0);
    info.inputLevelDbu = cleanNumber (member (*m, "input_level_dbu"), -60.0, 60.0);
    info.outputLevelDbu = cleanNumber (member (*m, "output_level_dbu"), -60.0, 60.0);
    // Only numbers the core can read reach it.
    if (info.loudnessDb)
        clean["loudness"] = *info.loudnessDb;
    if (info.inputLevelDbu)
        clean["input_level_dbu"] = *info.inputLevelDbu;
    if (info.outputLevelDbu)
        clean["output_level_dbu"] = *info.outputLevelDbu;

    // The trainer's gear types (nam.train.metadata.GearType).
    const auto& g = info.gearType;
    if (g == "amp_cab" || g == "amp_pedal_cab")
        info.kind = CaptureKind::FullRig;
    else if (g == "amp" || g == "pedal_amp" || g == "preamp")
        info.kind = CaptureKind::AmpOnly;
}

} // namespace

bool validate (std::string_view text, Validated& out, std::string& error)
{
    try
    {
        if (text.size() > CaptureFile::kMaxBytes)
            tooLarge ("the file is " + std::to_string (text.size() >> 20) +
                      " MB (Sedef's limit is " + std::to_string (CaptureFile::kMaxBytes >> 20) +
                      " MB)");
        prescan (text);
        const json root = json::parse (text.begin(), text.end(), nullptr, false);
        if (root.is_discarded())
            unsupported ("This isn't a NAM capture: the file is not valid JSON.");
        if (!root.is_object())
            unsupported ("This isn't a NAM capture: the file is not a JSON object.");
        for (const char* key : { "version", "architecture", "config", "weights" })
            if (member (root, key) == nullptr)
                unsupported (std::string ("This isn't a NAM capture: it has no \"") + key + "\".");

        Model m;
        validateModel (root, 1, 1, 0, "the model", m);

        Validated v;
        readMetadata (member (root, "metadata"), v.info, v.data.metadata);
        v.info.architecture = m.architecture;
        v.info.version = m.version;
        v.info.sampleRate = m.sampleRate;
        v.info.sampleRateKnown = m.sampleRateKnown;
        v.info.numWeights = static_cast<long> (m.cost.weights + m.cost.nestedWeights);
        v.info.macsPerSample = static_cast<long> (m.cost.macs);
        v.info.receptiveField = static_cast<long> (m.cost.receptiveField);

        v.data.version = m.version;
        v.data.architecture = m.architecture;
        v.data.config = std::move (m.config);
        v.data.weights = std::move (m.weights);
        v.data.expected_sample_rate = m.sampleRate;
        out = std::move (v);
        return true;
    }
    catch (const Invalid& e)
    {
        error = e.message;
    }
    catch (const std::bad_alloc&)
    {
        error = "This capture is too large: there isn't enough memory to read it.";
    }
    catch (const std::exception&)
    {
        error = "The capture file is damaged: it could not be read.";
    }
    return false;
}

} // namespace sedef::dsp::capture
