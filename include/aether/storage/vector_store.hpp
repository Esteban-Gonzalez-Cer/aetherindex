#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "aether/core/types.hpp"

namespace aetherindex {

enum class InsertStatus { Success, DuplicateId, DimensionMismatch, InvalidEmbedding };

// Sole owner of the embeddings. Reads and writes must not overlap.
// Discard all views from find()/ids() before calling insert(), even if it fails.
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

    // Rejections leave contents unchanged. Allocation/length exceptions propagate
    // with the same logical contents; capacities and view validity may change.
    InsertStatus insert(VectorId id, std::span<const EmbeddingValue> embedding);

    std::optional<std::span<const EmbeddingValue>> find(VectorId id) const;
    std::size_t size() const noexcept;
    std::size_t dimension() const noexcept;
    DistanceKind distance_kind() const noexcept;
    bool contains(VectorId id) const;
    std::span<const VectorId> ids() const noexcept;

   private:
    const std::size_t dimension_;
    const DistanceKind metric_;
    std::vector<EmbeddingValue> data_;
    std::vector<VectorId> ids_;
    std::unordered_map<VectorId, std::size_t> positions_;
};

}  // namespace aetherindex
