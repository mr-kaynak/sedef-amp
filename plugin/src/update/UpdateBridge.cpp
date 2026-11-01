// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UpdateBridge.h"

#include "../PresetManager.h"

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

Bridge::Bridge (Emit emit) : emit_ (std::move (emit))
{
#if SEDEF_UPDATE_CHECK
    service_->addChangeListener (this);
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
    fn ("sedef_update_download", call ([] (Service& s, const auto&) { s.download(); }));
    fn ("sedef_update_cancel", call ([] (Service& s, const auto&) { s.cancel(); }));
    fn ("sedef_update_reveal", call ([] (Service& s, const auto&) { s.reveal(); }));
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
