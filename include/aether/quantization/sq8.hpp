#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "aether/core/aligned_allocator.hpp"
#include "aether/quantization/quantized_vectors.hpp"
#include "aether/storage/vector_store.hpp"

namespace aetherindex {

// SQ8 scalar quantization with per-dimension ranges, trained on every stored row
// (cosine rows are already unit vectors). Row x is coded as c_d = round((x_d - min_d) /
// scale_d), scale_d = (max_d - min_d) / 255, so each component is reconstructed within
// scale_d / 2. Distances are asymmetric: the FP32 query is never quantized, and both
// metrics reduce to constant + sum_d w_d * c_d (+ a per-row term for squared Euclidean).
// Code rows are exactly `dimension` bytes (no padding). Squared Euclidean distances are
// computed directly on the dequantized row, so no per-row data is stored besides the codes.
class Sq8Vectors final : public QuantizedVectors {
   public:
    // Throws invalid_argument for an empty store or one whose rows were released.
    explicit Sq8Vectors(const VectorStore& store);

    std::unique_ptr<QuantizedQuery> prepare(const PreparedQuery& query) const override;
    std::size_t size() const noexcept override { return rows_; }
    std::size_t dimension() const noexcept override { return dimension_; }
    DistanceKind metric() const noexcept override { return metric_; }
    std::size_t memory_bytes() const noexcept override;
    void extend(const VectorStore& store) override;
    const char* name() const noexcept override { return "SQ8"; }

    // Reconstructed row, for tests and diagnostics.
    std::vector<float> decode(std::size_t position) const;
    float scale(std::size_t dimension) const { return scale_.at(dimension); }

   private:
    friend struct SnapshotAccess;  // Index snapshots (aether_persistence).
    Sq8Vectors() = default;  // Filled by SnapshotAccess.
    void encode(const VectorStore& store, std::size_t rows);  // Rows [rows_, rows).
    class Query;
    std::size_t dimension_;
    std::size_t stride_;
    std::size_t rows_;
    DistanceKind metric_;
    std::vector<float> min_;
    std::vector<float> scale_;
    AlignedVector<std::uint8_t> codes_;

};

namespace detail {
// sum_i weights[i] * codes[i] (NEON where available, any length).
float weighted_code_sum(const float* weights, const std::uint8_t* codes, std::size_t length);
// sum_i (scale[i] * codes[i] - offset[i])^2 (NEON where available, any length).
float squared_code_distance(const float* scale, const float* offset, const std::uint8_t* codes,
                            std::size_t length);
float squared_code_distance_scalar(const float* scale, const float* offset,
                                   const std::uint8_t* codes, std::size_t length);
// Portable reference used to verify the vector kernel.
float weighted_code_sum_scalar(const float* weights, const std::uint8_t* codes, std::size_t length);
}  // namespace detail

}  // namespace aetherindex
