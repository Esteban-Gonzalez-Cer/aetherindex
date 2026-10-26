#pragma once

#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace aetherindex {

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
        return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t{Alignment}));
    }
    void deallocate(T* pointer, std::size_t) noexcept {
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
