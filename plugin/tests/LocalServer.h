// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <juce_core/juce_core.h>
#include <map>
#include <mutex>

namespace sedef::update::test
{

/// HTTP/1.1 on 127.0.0.1, one connection at a time, canned responses per path. Records every
/// request head so tests can check exactly what the client sent.
class LocalServer final : private juce::Thread
{
public:
    LocalServer() : juce::Thread ("update test server")
    {
#if !JUCE_WINDOWS
        // A client that gives up (a cancelled request) must not kill the test with SIGPIPE.
        std::signal (SIGPIPE, SIG_IGN);
#endif
        REQUIRE (socket_.createListener (0, "127.0.0.1"));
        startThread();
    }

    ~LocalServer() override
    {
        signalThreadShouldExit();
        socket_.close();
        stopThread (5000);
    }

    juce::String url (const juce::String& path) const
    {
        return "http://127.0.0.1:" + juce::String (socket_.getBoundPort()) + path;
    }

    void serve (const juce::String& path, int status, const juce::MemoryBlock& body)
    {
        const std::scoped_lock l (lock_);
        routes_[path] = { status, body };
    }

    /// Holds every response back this long (a slow or stuck server).
    void delay (int ms) { delayMs_ = ms; }

    void serve (const juce::String& path, int status, const juce::String& body)
    {
        serve (path, status, juce::MemoryBlock (body.toRawUTF8(), body.getNumBytesAsUTF8()));
    }

    juce::StringArray requests() const
    {
        const std::scoped_lock l (lock_);
        return requests_;
    }

    int hits (const juce::String& path) const
    {
        int n = 0;
        for (const auto& r : requests())
            if (r.startsWith ("GET " + path + " "))
                ++n;
        return n;
    }

private:
    void run() override
    {
        while (!threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> client (socket_.waitForNextConnection());
            if (client == nullptr)
                return;
            juce::String head;
            char c = 0;
            while (!head.endsWith ("\r\n\r\n") && head.length() < 16384 &&
                   client->waitUntilReady (true, 2000) == 1 && client->read (&c, 1, true) == 1)
                head += c;

            const auto path = head.fromFirstOccurrenceOf (" ", false, false)
                                  .upToFirstOccurrenceOf (" ", false, false);
            std::pair<int, juce::MemoryBlock> response { 404, {} };
            {
                const std::scoped_lock l (lock_);
                requests_.add (head);
                if (const auto it = routes_.find (path); it != routes_.end())
                    response = it->second;
            }
            for (int waited = 0; waited < delayMs_ && !threadShouldExit(); waited += 10)
                juce::Thread::sleep (10);
            if (threadShouldExit())
                return;
            const auto& [status, body] = response;
            const juce::String header = "HTTP/1.1 " + juce::String (status) +
                                        " Test\r\nContent-Type: application/octet-stream\r\n"
                                        "Content-Length: " +
                                        juce::String (body.getSize()) +
                                        "\r\nConnection: close\r\n\r\n";
            send (*client, header.toRawUTF8(), header.getNumBytesAsUTF8());
            send (*client, body.getData(), body.getSize());
            client->close();
        }
    }

    static void send (juce::StreamingSocket& s, const void* data, size_t size)
    {
        auto* p = static_cast<const char*> (data);
        while (size > 0)
        {
            const int n = s.write (p, static_cast<int> (juce::jmin<size_t> (size, 1 << 15)));
            if (n <= 0)
                return;
            p += n;
            size -= static_cast<size_t> (n);
        }
    }

    juce::StreamingSocket socket_;
    std::atomic<int> delayMs_ { 0 };
    mutable std::mutex lock_;
    std::map<juce::String, std::pair<int, juce::MemoryBlock>> routes_;
    juce::StringArray requests_;
};

} // namespace sedef::update::test
