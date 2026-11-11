#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#endif

namespace aetherindex {

// Buffers of at least this size are mapped directly (POSIX) so that freeing them returns
// the pages to the OS. Some allocators (e.g. macOS libmalloc) keep large freed aligned
// blocks inside the process, which would hide memory released by release_rows().
inline constexpr std::size_t kDirectMappingBytes = std::size_t{1} << 20;

// C++17 aligned new/delete keep allocation and deallocation paired on all hosts.
template <class T, std::size_t Alignment = 64>
struct AlignedAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    template <class U>
    struct rebind {
        using other = AlignedAllocator<U, Alignment>;
    };

    AlignedAllocator() noexcept = default;
    template <class U>
    AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        static_assert(Alignment >= alignof(T) && (Alignment & (Alignment - 1)) == 0);
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
#if !defined(_WIN32)
        if (count * sizeof(T) >= kDirectMappingBytes) {
            // Page-aligned anonymous mapping: zero-filled, aligned beyond Alignment.
            void* mapped = ::mmap(nullptr, count * sizeof(T), PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0);
            if (mapped == MAP_FAILED) throw std::bad_alloc();
            return static_cast<T*>(mapped);
        }
#endif
        return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{Alignment}));
    }
    // std::vector passes the same count it allocated, which selects the same path.
    void deallocate(T* pointer, std::size_t count) noexcept {
#if !defined(_WIN32)
        if (count * sizeof(T) >= kDirectMappingBytes) {
            ::munmap(pointer, count * sizeof(T));
            return;
        }
#endif
        ::operator delete(pointer, std::align_val_t{Alignment});
    }
    template <class U>
    bool operator==(const AlignedAllocator<U, Alignment>&) const noexcept {
        return true;
    }
};

template <class T>
using AlignedVector = std::vector<T, AlignedAllocator<T>>;

}  // namespace aetherindex
