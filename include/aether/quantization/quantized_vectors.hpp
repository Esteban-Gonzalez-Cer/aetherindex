#pragma once

#include <cstddef>
#include <memory>

#include "aether/core/prepared_query.hpp"
#include "aether/core/types.hpp"

namespace aetherindex {

// Distances from one prepared query to compressed rows. Positions are the dense
// VectorStore positions the codes were built from. Smaller is better, same metric
// convention as the FP32 kernels (cosine: 1 - similarity; squared Euclidean).
class QuantizedQuery {
   public:
    virtual ~QuantizedQuery() = default;
    virtual float distance(std::size_t position) const noexcept = 0;
    // Hints the memory distance(position) will read, so HNSW can overlap cache misses.
    virtual void prefetch(std::size_t /*position*/) const noexcept {}
};

// Compressed copy of every row of a VectorStore (SQ8, PQ, ...), used by HNSW to
// traverse with approximate distances before an optional exact FP32 re-rank.
class QuantizedVectors {
   public:
    virtual ~QuantizedVectors() = default;
    // The query must match the store's dimension and metric (PreparedQuery is already
    // normalized for cosine). The returned object is independent of other queries.
    virtual std::unique_ptr<QuantizedQuery> prepare(const PreparedQuery& query) const = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual std::size_t dimension() const noexcept = 0;
    virtual DistanceKind metric() const noexcept = 0;
    // Bytes held for codes plus per-row and codebook data (excludes FP32 rows).
    virtual std::size_t memory_bytes() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

}  // namespace aetherindex
