#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

#include "aether/core/aligned_allocator.hpp"

namespace aetherindex {

struct SnapshotAccess;
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

    // Concurrent variants. Writers (serialized per slot by the caller's lock) store every
    // word atomically; lock-free readers load them atomically. Relaxed order: a reader may
    // see a list mid-update, but every word it sees is a neighbor that was valid at some
    // point. Publication of new nodes is ordered elsewhere (release/acquire on the graph's
    // entry point and the caller's locks). Loads compile to plain loads on arm64/x86.
    void set_neighbors_atomic(std::size_t slot, std::span<const InternalId> neighbors) {
        if (slot >= slots()) throw std::out_of_range("FlatGraph slot is absent.");
        if (neighbors.size() > max_neighbors_)
            throw std::invalid_argument("FlatGraph degree exceeds M_max.");
        auto* start = buffer_.data() + slot * stride_;
        for (std::size_t i = 0; i < neighbors.size(); ++i)
            std::atomic_ref<std::uint32_t>(start[1 + i]).store(neighbors[i], std::memory_order_relaxed);
        std::atomic_ref<std::uint32_t>(start[0]).store(static_cast<std::uint32_t>(neighbors.size()),
                                                       std::memory_order_relaxed);
    }
    void append_neighbor_atomic(std::size_t slot, InternalId neighbor) {
        auto* start = buffer_.data() + slot * stride_;
        const std::uint32_t degree = std::atomic_ref<std::uint32_t>(start[0]).load(std::memory_order_relaxed);
        if (degree >= max_neighbors_) throw std::length_error("FlatGraph slot is full.");
        std::atomic_ref<std::uint32_t>(start[1 + degree]).store(neighbor, std::memory_order_relaxed);
        std::atomic_ref<std::uint32_t>(start[0]).store(degree + 1, std::memory_order_relaxed);
    }
    // Copies a slot's neighbors with atomic loads into `out` (capacity >= max_neighbors).
    std::size_t copy_neighbors_atomic(std::size_t slot, InternalId* out) const noexcept {
        auto* start = const_cast<std::uint32_t*>(buffer_.data()) + slot * stride_;
        const std::size_t degree = std::min<std::size_t>(
            std::atomic_ref<std::uint32_t>(start[0]).load(std::memory_order_relaxed), max_neighbors_);
        for (std::size_t i = 0; i < degree; ++i)
            out[i] = std::atomic_ref<std::uint32_t>(start[1 + i]).load(std::memory_order_relaxed);
        return degree;
    }

   private:
    friend struct ::aetherindex::SnapshotAccess;  // Index snapshots (aether_persistence).
    std::size_t max_neighbors_;
    std::size_t stride_;
    AlignedVector<std::uint32_t> buffer_;
};

}  // namespace aetherindex
