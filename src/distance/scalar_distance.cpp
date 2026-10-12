#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

#include "aether/distance/distance.hpp"

namespace aetherindex {

float compute_distance(std::span<const EmbeddingValue> a, std::span<const EmbeddingValue> b,
                       DistanceKind metric) {
    if (metric != DistanceKind::Cosine && metric != DistanceKind::SquaredEuclidean) {
        throw std::invalid_argument("Metrica de distancia no soportada.");
    }
    if (a.empty() || a.size() != b.size()) {
        throw std::invalid_argument("Los vectores deben tener la misma dimension positiva.");
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) {
            throw std::invalid_argument("Los componentes deben ser finitos.");
        }
    }

    if (metric == DistanceKind::SquaredEuclidean) {
        double distance = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const double difference = static_cast<double>(a[i]) - static_cast<double>(b[i]);
            distance += difference * difference;
        }
        if (distance > std::numeric_limits<float>::max()) {
            throw std::overflow_error("La distancia al cuadrado excede el rango de float.");
        }
        return static_cast<float>(distance);
    }

    double dot_product = 0.0;
    double norm_a_squared = 0.0;
    double norm_b_squared = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double lhs = a[i];
        const double rhs = b[i];
        dot_product += lhs * rhs;
        norm_a_squared += lhs * lhs;
        norm_b_squared += rhs * rhs;
    }
    if (norm_a_squared == 0.0 || norm_b_squared == 0.0) {
        throw std::invalid_argument("La distancia coseno requiere vectores de norma no nula.");
    }
    const double similarity = dot_product / (std::sqrt(norm_a_squared) * std::sqrt(norm_b_squared));
    return static_cast<float>(1.0 - std::clamp(similarity, -1.0, 1.0));
}

}  // namespace aetherindex
