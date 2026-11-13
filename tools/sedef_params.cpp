// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Dumps the parameter schema as JSON. The UI's browser mock consumes these files:
//   sedef-params > ui/src/generated/parameters.json
// with --pedals, the pedal registry (board/PedalList.inc) for the UI's pedalboard:
//   sedef-params --pedals > ui/src/generated/pedals.json
// with --midi, Sedef MIDI's parameters:
//   sedef-params --midi > ui/src/generated/midi-parameters.json
// and with --grooves, the drum grooves (docs/drums.md) for the metronome panel's groove picker:
//   sedef-params --grooves > ui/src/generated/grooves.json

#include "sedef/board/Board.h"
#include "sedef/dsp/drums/Grooves.h"
#include "sedef/midi/MidiParameters.h"
#include "sedef/params/Parameters.h"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace
{
std::string quote (std::string_view s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

const char* kindName (sedef::ParamKind k)
{
    switch (k)
    {
    case sedef::ParamKind::Float:
        return "float";
    case sedef::ParamKind::Int:
        return "int";
    case sedef::ParamKind::Bool:
        return "bool";
    case sedef::ParamKind::Choice:
        return "choice";
    }
    return "float";
}

std::string number (float v)
{
    char buf[32];
    std::snprintf (buf, sizeof buf, "%.6g", static_cast<double> (v));
    return buf;
}
} // namespace

std::string pedals()
{
    std::string json = "{\n  \"version\": 1,\n  \"pedals\": [\n";
    for (std::size_t i = 0; i < sedef::kNumPedals; ++i)
    {
        const auto& p = sedef::kPedals[i];
        json += "    {\"id\": " + quote (p.id) + ", \"name\": " + quote (p.name) +
                ", \"category\": " + quote (sedef::categoryName (p.category)) +
                ", \"rank\": " + std::to_string (p.rank) +
                ", \"enable\": " + quote (sedef::spec (p.enable).id) + "}";
        json += i + 1 < sedef::kNumPedals ? ",\n" : "\n";
    }
    json += "  ],\n  \"legacy\": [";
    const auto legacy = sedef::Board::legacy();
    for (std::size_t i = 0; i < legacy.size(); ++i)
        json += (i > 0 ? ", " : "") + quote (sedef::pedalInfo (legacy[i]).id);
    return json + "]\n}\n";
}

std::string grooves()
{
    std::string json = "{\n  \"version\": 1,\n  \"grooves\": [\n";
    const auto list = sedef::dsp::drums::grooves();
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const auto& g = list[i];
        json += "    {\"name\": " + quote (g.name) + ", \"feel\": " + quote (g.feel) +
                ", \"signature\": " + std::to_string (g.signature) + "}";
        json += i + 1 < list.size() ? ",\n" : "\n";
    }
    return json + "  ]\n}\n";
}

std::string schema (std::span<const sedef::ParamSpec> specs)
{
    std::string json = "{\n  \"version\": 1,\n  \"parameters\": [\n";
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        const auto& s = specs[i];
        json += "    {\"id\": " + quote (s.id) + ", \"name\": " + quote (s.name) +
                ", \"group\": " + quote (s.group) + ", \"kind\": \"" + kindName (s.kind) +
                "\", \"min\": " + number (s.min) + ", \"max\": " + number (s.max) +
                ", \"default\": " + number (s.defaultValue) + ", \"skew\": " + number (s.skew) +
                ", \"unit\": " + quote (s.unit) +
                ", \"automatable\": " + (s.automatable ? "true" : "false") + ", \"choices\": [";
        for (int c = 0; c < s.numChoices(); ++c)
            json += (c > 0 ? ", " : "") + quote (s.choiceLabel (c));
        json += "]}";
        json += i + 1 < specs.size() ? ",\n" : "\n";
    }
    return json + "  ]\n}\n";
}

int main (int argc, char** argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "";
    const auto json = mode == "--pedals"    ? pedals()
                      : mode == "--midi"    ? schema (sedef::midi::kParamSpecs)
                      : mode == "--grooves" ? grooves()
                                            : schema (sedef::kParamSpecs);
    std::fputs (json.c_str(), stdout);
    return 0;
}
