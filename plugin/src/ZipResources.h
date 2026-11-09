// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <optional>
#include <unordered_map>

namespace sedef::plugin
{

/// The MIME type the WebView needs for a file of the embedded UI.
inline juce::String mimeFor (const juce::String& path)
{
    static const std::unordered_map<juce::String, const char*> types {
        { "html", "text/html" },    { "js", "text/javascript" },    { "mjs", "text/javascript" },
        { "css", "text/css" },      { "json", "application/json" }, { "map", "application/json" },
        { "svg", "image/svg+xml" }, { "png", "image/png" },         { "jpg", "image/jpeg" },
        { "webp", "image/webp" },   { "ico", "image/x-icon" },      { "woff2", "font/woff2" },
        { "woff", "font/woff" },    { "ttf", "font/ttf" },          { "txt", "text/plain" },
    };
    const auto ext = path.fromLastOccurrenceOf (".", false, false).toLowerCase();
    if (const auto it = types.find (ext); it != types.end())
        return it->second;
    return "application/octet-stream";
}

/// Serves a file of a web UI packed into a zip (built from a Vite dist folder) to the WebView;
/// the root is `page`.
inline std::optional<juce::WebBrowserComponent::Resource>
resourceFromZip (juce::ZipFile& zip, const juce::String& url, const char* page = "index.html")
{
    auto path =
        url.upToFirstOccurrenceOf ("?", false, false).fromFirstOccurrenceOf ("/", false, false);
    if (path.isEmpty())
        path = page;

    const auto* entry = zip.getEntry (path);
    if (entry == nullptr)
        entry = zip.getEntry ("./" + path);
    if (entry == nullptr)
        return std::nullopt;

    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (*entry));
    if (in == nullptr)
        return std::nullopt;
    juce::MemoryBlock block;
    in->readIntoMemoryBlock (block);
    std::vector<std::byte> data (block.getSize());
    std::memcpy (data.data(), block.getData(), block.getSize());
    return juce::WebBrowserComponent::Resource { std::move (data), mimeFor (path) };
}

} // namespace sedef::plugin
