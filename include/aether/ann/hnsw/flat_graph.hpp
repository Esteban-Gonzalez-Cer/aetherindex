#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

#include "aether/core/aligned_allocator.hpp"

namespace aetherindex {

using InternalId = std::uint32_t;

// Fixed stride [degree, neighbor ...] in one 64-byte aligned allocation. Slots
// need not themselves be cache-line aligned (e.g. M0=32 gives 132 bytes).
class FlatGraphLayer {
   public:
    FlatGraphLayer(std::size_t capacity, std::size_t max_neighbors)
        : max_neighbors_(max_neighbors), stride_(max_neighbors + 1) {
        if (max_neighbors == 0 || max_neighbors >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("FlatGraph degree is not representable.");
        }
        reserve(capacity);
    }

    void reserve(std::size_t capacity) {
        if (capacity > buffer_.max_size() / stride_)
            throw std::length_error("FlatGraph capacity overflow.");
        buffer_.reserve(capacity * stride_);
    }
    void resize(std::size_t slots) {
        if (slots > buffer_.max_size() / stride_)
            throw std::length_error("FlatGraph size overflow.");
        buffer_.resize(slots * stride_, 0);
    }
    std::size_t slots() const noexcept { return buffer_.size() / stride_; }
    std::size_t capacity() const noexcept { return buffer_.capacity() / stride_; }
    std::size_t allocated_bytes() const noexcept {
        return buffer_.capacity() * sizeof(std::uint32_t);
    }
    std::size_t max_neighbors() const noexcept { return max_neighbors_; }
    std::size_t stride() const noexcept { return stride_; }
    const std::uint32_t* data() const noexcept { return buffer_.data(); }

    // Internal precondition: slot < slots(). Neighbors are dense internal IDs.
    std::uint32_t get_degree(std::size_t slot) const noexcept { return buffer_[slot * stride_]; }
    std::span<const InternalId> get_neighbors(std::size_t slot) const noexcept {
        return {buffer_.data() + slot * stride_ + 1, get_degree(slot)};
    }
    void set_neighbors(std::size_t slot, std::span<const InternalId> neighbors) {
        if (slot >= slots()) throw std::out_of_range("FlatGraph slot is absent.");
        if (neighbors.size() > max_neighbors_)
            throw std::invalid_argument("FlatGraph degree exceeds M_max.");
        auto* start = buffer_.data() + slot * stride_;
        if (!neighbors.empty()) std::memmove(start + 1, neighbors.data(), neighbors.size_bytes());
        start[0] = static_cast<std::uint32_t>(neighbors.size());
    }
    void append_neighbor(std::size_t slot, InternalId neighbor) {
        auto* start = buffer_.data() + slot * stride_;
        if (start[0] >= max_neighbors_) throw std::length_error("FlatGraph slot is full.");
        start[1 + start[0]] = neighbor;
        ++start[0];
    }

   private:
    std::size_t max_neighbors_;
    std::size_t stride_;
    AlignedVector<std::uint32_t> buffer_;
};

}  // namespace aetherindex
