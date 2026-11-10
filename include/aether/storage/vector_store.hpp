#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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
    std::span<const VectorId> ids() const noexcept { return ids_; }

    // Logical dimensions are unchanged. Physical rows have zero padding and
    // start on 64-byte boundaries. Positions are dense, in insertion order.
    std::size_t padded_dimension() const noexcept { return stride_; }
    std::optional<std::size_t> position_of(VectorId id) const;
    // Precondition: position < size(). Read-only hot path, no hash lookup.
    const float* data_by_position(std::size_t position) const noexcept {
        return data_.data() + position * stride_;
    }
    void reserve(std::size_t count);

    // Frees the FP32 rows while keeping IDs, positions, dimension and metric, so a
    // quantized copy can serve searches with less memory. Afterwards insert(), reserve()
    // and find() throw logic_error, and data_by_position() must not be called; callers
    // check has_rows() first. Irreversible.
    void release_rows();
    bool has_rows() const noexcept { return !rows_released_; }

    // Deletes: erase() unmaps the ID (contains/find/position_of no longer see it) and
    // tombstones its position. Positions never move, so indexes built on the store stay
    // valid; they skip tombstoned positions in results (rows remain readable for graph
    // routing). The same ID may be inserted again; it gets a new position. Returns false
    // when the ID is absent.
    bool erase(VectorId id);
    bool is_deleted(std::size_t position) const noexcept {
        return deleted_count_ != 0 && position / 64 < deleted_.size() &&
               ((deleted_[position / 64] >> (position % 64)) & 1u) != 0;
    }
    std::size_t deleted_count() const noexcept { return deleted_count_; }
    std::size_t live_size() const noexcept { return ids_.size() - deleted_count_; }
    // Appends the row at `position` of `source` (same dimension and metric) under `id`,
    // copied bit for bit: it was already normalized when `source` stored it.
    InsertStatus insert_from(const VectorStore& source, std::size_t position, VectorId id);
    // A new store with only the live rows, in position order, copied bit for bit (no
    // re-normalization), so it equals a store that never held the deleted rows.
    std::unique_ptr<VectorStore> compacted() const;

   private:
    friend struct SnapshotAccess;  // Index snapshots (aether_persistence).
    const std::size_t dimension_;
    const std::size_t stride_;
    const DistanceKind metric_;
    AlignedVector<EmbeddingValue> data_;
    std::vector<VectorId> ids_;
    std::unordered_map<VectorId, std::size_t> positions_;
    std::vector<std::uint64_t> deleted_;  // Tombstone bit per position (grown on demand).
    std::size_t deleted_count_ = 0;
    bool rows_released_ = false;
};

}  // namespace aetherindex
