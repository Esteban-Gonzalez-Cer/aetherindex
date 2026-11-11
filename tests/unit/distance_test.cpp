#include "aether/distance/distance.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/distance/kernels.hpp"

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

// KR3.5: the SIMD kernels must agree with the sequential scalar references. Both are
// FP32 sums of n terms, so each lies within n*eps*sum|term| of the exact value; the
// tolerance is twice that (plus one ulp of slack) and is checked against long double too.
void checkSimdMatchesScalar() {
    std::mt19937 random(7);
    std::uniform_real_distribution<float> coordinate(-1.0f, 1.0f);
    constexpr float eps = std::numeric_limits<float>::epsilon();
    for (std::size_t n : {1, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 100, 127, 128, 129, 384,
                          768, 1024, 1536}) {
        std::vector<float> a(n), b(n);
        for (int sample = 0; sample < 200; ++sample) {
            long double norm_a = 0.0L, norm_b = 0.0L;
            for (std::size_t i = 0; i < n; ++i) {
                a[i] = coordinate(random);
                b[i] = coordinate(random);
                norm_a += static_cast<long double>(a[i]) * a[i];
                norm_b += static_cast<long double>(b[i]) * b[i];
            }
            if (norm_a == 0.0L || norm_b == 0.0L) continue;
            long double squared = 0.0L;  // every term is nonnegative: sum|term| == sum
            for (std::size_t i = 0; i < n; ++i) {
                const long double d = static_cast<long double>(a[i]) - b[i];
                squared += d * d;
            }
            const float l2_tolerance =
                2.0f * static_cast<float>(n) * eps * static_cast<float>(squared) +
                std::numeric_limits<float>::min();
            const float l2_simd = detail::squared_euclidean_kernel(a.data(), b.data(), n);
            const float l2_scalar = detail::squared_euclidean_kernel_scalar(a.data(), b.data(), n);
            require(std::abs(l2_simd - l2_scalar) <= l2_tolerance,
                    "SIMD squared Euclidean differs from the scalar reference.");
            require(std::abs(static_cast<long double>(l2_simd) - squared) <= l2_tolerance,
                    "SIMD squared Euclidean differs from the long-double reference.");

            // Cosine kernels take unit vectors (the store/query contract).
            const float scale_a = static_cast<float>(1.0L / std::sqrt(norm_a));
            const float scale_b = static_cast<float>(1.0L / std::sqrt(norm_b));
            std::vector<float> ua(n), ub(n);
            long double dot = 0.0L, dot_magnitude = 0.0L;
            for (std::size_t i = 0; i < n; ++i) {
                ua[i] = a[i] * scale_a;
                ub[i] = b[i] * scale_b;
                dot += static_cast<long double>(ua[i]) * ub[i];
                dot_magnitude += std::abs(static_cast<long double>(ua[i]) * ub[i]);
            }
            const float cos_tolerance =
                2.0f * static_cast<float>(n) * eps * static_cast<float>(dot_magnitude) + eps;
            const float cos_simd = detail::cosine_unit_kernel(ua.data(), ub.data(), n);
            const float cos_scalar = detail::cosine_unit_kernel_scalar(ua.data(), ub.data(), n);
            const long double cos_exact = 1.0L - std::clamp(dot, -1.0L, 1.0L);
            require(std::abs(cos_simd - cos_scalar) <= cos_tolerance,
                    "SIMD cosine differs from the scalar reference.");
            require(std::abs(static_cast<long double>(cos_simd) - cos_exact) <= cos_tolerance,
                    "SIMD cosine differs from the long-double reference.");
        }
    }
    require(detail::scalar_distance_kernel(DistanceKind::Cosine) == detail::cosine_unit_kernel_scalar &&
                detail::scalar_distance_kernel(DistanceKind::SquaredEuclidean) ==
                    detail::squared_euclidean_kernel_scalar,
            "scalar_distance_kernel must select the scalar references.");
}

int main() {
    try {
        checkKnownDistances();
        checkValidation();
        checkNumericRange();
        checkPropertiesAndReference();
        checkSimdMatchesScalar();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
