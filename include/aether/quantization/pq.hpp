#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "aether/quantization/quantized_vectors.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

struct PqConfig {
    // Subspaces m (must divide the dimension). 0 = automatic: 4 dimensions per subspace
    // when divisible, otherwise the largest divisor of the dimension giving >= 4.
    std::size_t subspaces = 0;
    std::size_t training_sample = 16384;  // Rows sampled (seeded) for k-means.
    std::size_t iterations = 12;          // Lloyd iterations per subspace.
    std::uint64_t seed = 42;
    std::size_t threads = 0;  // 0 = hardware threads. Results do not depend on it.
};

// Product quantization: each row is split into m subspaces and every sub-vector is
// replaced by the index of its nearest of (up to) 256 k-means centroids, one byte per
// subspace. Training is deterministic for a seed: each subspace has its own generator
// (k-means++ initialization, then Lloyd iterations) and runs independently, so the
// thread count never changes the codebooks. Distances are asymmetric (ADC): per query a
// m x 256 table holds sub-distances (squared Euclidean) or sub-dot-products (cosine on
// the stored unit rows), and a row's distance is the sum of m table lookups.
class PqVectors final : public QuantizedVectors {
   public:
    // Throws invalid_argument for an empty store, released rows or an invalid m.
    explicit PqVectors(const VectorStore& store, PqConfig config = {});

    std::unique_ptr<QuantizedQuery> prepare(const PreparedQuery& query) const override;
    std::size_t size() const noexcept override { return rows_; }
    std::size_t dimension() const noexcept override { return dimension_; }
    DistanceKind metric() const noexcept override { return metric_; }
    std::size_t memory_bytes() const noexcept override;
    const char* name() const noexcept override { return "PQ"; }

    std::size_t subspaces() const noexcept { return subspaces_; }
    std::size_t centroids() const noexcept { return centroids_; }
    // Reconstructed row (concatenated centroids), for tests and diagnostics.
    std::vector<float> decode(std::size_t position) const;
    const std::uint8_t* codes(std::size_t position) const noexcept {
        return codes_.data() + position * subspaces_;
    }

   private:
    class Query;
    std::size_t dimension_;
    std::size_t rows_;
    DistanceKind metric_;
    std::size_t subspaces_;
    std::size_t sub_dimension_;
    std::size_t centroids_;
    std::vector<float> codebooks_;     // [subspace][centroid][sub_dimension]
    std::vector<std::uint8_t> codes_;  // [row][subspace]
};

// Automatic subspace count for a dimension (see PqConfig::subspaces).
std::size_t default_pq_subspaces(std::size_t dimension) noexcept;

}  // namespace aetherindex
