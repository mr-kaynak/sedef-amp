// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdateBridge.h"

#include "../PresetManager.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace sedef::update
{
namespace
{
using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;
} // namespace

#if SEDEF_UPDATE_CHECK
juce::File Service::storeFile()
{
    return plugin::PresetManager::userFolder().getSiblingFile ("update.json");
}
#endif

Bridge::Bridge (Emit emit, bool standalone) : emit_ (std::move (emit))
{
#if SEDEF_UPDATE_CHECK
    // "Restart <host> to use it": the host's own name, or "your DAW" when JUCE doesn't know it.
    juce::String host = standalone ? "Sedef" : juce::PluginHostType().getHostDescription();
    if (host == "Unknown")
        host.clear();
    service_->setHost (host, standalone);
    service_->addChangeListener (this);
#else
    juce::ignoreUnused (standalone);
#endif
}

Bridge::~Bridge()
{
#if SEDEF_UPDATE_CHECK
    service_->removeChangeListener (this);
#endif
}

void Bridge::changeListenerCallback (juce::ChangeBroadcaster*)
{
#if SEDEF_UPDATE_CHECK
    if (emit_)
        emit_ (service_->state());
#endif
}

juce::WebBrowserComponent::Options
Bridge::withNativeFunctions (juce::WebBrowserComponent::Options options)
{
    const auto fn = [&options] (const char* name, juce::WebBrowserComponent::NativeFunction f) {
        options = options.withNativeFunction (name, std::move (f));
    };

#if SEDEF_UPDATE_CHECK
    // Each call acts, then answers with the new state; later changes arrive as sedef:update.
    const auto call = [this] (auto action) {
        return [this, action] (const juce::Array<juce::var>& a, Completion done) {
            action (*service_, a);
            done (service_->state());
        };
    };
    fn ("sedef_update_getState", call ([] (Service& s, const auto&) { s.pageOpened(); }));
    fn ("sedef_update_check", call ([] (Service& s, const auto&) { s.checkNow(); }));
    fn ("sedef_update_setAutoCheck", call ([] (Service& s, const auto& a) {
            if (!a.isEmpty())
                s.setAutoCheck (static_cast<bool> (a[0]));
        }));
    fn ("sedef_update_skip", call ([] (Service& s, const auto& a) {
            s.skip (a.isEmpty() ? juce::String() : a[0].toString());
        }));
    fn ("sedef_update_install", call ([] (Service& s, const auto&) { s.install(); }));
    fn ("sedef_update_installManually",
        call ([] (Service& s, const auto&) { s.installManually(); }));
    fn ("sedef_update_cancel", call ([] (Service& s, const auto&) { s.cancel(); }));
    fn ("sedef_update_reveal", call ([] (Service& s, const auto&) { s.reveal(); }));
    // The standalone app quits the way the OS asks it to (its state is saved), after arranging for
    // the new version to open; in a plug-in this does nothing.
    fn ("sedef_update_restart", call ([] (Service& s, const auto&) {
            if (s.prepareRestart())
                juce::MessageManager::callAsync ([] {
                    if (auto* app = juce::JUCEApplicationBase::getInstance())
                        app->systemRequestedQuit();
                });
        }));
#else
    fn ("sedef_update_getState", [] (const juce::Array<juce::var>&, Completion done) {
        auto* o = new juce::DynamicObject();
        o->setProperty ("supported", false);
        o->setProperty ("currentVersion", SEDEF_VERSION);
        done (juce::var (o));
    });
#endif
    return options;
}

} // namespace sedef::update
