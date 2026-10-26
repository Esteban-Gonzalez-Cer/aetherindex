#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "aether/core/aligned_allocator.hpp"
#include "aether/core/types.hpp"

namespace aetherindex {

enum class InsertStatus { Success, DuplicateId, DimensionMismatch, InvalidEmbedding };

// Sole owner of the embeddings. Reads and writes must not overlap.
// Discard all views before calling insert()/reserve(), even if it fails.
// Passing a view from this store directly to insert() is supported, but that
// view must not be used afterwards. Destruction also invalidates all views.
class VectorStore {
   public:
    // Throws invalid_argument for a zero dimension or unsupported metric.
    explicit VectorStore(std::size_t dimension, DistanceKind metric = DistanceKind::Cosine);

    VectorStore(const VectorStore&) = delete;
    VectorStore& operator=(const VectorStore&) = delete;
    VectorStore(VectorStore&&) = delete;
    VectorStore& operator=(VectorStore&&) = delete;

    // Cosine stores unit vectors and rejects L2 norms < 1e-7. SquaredEuclidean
    // preserves raw FP32 components. The caller's span is never modified.
    // Rejections leave contents unchanged. Allocation/length exceptions propagate
    // with the same logical contents; capacities and view validity may change.
    InsertStatus insert(VectorId id, std::span<const EmbeddingValue> embedding);

    std::optional<std::span<const EmbeddingValue>> find(VectorId id) const;
    std::size_t size() const noexcept;
    std::size_t dimension() const noexcept;
    DistanceKind distance_kind() const noexcept;
    bool contains(VectorId id) const;
    std::span<const VectorId> ids() const noexcept;

    // Logical dimensions are unchanged. Physical rows have zero padding and
    // start on 64-byte boundaries. Positions are dense, in insertion order.
    std::size_t padded_dimension() const noexcept;
    std::optional<std::size_t> position_of(VectorId id) const;
    // Precondition: position < size(). Read-only hot path, no hash lookup.
    const float* data_by_position(std::size_t position) const noexcept;
    void reserve(std::size_t count);

   private:
    const std::size_t dimension_;
    const std::size_t stride_;
    const DistanceKind metric_;
    AlignedVector<EmbeddingValue> data_;
    std::vector<VectorId> ids_;
    std::unordered_map<VectorId, std::size_t> positions_;
};

}  // namespace aetherindex
