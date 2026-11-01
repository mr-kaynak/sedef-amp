// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_core/juce_core.h>

/// The mark browsers put on what they download, so the OS checks a file before it runs it: the
/// quarantine attribute on macOS (Gatekeeper asks before the installer opens) and the
/// Zone.Identifier stream on Windows (SmartScreen and the attachment manager). Linux has none.
namespace sedef::update
{

/// Marks `file` as downloaded from `sourceUrl`, linked from `pageUrl`, unless it already carries a
/// mark: a mark the OS or a browser set, or the user's approval of it, stays as it is. Returns
/// whether the file carries a mark afterwards (always false on Linux, and on file systems
/// without extended attributes or alternate streams).
bool markAsDownloaded (const juce::File& file, const juce::String& sourceUrl,
                       const juce::String& pageUrl);

bool hasDownloadMark (const juce::File& file);

} // namespace sedef::update
