// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Sedef MIDI's names, in one place (the codes and bundle ids are in plugin/midi/CMakeLists.txt).
namespace sedef::midiplugin::product
{
inline constexpr const char* kName = "Sedef MIDI";
inline constexpr const char* kInstrumentName = "Sedef MIDI (Instrument)";
/// The virtual MIDI source the standalone app creates; instrument apps list it as an input.
inline constexpr const char* kVirtualPort = "Sedef MIDI";
/// The plug-ins' own virtual source, shared by every instance in a host.
inline constexpr const char* kPluginVirtualPort = "Sedef MIDI (plug-in)";
} // namespace sedef::midiplugin::product
