#pragma once

#include <cstddef>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

namespace aetherindex {

// Read prefetch hint into all cache levels; a no-op where the compiler has no intrinsic.
inline void prefetch_read(const void* pointer) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(pointer, 0, 3);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(static_cast<const char*>(pointer), _MM_HINT_T0);
#else
    (void)pointer;
#endif
}

// Prefetches every 64-byte line of [pointer, pointer + bytes), not only the first one.
inline void prefetch_bytes(const void* pointer, std::size_t bytes) noexcept {
    const auto* first = static_cast<const char*>(pointer);
    for (std::size_t offset = 0; offset < bytes; offset += 64) prefetch_read(first + offset);
}

}  // namespace aetherindex
