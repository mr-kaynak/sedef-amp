// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sedef/core/RtHandoff.h"
#include "sedef/core/SpscRing.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace sedef;

TEST_CASE ("SpscRing transfers every item in order across threads", "[core][concurrency]")
{
    SpscRing<int> ring (1024);
    constexpr int total = 200000;
    std::thread producer ([&] {
        int next = 0;
        while (next < total)
        {
            const int chunk[4] = { next, next + 1, next + 2, next + 3 };
            next += static_cast<int> (ring.push (chunk, std::min (4, total - next)));
        }
    });

    int expected = 0;
    int buffer[64];
    bool ordered = true;
    while (expected < total)
    {
        const auto n = ring.pop (buffer, 64);
        for (size_t i = 0; i < n; ++i)
            ordered = ordered && buffer[i] == expected++;
    }
    producer.join();
    CHECK (ordered);
    CHECK (ring.available() == 0);
}

TEST_CASE ("RtHandoff swaps objects and frees retired ones off the audio thread", "[core][concurrency]")
{
    struct Counted
    {
        explicit Counted (int& live) : live_ (live) { ++live_; }
        ~Counted() { --live_; }
        int& live_;
    };

    int live = 0;
    {
        RtHandoff<Counted> h;
        CHECK (h.acquire() == nullptr);
        h.publish (std::make_unique<Counted> (live));
        Counted* first = h.acquire();
        CHECK (first != nullptr);
        h.publish (std::make_unique<Counted> (live));
        h.publish (std::make_unique<Counted> (live)); // replaces the unconsumed one
        CHECK (live == 2);
        Counted* second = h.acquire();
        CHECK (second != first);
        CHECK (live == 2); // `first` is retired, not freed by the audio thread
        h.collect();
        CHECK (live == 1);
    }
    CHECK (live == 0);
}
