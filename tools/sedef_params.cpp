// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Dumps the parameter schema as JSON. The UI's browser mock consumes this file:
//   sedef-params > ui/src/generated/parameters.json

#include "sedef/params/Parameters.h"

#include <cstdio>
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

int main()
{
    std::string json = "{\n  \"version\": 1,\n  \"parameters\": [\n";
    for (std::size_t i = 0; i < sedef::kNumParams; ++i)
    {
        const auto& s = sedef::kParamSpecs[i];
        json += "    {\"id\": " + quote (s.id) + ", \"name\": " + quote (s.name) +
                ", \"group\": " + quote (s.group) + ", \"kind\": \"" + kindName (s.kind) +
                "\", \"min\": " + number (s.min) + ", \"max\": " + number (s.max) +
                ", \"default\": " + number (s.defaultValue) + ", \"skew\": " + number (s.skew) +
                ", \"unit\": " + quote (s.unit) +
                ", \"automatable\": " + (s.automatable ? "true" : "false") + ", \"choices\": [";
        for (int c = 0; c < s.numChoices(); ++c)
            json += (c > 0 ? ", " : "") + quote (s.choiceLabel (c));
        json += "]}";
        json += i + 1 < sedef::kNumParams ? ",\n" : "\n";
    }
    json += "  ]\n}\n";
    std::fputs (json.c_str(), stdout);
    return 0;
}
