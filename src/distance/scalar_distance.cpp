#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "aether/distance/kernels.hpp"

#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))
#include <immintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64) && !defined(__clang__)
#include <arm64_neon.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace aetherindex::detail {
namespace {

template <bool Squared>
float accumulate(const float* a, const float* b, std::size_t length) {
    std::size_t i = 0;
    float sum;
#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))
    __m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0;
    for (; i + 32 <= length; i += 32) {
        const __m256 a0 = _mm256_loadu_ps(a + i), a1 = _mm256_loadu_ps(a + i + 8);
        const __m256 a2 = _mm256_loadu_ps(a + i + 16), a3 = _mm256_loadu_ps(a + i + 24);
        const __m256 b0 = _mm256_loadu_ps(b + i), b1 = _mm256_loadu_ps(b + i + 8);
        const __m256 b2 = _mm256_loadu_ps(b + i + 16), b3 = _mm256_loadu_ps(b + i + 24);
        if constexpr (Squared) {
            const __m256 d0 = _mm256_sub_ps(a0, b0), d1 = _mm256_sub_ps(a1, b1);
            const __m256 d2 = _mm256_sub_ps(a2, b2), d3 = _mm256_sub_ps(a3, b3);
            s0 = _mm256_fmadd_ps(d0, d0, s0);
            s1 = _mm256_fmadd_ps(d1, d1, s1);
            s2 = _mm256_fmadd_ps(d2, d2, s2);
            s3 = _mm256_fmadd_ps(d3, d3, s3);
        } else {
            s0 = _mm256_fmadd_ps(a0, b0, s0);
            s1 = _mm256_fmadd_ps(a1, b1, s1);
            s2 = _mm256_fmadd_ps(a2, b2, s2);
            s3 = _mm256_fmadd_ps(a3, b3, s3);
        }
    }
    const __m256 combined = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    __m128 reduced =
        _mm_add_ps(_mm256_castps256_ps128(combined), _mm256_extractf128_ps(combined, 1));
    reduced = _mm_hadd_ps(reduced, reduced);
    reduced = _mm_hadd_ps(reduced, reduced);
    sum = _mm_cvtss_f32(reduced);
#elif defined(__aarch64__) || defined(_M_ARM64)
    float32x4_t s0 = vdupq_n_f32(0.0f), s1 = s0, s2 = s0, s3 = s0;
    for (; i + 16 <= length; i += 16) {
        const float32x4_t a0 = vld1q_f32(a + i), a1 = vld1q_f32(a + i + 4);
        const float32x4_t a2 = vld1q_f32(a + i + 8), a3 = vld1q_f32(a + i + 12);
        const float32x4_t b0 = vld1q_f32(b + i), b1 = vld1q_f32(b + i + 4);
        const float32x4_t b2 = vld1q_f32(b + i + 8), b3 = vld1q_f32(b + i + 12);
        if constexpr (Squared) {
            const float32x4_t d0 = vsubq_f32(a0, b0), d1 = vsubq_f32(a1, b1);
            const float32x4_t d2 = vsubq_f32(a2, b2), d3 = vsubq_f32(a3, b3);
            s0 = vfmaq_f32(s0, d0, d0);
            s1 = vfmaq_f32(s1, d1, d1);
            s2 = vfmaq_f32(s2, d2, d2);
            s3 = vfmaq_f32(s3, d3, d3);
        } else {
            s0 = vfmaq_f32(s0, a0, b0);
            s1 = vfmaq_f32(s1, a1, b1);
            s2 = vfmaq_f32(s2, a2, b2);
            s3 = vfmaq_f32(s3, a3, b3);
        }
    }
    sum = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
#else
    float sums[8]{};
    for (; i + 8 <= length; i += 8) {
        for (std::size_t lane = 0; lane < 8; ++lane) {
            if constexpr (Squared) {
                const float d = a[i + lane] - b[i + lane];
                sums[lane] = std::fma(d, d, sums[lane]);
            } else {
                sums[lane] = std::fma(a[i + lane], b[i + lane], sums[lane]);
            }
        }
    }
    sum = ((sums[0] + sums[1]) + (sums[2] + sums[3])) + ((sums[4] + sums[5]) + (sums[6] + sums[7]));
#endif
    for (; i < length; ++i) {
        if constexpr (Squared) {
            const float d = a[i] - b[i];
            sum = std::fma(d, d, sum);
        } else {
            sum = std::fma(a[i], b[i], sum);
        }
    }
    return sum;
}

}  // namespace

float cosine_unit_kernel(const float* a, const float* b, std::size_t length) {
    return 1.0f - std::clamp(accumulate<false>(a, b, length), -1.0f, 1.0f);
}

float squared_euclidean_kernel(const float* a, const float* b, std::size_t length) {
    const float result = accumulate<true>(a, b, length);
    if (!std::isfinite(result)) {
        throw std::overflow_error("La distancia al cuadrado excede el rango de float.");
    }
    return result;
}

DistanceKernel distance_kernel(DistanceKind metric) {
    switch (metric) {
        case DistanceKind::Cosine:
            return cosine_unit_kernel;
        case DistanceKind::SquaredEuclidean:
            return squared_euclidean_kernel;
    }
    throw std::invalid_argument("Metrica de distancia no soportada.");
}

const char* distance_kernel_backend() noexcept {
#if defined(__AVX2__) && (defined(__FMA__) || defined(_MSC_VER))
    return "AVX2/FMA FP32";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "NEON/FMA FP32";
#else
    return "Scalar/FMA FP32";
#endif
}

}  // namespace aetherindex::detail
