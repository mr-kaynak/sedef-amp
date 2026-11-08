// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Dumps the parameter schema as JSON. The UI's browser mock consumes these files:
//   sedef-params > ui/src/generated/parameters.json
//   sedef-params --midi > ui/src/generated/midi-parameters.json   (Sedef MIDI)

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
    const bool midi = argc > 1 && std::string_view (argv[1]) == "--midi";
    const auto json = midi ? schema (sedef::midi::kParamSpecs) : schema (sedef::kParamSpecs);
    std::fputs (json.c_str(), stdout);
    return 0;
}
