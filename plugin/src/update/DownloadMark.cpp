// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DownloadMark.h"

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h> // the LSQuarantine keys
#endif

namespace sedef::update
{

#if JUCE_MAC

namespace
{
/// Owns one Core Foundation reference.
template <typename Ref>
class CFOwned
{
public:
    explicit CFOwned (Ref ref = nullptr) : ref_ (ref) {}
    ~CFOwned()
    {
        if (ref_ != nullptr)
            CFRelease (ref_);
    }
    CFOwned (const CFOwned&) = delete;
    CFOwned& operator= (const CFOwned&) = delete;
    Ref get() const noexcept { return ref_; }

private:
    Ref ref_;
};

CFOwned<CFStringRef> cfString (const juce::String& text)
{
    return CFOwned<CFStringRef> (
        CFStringCreateWithCString (nullptr, text.toRawUTF8(), kCFStringEncodingUTF8));
}

CFOwned<CFURLRef> fileUrl (const juce::File& file)
{
    const auto path = cfString (file.getFullPathName());
    return CFOwned<CFURLRef> (
        path.get() == nullptr
            ? nullptr
            : CFURLCreateWithFileSystemPath (nullptr, path.get(), kCFURLPOSIXPathStyle, false));
}

CFOwned<CFURLRef> webUrl (const juce::String& url)
{
    const auto text = cfString (url);
    return CFOwned<CFURLRef> (url.isEmpty() || text.get() == nullptr
                                  ? nullptr
                                  : CFURLCreateWithString (nullptr, text.get(), nullptr));
}
} // namespace

bool hasDownloadMark (const juce::File& file)
{
    const auto url = fileUrl (file);
    CFTypeRef properties = nullptr;
    if (url.get() == nullptr || !CFURLCopyResourcePropertyForKey (
                                    url.get(), kCFURLQuarantinePropertiesKey, &properties, nullptr))
        return false;
    const CFOwned<CFTypeRef> owned (properties);
    return properties != nullptr;
}

bool markAsDownloaded (const juce::File& file, const juce::String& sourceUrl,
                       const juce::String& pageUrl)
{
    if (!file.existsAsFile())
        return false;
    if (hasDownloadMark (file))
        return true;
    const auto url = fileUrl (file);
    const auto agent = cfString ("Sedef Amp");
    const auto source = webUrl (sourceUrl);
    const auto page = webUrl (pageUrl);
    const CFOwned<CFMutableDictionaryRef> properties (CFDictionaryCreateMutable (
        nullptr, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (url.get() == nullptr || properties.get() == nullptr)
        return false;
    // What a browser records for a download; LaunchServices adds the time and an id.
    CFDictionarySetValue (properties.get(), kLSQuarantineTypeKey, kLSQuarantineTypeWebDownload);
    if (agent.get() != nullptr)
        CFDictionarySetValue (properties.get(), kLSQuarantineAgentNameKey, agent.get());
    if (source.get() != nullptr)
        CFDictionarySetValue (properties.get(), kLSQuarantineDataURLKey, source.get());
    if (page.get() != nullptr)
        CFDictionarySetValue (properties.get(), kLSQuarantineOriginURLKey, page.get());
    return CFURLSetResourcePropertyForKey (url.get(), kCFURLQuarantinePropertiesKey,
                                           properties.get(), nullptr) &&
           hasDownloadMark (file);
}

#elif JUCE_WINDOWS

namespace
{
/// The alternate data stream browsers write next to a download (NTFS and ReFS only).
juce::File zoneStream (const juce::File& file)
{
    return juce::File (file.getFullPathName() + ":Zone.Identifier");
}
} // namespace

bool hasDownloadMark (const juce::File& file)
{
    juce::FileInputStream in (zoneStream (file));
    return in.openedOk();
}

bool markAsDownloaded (const juce::File& file, const juce::String& sourceUrl,
                       const juce::String& pageUrl)
{
    if (!file.existsAsFile())
        return false;
    if (hasDownloadMark (file))
        return true;
    {
        juce::FileOutputStream out (zoneStream (file));
        if (!out.openedOk() || !out.setPosition (0) || out.truncate().failed())
            return false;
        // Zone 3 is the Internet zone, as browsers write it.
        out << "[ZoneTransfer]\r\nZoneId=3\r\n";
        if (pageUrl.isNotEmpty())
            out << "ReferrerUrl=" << pageUrl << "\r\n";
        if (sourceUrl.isNotEmpty())
            out << "HostUrl=" << sourceUrl << "\r\n";
        out.flush();
        if (out.getStatus().failed())
            return false;
    }
    return hasDownloadMark (file);
}

#else

bool hasDownloadMark (const juce::File&)
{
    return false;
}

bool markAsDownloaded (const juce::File&, const juce::String&, const juce::String&)
{
    return false; // Linux has no such mark; the archive is only shown, never run
}

#endif

} // namespace sedef::update
