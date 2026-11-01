// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <cstdint>
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
        auto& route = routes_[path];
        route.status = status;
        route.body = body;
        route.headers.clear();
    }

    void serve (const juce::String& path, int status, const juce::String& body)
    {
        serve (path, status, juce::MemoryBlock (body.toRawUTF8(), body.getNumBytesAsUTF8()));
    }

    void redirect (const juce::String& path, int status, const juce::String& location)
    {
        const std::scoped_lock l (lock_);
        auto& route = routes_[path];
        route.status = status;
        route.body.reset();
        route.headers = "Location: " + location + "\r\n";
    }

    /// Holds every response back this long (a slow or stuck server).
    void delay (int ms) { delayMs_ = ms; }
    /// Holds this path's responses back this long, on top of delay().
    void delay (const juce::String& path, int ms)
    {
        const std::scoped_lock l (lock_);
        routes_[path].delayMs = ms;
    }
    /// Every body longer than `bytes` stops for `ms` after that many bytes (a stalled transfer).
    void pauseAfter (size_t bytes, int ms)
    {
        pauseAfterBytes_ = bytes;
        pauseMs_ = ms;
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
            Route route;
            {
                const std::scoped_lock l (lock_);
                requests_.add (head);
                if (const auto it = routes_.find (path); it != routes_.end())
                    route = it->second;
            }
            if (!sleep (delayMs_ + route.delayMs))
                return;
            const auto& body = route.body;
            const juce::String header = "HTTP/1.1 " + juce::String (route.status) +
                                        " Test\r\nContent-Type: application/octet-stream\r\n" +
                                        route.headers +
                                        "Content-Length: " + juce::String (body.getSize()) +
                                        "\r\nConnection: close\r\n\r\n";
            send (*client, header.toRawUTF8(), header.getNumBytesAsUTF8());
            const auto* data = static_cast<const char*> (body.getData());
            size_t sent = 0;
            if (const size_t pause = pauseAfterBytes_; body.getSize() > pause)
            {
                send (*client, data, pause);
                sent = pause;
                if (!sleep (pauseMs_))
                    return;
            }
            send (*client, data + sent, body.getSize() - sent);
            client->close();
        }
    }

    /// False when the server is shutting down.
    bool sleep (int ms)
    {
        for (int waited = 0; waited < ms && !threadShouldExit(); waited += 10)
            juce::Thread::sleep (10);
        return !threadShouldExit();
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

    struct Route
    {
        int status = 404;
        juce::MemoryBlock body;
        juce::String headers; // extra header lines, each ending in CRLF
        int delayMs = 0;
    };

    juce::StreamingSocket socket_;
    std::atomic<int> delayMs_ { 0 };
    std::atomic<size_t> pauseAfterBytes_ { SIZE_MAX };
    std::atomic<int> pauseMs_ { 0 };
    mutable std::mutex lock_;
    std::map<juce::String, Route> routes_;
    juce::StringArray requests_;
};

} // namespace sedef::update::test
