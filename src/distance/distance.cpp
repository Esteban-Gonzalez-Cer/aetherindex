#include "aether/distance/distance.hpp"

#include <stdexcept>

#include "aether/core/prepared_query.hpp"
#include "aether/distance/kernels.hpp"

namespace aetherindex {

float compute_distance(std::span<const EmbeddingValue> a, std::span<const EmbeddingValue> b,
                       DistanceKind metric) {
    if (a.empty() || a.size() != b.size()) {
        throw std::invalid_argument("Los vectores deben tener la misma dimension positiva.");
    }
    // General raw-vector API. Search uses the prepared kernels directly and
    // does not pay for validation, alignment or normalization per candidate.
    const PreparedQuery lhs(a, a.size(), metric);
    const PreparedQuery rhs(b, a.size(), metric);
    return detail::distance_kernel(metric)(lhs.data(), rhs.data(), lhs.padded_dimension());
}

}  // namespace aetherindex
