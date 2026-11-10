#include "aether/quantization/sq8.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "aether/core/prefetch.hpp"

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace aetherindex {
namespace detail {

float weighted_code_sum_scalar(const float* weights, const std::uint8_t* codes,
                               std::size_t length) {
    float lanes[4]{};
    for (std::size_t i = 0; i < length; ++i)
        lanes[i % 4] = std::fma(weights[i], static_cast<float>(codes[i]), lanes[i % 4]);
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}

float weighted_code_sum(const float* weights, const std::uint8_t* codes, std::size_t length) {
#if defined(__aarch64__) || defined(_M_ARM64)
    float32x4_t s0 = vdupq_n_f32(0.0f), s1 = s0, s2 = s0, s3 = s0;
    for (std::size_t i = 0; i + 16 <= length; i += 16) {
        const uint8x16_t bytes = vld1q_u8(codes + i);
        const uint16x8_t low = vmovl_u8(vget_low_u8(bytes));
        const uint16x8_t high = vmovl_u8(vget_high_u8(bytes));
        s0 = vfmaq_f32(s0, vld1q_f32(weights + i), vcvtq_f32_u32(vmovl_u16(vget_low_u16(low))));
        s1 =
            vfmaq_f32(s1, vld1q_f32(weights + i + 4), vcvtq_f32_u32(vmovl_u16(vget_high_u16(low))));
        s2 =
            vfmaq_f32(s2, vld1q_f32(weights + i + 8), vcvtq_f32_u32(vmovl_u16(vget_low_u16(high))));
        s3 = vfmaq_f32(s3, vld1q_f32(weights + i + 12),
                       vcvtq_f32_u32(vmovl_u16(vget_high_u16(high))));
    }
    float sum = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
    for (std::size_t i = length / 16 * 16; i < length; ++i)
        sum = std::fma(weights[i], static_cast<float>(codes[i]), sum);
    return sum;
#else
    return weighted_code_sum_scalar(weights, codes, length);
#endif
}

float squared_code_distance_scalar(const float* scale, const float* offset,
                                   const std::uint8_t* codes, std::size_t length) {
    float lanes[4]{};
    for (std::size_t i = 0; i < length; ++i) {
        const float difference = std::fma(scale[i], static_cast<float>(codes[i]), -offset[i]);
        lanes[i % 4] = std::fma(difference, difference, lanes[i % 4]);
    }
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
}

float squared_code_distance(const float* scale, const float* offset, const std::uint8_t* codes,
                            std::size_t length) {
#if defined(__aarch64__) || defined(_M_ARM64)
    float32x4_t s0 = vdupq_n_f32(0.0f), s1 = s0, s2 = s0, s3 = s0;
    const auto step = [&](float32x4_t& sum, std::size_t i, uint16x4_t values) {
        // d = scale * c - offset, then sum += d * d.
        const float32x4_t d =
            vfmaq_f32(vnegq_f32(vld1q_f32(offset + i)), vld1q_f32(scale + i),
                      vcvtq_f32_u32(vmovl_u16(values)));
        sum = vfmaq_f32(sum, d, d);
    };
    std::size_t i = 0;
    for (; i + 16 <= length; i += 16) {
        const uint8x16_t bytes = vld1q_u8(codes + i);
        const uint16x8_t low = vmovl_u8(vget_low_u8(bytes));
        const uint16x8_t high = vmovl_u8(vget_high_u8(bytes));
        step(s0, i, vget_low_u16(low));
        step(s1, i + 4, vget_high_u16(low));
        step(s2, i + 8, vget_low_u16(high));
        step(s3, i + 12, vget_high_u16(high));
    }
    float sum = vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
    for (; i < length; ++i) {
        const float difference = std::fma(scale[i], static_cast<float>(codes[i]), -offset[i]);
        sum = std::fma(difference, difference, sum);
    }
    return sum;
#else
    return squared_code_distance_scalar(scale, offset, codes, length);
#endif
}

}  // namespace detail

// Cosine: per-query weights q_d * scale_d and constant sum_d q_d * min_d, so the dot
// product with a dequantized row is constant + sum_d w_d * c_d. Squared Euclidean: the
// query offset q_d - min_d, and the distance sum_d (scale_d * c_d - offset_d)^2.
class Sq8Vectors::Query final : public QuantizedQuery {
   public:
    Query(const Sq8Vectors& owner, const PreparedQuery& query)
        : owner_(owner), weights_(owner.dimension_, 0.0f) {
        const float* q = query.data();
        double constant = 0;
        for (std::size_t d = 0; d < owner.dimension_; ++d) {
            if (owner.metric_ == DistanceKind::Cosine) {
                weights_[d] = q[d] * owner.scale_[d];
                constant += static_cast<double>(q[d]) * owner.min_[d];
            } else {
                weights_[d] = q[d] - owner.min_[d];
            }
        }
        constant_ = static_cast<float>(constant);
    }

    float distance(std::size_t position) const noexcept override {
        const std::uint8_t* code = owner_.codes_.data() + position * owner_.stride_;
        if (owner_.metric_ == DistanceKind::Cosine)
            return 1.0f - std::clamp(constant_ + detail::weighted_code_sum(weights_.data(), code,
                                                                            owner_.dimension_),
                                     -1.0f, 1.0f);
        return detail::squared_code_distance(owner_.scale_.data(), weights_.data(), code,
                                             owner_.dimension_);
    }

    void prefetch(std::size_t position) const noexcept override {
        prefetch_bytes(owner_.codes_.data() + position * owner_.stride_, owner_.stride_);
    }

   private:
    const Sq8Vectors& owner_;
    AlignedVector<float> weights_;
    float constant_ = 0;
};

Sq8Vectors::Sq8Vectors(const VectorStore& store)
    : dimension_(store.dimension()),
      stride_(store.dimension()),
      rows_(store.size()),
      metric_(store.distance_kind()),
      min_(store.dimension(), std::numeric_limits<float>::max()),
      scale_(store.dimension(), 0.0f) {
    if (rows_ == 0) throw std::invalid_argument("SQ8 training requires a nonempty VectorStore.");
    if (!store.has_rows())
        throw std::invalid_argument("SQ8 training requires the FP32 rows of the VectorStore.");
    std::vector<float> max(dimension_, std::numeric_limits<float>::lowest());
    for (std::size_t p = 0; p < rows_; ++p) {
        const float* row = store.data_by_position(p);
        for (std::size_t d = 0; d < dimension_; ++d) {
            min_[d] = std::min(min_[d], row[d]);
            max[d] = std::max(max[d], row[d]);
        }
    }
    for (std::size_t d = 0; d < dimension_; ++d) {
        const double range = static_cast<double>(max[d]) - min_[d];
        scale_[d] = static_cast<float>(range / 255.0);  // Zero for a constant dimension.
    }
    const std::size_t rows = rows_;
    rows_ = 0;
    encode(store, rows);
}

void Sq8Vectors::extend(const VectorStore& store) {
    if (store.dimension() != dimension_ || store.distance_kind() != metric_ || !store.has_rows())
        throw std::invalid_argument("SQ8 extend requires the same store with FP32 rows.");
    encode(store, store.size());
}

void Sq8Vectors::encode(const VectorStore& store, std::size_t rows) {
    const std::size_t first = rows_;
    codes_.resize(rows * stride_, 0);
    for (std::size_t p = first; p < rows; ++p) {
        const float* row = store.data_by_position(p);
        std::uint8_t* code = codes_.data() + p * stride_;
        for (std::size_t d = 0; d < dimension_; ++d) {
            if (scale_[d] > 0) {
                const double level =
                    std::round((static_cast<double>(row[d]) - min_[d]) / scale_[d]);
                code[d] = static_cast<std::uint8_t>(std::clamp(level, 0.0, 255.0));
            }
        }
    }
    rows_ = rows;
}

std::unique_ptr<QuantizedQuery> Sq8Vectors::prepare(const PreparedQuery& query) const {
    if (query.dimension() != dimension_ || query.distance_kind() != metric_)
        throw std::invalid_argument("Prepared query does not match the SQ8 codes.");
    return std::make_unique<Query>(*this, query);
}

std::size_t Sq8Vectors::memory_bytes() const noexcept {
    return codes_.capacity() + (min_.capacity() + scale_.capacity()) * sizeof(float);
}

std::vector<float> Sq8Vectors::decode(std::size_t position) const {
    if (position >= rows_) throw std::out_of_range("SQ8 position is absent.");
    std::vector<float> row(dimension_);
    const std::uint8_t* code = codes_.data() + position * stride_;
    for (std::size_t d = 0; d < dimension_; ++d) row[d] = min_[d] + scale_[d] * code[d];
    return row;
}

}  // namespace aetherindex
