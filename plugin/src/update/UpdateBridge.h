// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#if SEDEF_UPDATE_CHECK
#include "UpdateService.h"
#endif

namespace sedef::update
{

/// The editor's side of the update checker: registers the sedef_update_* native functions and
/// forwards the shared service's changes as sedef:update events (ui/src/bridge/protocol.ts).
/// Built with SEDEF_UPDATE_CHECK off it only answers sedef_update_getState, with
/// { supported: false }, and the UI hides everything about updates.
class Bridge final : private juce::ChangeListener
{
public:
    using Emit = std::function<void (const juce::var& state)>;

    /// `standalone`: this editor belongs to the standalone app, which can restart itself after an
    /// update; in a plug-in the panel names the host to restart instead.
    Bridge (Emit emit, bool standalone);
    ~Bridge() override;

    juce::WebBrowserComponent::Options
    withNativeFunctions (juce::WebBrowserComponent::Options options);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Emit emit_;
#if SEDEF_UPDATE_CHECK
    juce::SharedResourcePointer<Service> service_;
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Bridge)
};

} // namespace sedef::update
