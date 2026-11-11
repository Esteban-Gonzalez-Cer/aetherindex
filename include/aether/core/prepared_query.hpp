#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>

#include "aether/core/aligned_allocator.hpp"
#include "aether/core/types.hpp"
#include "aether/core/vector_math.hpp"

namespace aetherindex {

// An owning, immutable query: finite FP32, 64-byte aligned, zero padded, and
// unit-normalized for cosine. Construct once at the application boundary.
class PreparedQuery {
   public:
    PreparedQuery(std::span<const float> input, std::size_t dimension, DistanceKind metric)
        : dimension_(dimension), metric_(metric) {
        if (input.size() != dimension || input.empty()) {
            throw std::invalid_argument("La consulta debe tener la dimension del VectorStore.");
        }
        if (metric != DistanceKind::Cosine && metric != DistanceKind::SquaredEuclidean) {
            throw std::invalid_argument("Metrica de distancia no soportada.");
        }
        if (metric == DistanceKind::Cosine) {
            const auto norm = detail::normalization(input);
            if (!norm) throw std::invalid_argument("La consulta coseno debe ser finita y no nula.");
            data_.resize(detail::padded_dimension(dimension), 0.0f);
            for (std::size_t i = 0; i < dimension; ++i) data_[i] = norm->apply(input[i]);
        } else {
            for (float value : input) {
                if (!std::isfinite(value))
                    throw std::invalid_argument("La consulta debe ser finita.");
            }
            data_.resize(detail::padded_dimension(dimension), 0.0f);
            std::copy(input.begin(), input.end(), data_.begin());
        }
    }

    // Keep the prepared invariant and view lifetimes explicit. In particular,
    // a moved-from buffer must never reach a kernel with nonzero dimensions.
    PreparedQuery(const PreparedQuery&) = delete;
    PreparedQuery& operator=(const PreparedQuery&) = delete;
    PreparedQuery(PreparedQuery&&) = delete;
    PreparedQuery& operator=(PreparedQuery&&) = delete;

    std::size_t dimension() const noexcept { return dimension_; }
    std::size_t padded_dimension() const noexcept { return data_.size(); }
    DistanceKind distance_kind() const noexcept { return metric_; }
    const float* data() const noexcept { return data_.data(); }
    std::span<const float> values() const noexcept { return {data_.data(), dimension_}; }

   private:
    std::size_t dimension_;
    DistanceKind metric_;
    AlignedVector<float> data_;
};

}  // namespace aetherindex
