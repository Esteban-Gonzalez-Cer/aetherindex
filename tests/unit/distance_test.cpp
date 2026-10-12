#include "aether/distance/distance.hpp"

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <random>

#include "../test_helpers.hpp"

using namespace aetherindex;

float referenceDistance(std::span<const float> a, std::span<const float> b, DistanceKind metric) {
    long double squared = 0.0L;
    long double dot = 0.0L;
    long double aa = 0.0L;
    long double bb = 0.0L;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const long double lhs = a[i];
        const long double rhs = b[i];
        squared += (lhs - rhs) * (lhs - rhs);
        dot += lhs * rhs;
        aa += lhs * lhs;
        bb += rhs * rhs;
    }
    return static_cast<float>(metric == DistanceKind::SquaredEuclidean
                                  ? squared
                                  : 1.0L - dot / (std::sqrt(aa) * std::sqrt(bb)));
}

void checkKnownDistances() {
    const std::array<float, 2> x = {1.0f, 0.0f};
    const std::array<float, 2> y = {0.0f, 1.0f};
    const std::array<float, 2> opposite = {-1.0f, 0.0f};
    const std::array<float, 2> scaled = {7.0f, 0.0f};
    const std::array<float, 2> zero = {0.0f, 0.0f};
    const std::array<float, 2> pythagorean = {3.0f, 4.0f};
    requireNear(compute_distance(x, x, DistanceKind::Cosine), 0.0f, "Cosine identity failed.");
    requireNear(compute_distance(x, y, DistanceKind::Cosine), 1.0f, "Cosine orthogonality failed.");
    requireNear(compute_distance(x, opposite, DistanceKind::Cosine), 2.0f,
                "Opposite cosine directions must have distance two.");
    requireNear(compute_distance(x, scaled, DistanceKind::Cosine), 0.0f,
                "Cosine must be independent of positive magnitude scaling.");
    requireNear(compute_distance(zero, pythagorean, DistanceKind::SquaredEuclidean), 25.0f,
                "Euclidean distance must be squared, without a root.");
    requireNear(compute_distance(zero, zero, DistanceKind::SquaredEuclidean), 0.0f,
                "Squared distance permits zero vectors.");
    const std::array<float, 2> negative = {-1.0f, 0.5f};
    const std::array<float, 2> positive = {1.0f, -0.5f};
    requireNear(compute_distance(negative, positive, DistanceKind::SquaredEuclidean), 5.0f,
                "Negative and fractional coordinates must work.");
}

void checkValidation() {
    const std::array<float, 2> valid = {1.0f, 0.0f};
    const std::array<float, 1> shorter = {1.0f};
    const std::array<float, 2> zero = {-0.0f, 0.0f};
    for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        requireInvalidArgument([&] { compute_distance(valid, shorter, metric); },
                               "Unequal dimensions must be rejected.");
        requireInvalidArgument([&] { compute_distance({}, {}, metric); },
                               "Empty dimensions must be rejected.");
        requireInvalidArgument([&] { compute_distance({}, valid, metric); },
                               "Empty lhs must be rejected.");
        for (float invalid :
             {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()}) {
            const std::array<float, 2> values = {invalid, 1.0f};
            requireInvalidArgument([&] { compute_distance(values, valid, metric); },
                                   "Nonfinite lhs must be rejected.");
            requireInvalidArgument([&] { compute_distance(valid, values, metric); },
                                   "Nonfinite rhs must be rejected.");
        }
    }
    requireInvalidArgument([&] { compute_distance(zero, valid, DistanceKind::Cosine); },
                           "Cosine must reject a zero lhs norm.");
    requireInvalidArgument([&] { compute_distance(valid, zero, DistanceKind::Cosine); },
                           "Cosine must reject a zero rhs norm.");
    requireInvalidArgument([&] { compute_distance(valid, valid, static_cast<DistanceKind>(42)); },
                           "Unsupported metrics must be rejected.");
}

void checkNumericRange() {
    const float maximum = std::numeric_limits<float>::max();
    const float tiny = std::numeric_limits<float>::denorm_min();
    const std::array<float, 2> huge = {maximum, maximum};
    const std::array<float, 2> opposite = {-maximum, -maximum};
    const std::array<float, 2> subnormal = {tiny, tiny};
    requireNear(compute_distance(huge, huge, DistanceKind::Cosine), 0.0f,
                "Cosine must not overflow when finite inputs are very large.");
    requireNear(compute_distance(huge, opposite, DistanceKind::Cosine), 2.0f,
                "Extreme opposite inputs must produce finite cosine distance two.");
    requireNear(compute_distance(subnormal, subnormal, DistanceKind::Cosine), 0.0f,
                "Tiny nonzero norms must not underflow to zero during accumulation.");
    requireNear(compute_distance(huge, huge, DistanceKind::SquaredEuclidean), 0.0f,
                "Large identical coordinates must have zero squared distance.");
    requireOverflow([&] { compute_distance(huge, opposite, DistanceKind::SquaredEuclidean); },
                    "Unrepresentable squared distances must throw overflow_error.");
    const std::array<float, 1> large = {1.0e19f};
    const std::array<float, 1> zero = {0.0f};
    const float expected = referenceDistance(large, zero, DistanceKind::SquaredEuclidean);
    const float actual = compute_distance(large, zero, DistanceKind::SquaredEuclidean);
    require(std::isfinite(actual) && std::abs(actual - expected) <= expected * 1.0e-6f,
            "Representable large squared distances must remain finite and accurate.");
}

void checkPropertiesAndReference() {
    std::mt19937 random(42);
    for (int sample = 0; sample < 64; ++sample) {
        std::array<float, 16> a;
        std::array<float, 16> b;
        for (std::size_t i = 0; i < a.size(); ++i) {
            a[i] = static_cast<float>(static_cast<int>(random() % 2001) - 1000) / 1000.0f;
            b[i] = static_cast<float>(static_cast<int>(random() % 2001) - 1000) / 1000.0f;
        }
        const auto original_a = a;
        const auto original_b = b;
        for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
            const float actual = compute_distance(a, b, metric);
            requireNear(actual, referenceDistance(a, b, metric),
                        "Scalar calculation differs from the independent long-double reference.");
            requireNear(actual, compute_distance(b, a, metric), "Distance symmetry failed.");
            requireNear(compute_distance(a, a, metric), 0.0f, "Self distance must be zero.");
            require(actual >= 0.0f, "Distances must not be negative.");
            if (metric == DistanceKind::Cosine) {
                require(actual <= 2.0f, "Cosine distance must stay within [0, 2].");
            }
        }
        require(a == original_a && b == original_b, "Distance calculation modified the inputs.");
    }
}

int main() {
    try {
        checkKnownDistances();
        checkValidation();
        checkNumericRange();
        checkPropertiesAndReference();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
