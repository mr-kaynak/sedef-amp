// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiResources.h"

#include "SedefUiData.h"
#include "ZipResources.h"

namespace sedef::plugin
{
namespace
{
juce::ZipFile* uiArchive()
{
    // The archive is the only resource in this binary-data target.
    static int size = 0;
    static const char* data =
        SedefUiData::getNamedResource (SedefUiData::namedResourceList[0], size);
    static juce::MemoryInputStream stream (data, static_cast<size_t> (size), false);
    static juce::ZipFile zip (stream);
    return &zip;
}
} // namespace

std::optional<juce::WebBrowserComponent::Resource> getUiResource (const juce::String& url)
{
    return resourceFromZip (*uiArchive(), url);
}

} // namespace sedef::plugin
