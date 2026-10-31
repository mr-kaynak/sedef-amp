// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <optional>

namespace sedef::plugin
{

/// Serves the embedded web UI (a zip produced from ui/dist at build time) to the WebView.
std::optional<juce::WebBrowserComponent::Resource> getUiResource (const juce::String& url);

} // namespace sedef::plugin
