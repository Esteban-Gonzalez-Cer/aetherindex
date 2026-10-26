#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>

namespace aetherindex::detail {

inline std::size_t padded_dimension(std::size_t dimension) {
    if (dimension == 0 || dimension > std::numeric_limits<std::size_t>::max() - 15) {
        throw std::invalid_argument("La dimension debe ser positiva y representable.");
    }
    return (dimension + 15) & ~std::size_t{15};
}

struct Normalization {
    float scale;
    float inverse_scaled_norm;
    float apply(float value) const noexcept { return (value / scale) * inverse_scaled_norm; }
};

// Preprocessing only: scale first to avoid overflow/underflow for finite FP32
// inputs. No double intermediate. Eight partial sums limit rounding error.
inline std::optional<Normalization> normalization(std::span<const float> values,
                                                  float minimum_norm = 0.0f) {
    float scale = 0.0f;
    for (float value : values) {
        if (!std::isfinite(value)) return std::nullopt;
        scale = std::max(scale, std::abs(value));
    }
    if (scale == 0.0f) return std::nullopt;
    std::array<float, 8> sums{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float scaled = values[i] / scale;
        sums[i % sums.size()] = std::fma(scaled, scaled, sums[i % sums.size()]);
    }
    const float squared =
        ((sums[0] + sums[1]) + (sums[2] + sums[3])) + ((sums[4] + sums[5]) + (sums[6] + sums[7]));
    const float scaled_norm = std::sqrt(squared);
    // Comparing after division avoids forming scale * scaled_norm, which could
    // overflow even though all components are finite.
    if (minimum_norm > 0.0f && scale < minimum_norm / scaled_norm) return std::nullopt;
    return Normalization{scale, 1.0f / scaled_norm};
}

}  // namespace aetherindex::detail
