// SPDX-License-Identifier: AGPL-3.0-or-later
#include "UiResources.h"

#include "SedefUiData.h"

#include <unordered_map>

namespace sedef::plugin
{
namespace
{
juce::ZipFile* uiArchive()
{
    // The archive is the only resource in this binary-data target.
    static int size = 0;
    static const char* data = SedefUiData::getNamedResource (SedefUiData::namedResourceList[0], size);
    static juce::MemoryInputStream stream (data, static_cast<size_t> (size), false);
    static juce::ZipFile zip (stream);
    return &zip;
}

juce::String mimeFor (const juce::String& path)
{
    static const std::unordered_map<juce::String, const char*> types {
        { "html", "text/html" },      { "js", "text/javascript" },  { "mjs", "text/javascript" },
        { "css", "text/css" },        { "json", "application/json" }, { "map", "application/json" },
        { "svg", "image/svg+xml" },   { "png", "image/png" },       { "jpg", "image/jpeg" },
        { "webp", "image/webp" },     { "ico", "image/x-icon" },    { "woff2", "font/woff2" },
        { "woff", "font/woff" },      { "ttf", "font/ttf" },        { "txt", "text/plain" },
    };
    const auto ext = path.fromLastOccurrenceOf (".", false, false).toLowerCase();
    if (const auto it = types.find (ext); it != types.end())
        return it->second;
    return "application/octet-stream";
}
} // namespace

std::optional<juce::WebBrowserComponent::Resource> getUiResource (const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf ("?", false, false).fromFirstOccurrenceOf ("/", false, false);
    if (path.isEmpty())
        path = "index.html";

    auto* zip = uiArchive();
    const auto* entry = zip->getEntry (path);
    if (entry == nullptr)
        entry = zip->getEntry ("./" + path);
    if (entry == nullptr)
        return std::nullopt;

    std::unique_ptr<juce::InputStream> in (zip->createStreamForEntry (*entry));
    if (in == nullptr)
        return std::nullopt;
    juce::MemoryBlock block;
    in->readIntoMemoryBlock (block);
    std::vector<std::byte> data (block.getSize());
    std::memcpy (data.data(), block.getData(), block.getSize());
    return juce::WebBrowserComponent::Resource { std::move (data), mimeFor (path) };
}

} // namespace sedef::plugin
