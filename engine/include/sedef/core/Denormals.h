// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define SEDEF_HAS_SSE 1
#endif

namespace sedef
{

/// Enables flush-to-zero / denormals-are-zero for the current scope.
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
#if defined(SEDEF_HAS_SSE)
        previous_ = _mm_getcsr();
        _mm_setcsr (static_cast<unsigned int> (previous_ | 0x8040u));
#elif defined(__aarch64__) && !defined(_MSC_VER)
        std::uint64_t fpcr;
        asm volatile ("mrs %0, fpcr" : "=r"(fpcr));
        previous_ = fpcr;
        asm volatile ("msr fpcr, %0" ::"r"(fpcr | (1ull << 24)));
#endif
    }

    ~ScopedNoDenormals()
    {
#if defined(SEDEF_HAS_SSE)
        _mm_setcsr (static_cast<unsigned int> (previous_));
#elif defined(__aarch64__) && !defined(_MSC_VER)
        asm volatile ("msr fpcr, %0" ::"r"(previous_));
#endif
    }

    ScopedNoDenormals (const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

private:
    std::uint64_t previous_ = 0;
};

} // namespace sedef
