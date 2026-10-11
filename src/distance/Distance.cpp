
#include "distance/Distance.hpp"

#include <stdexcept>

namespace aetherindex {

float squaredEuclideanDistance(
    const std::vector<float>& a,
    const std::vector<float>& b
) {
    if (a.size() != b.size()) {
        throw std::invalid_argument(
            "Los vectores tienen dimensiones diferentes."
        );
    }

    float distance = 0.0f;

    for (std::size_t i = 0; i < a.size(); ++i) {
        float difference = a[i] - b[i];
        distance += difference * difference;
    }

    return distance;
}

} // namespace aetherindex
