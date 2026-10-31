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
    return vaddvq_f32(vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3)));
#else
    return weighted_code_sum_scalar(weights, codes, length);
#endif
}

}  // namespace detail

namespace {
constexpr std::size_t round16(std::size_t value) { return (value + 15) / 16 * 16; }
}  // namespace

// Per-query weights over the padded code width; padding weights are zero.
class Sq8Vectors::Query final : public QuantizedQuery {
   public:
    Query(const Sq8Vectors& owner, const PreparedQuery& query)
        : owner_(owner), weights_(owner.stride_, 0.0f) {
        const float* q = query.data();
        double constant = 0;
        for (std::size_t d = 0; d < owner.dimension_; ++d) {
            if (owner.metric_ == DistanceKind::Cosine) {
                weights_[d] = q[d] * owner.scale_[d];
                constant += static_cast<double>(q[d]) * owner.min_[d];
            } else {
                const double a = static_cast<double>(q[d]) - owner.min_[d];
                weights_[d] = static_cast<float>(-2.0 * a * owner.scale_[d]);
                constant += a * a;
            }
        }
        constant_ = static_cast<float>(constant);
    }

    float distance(std::size_t position) const noexcept override {
        const float sum = detail::weighted_code_sum(
            weights_.data(), owner_.codes_.data() + position * owner_.stride_, owner_.stride_);
        if (owner_.metric_ == DistanceKind::Cosine)
            return 1.0f - std::clamp(constant_ + sum, -1.0f, 1.0f);
        return std::max(0.0f, constant_ + sum + owner_.row_terms_[position]);
    }

    void prefetch(std::size_t position) const noexcept override {
        prefetch_bytes(owner_.codes_.data() + position * owner_.stride_, owner_.stride_);
        if (owner_.metric_ != DistanceKind::Cosine) prefetch_read(&owner_.row_terms_[position]);
    }

   private:
    const Sq8Vectors& owner_;
    AlignedVector<float> weights_;
    float constant_ = 0;
};

Sq8Vectors::Sq8Vectors(const VectorStore& store)
    : dimension_(store.dimension()),
      stride_(round16(store.dimension())),
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
    codes_.assign(rows_ * stride_, 0);
    if (metric_ == DistanceKind::SquaredEuclidean) row_terms_.assign(rows_, 0.0f);
    for (std::size_t p = 0; p < rows_; ++p) {
        const float* row = store.data_by_position(p);
        std::uint8_t* code = codes_.data() + p * stride_;
        double term = 0;
        for (std::size_t d = 0; d < dimension_; ++d) {
            if (scale_[d] > 0) {
                const double level =
                    std::round((static_cast<double>(row[d]) - min_[d]) / scale_[d]);
                code[d] = static_cast<std::uint8_t>(std::clamp(level, 0.0, 255.0));
            }
            const double reconstructed = static_cast<double>(scale_[d]) * code[d];
            term += reconstructed * reconstructed;
        }
        if (!row_terms_.empty()) row_terms_[p] = static_cast<float>(term);
    }
}

std::unique_ptr<QuantizedQuery> Sq8Vectors::prepare(const PreparedQuery& query) const {
    if (query.dimension() != dimension_ || query.distance_kind() != metric_)
        throw std::invalid_argument("Prepared query does not match the SQ8 codes.");
    return std::make_unique<Query>(*this, query);
}

std::size_t Sq8Vectors::memory_bytes() const noexcept {
    return codes_.capacity() +
           (min_.capacity() + scale_.capacity() + row_terms_.capacity()) * sizeof(float);
}

std::vector<float> Sq8Vectors::decode(std::size_t position) const {
    if (position >= rows_) throw std::out_of_range("SQ8 position is absent.");
    std::vector<float> row(dimension_);
    const std::uint8_t* code = codes_.data() + position * stride_;
    for (std::size_t d = 0; d < dimension_; ++d) row[d] = min_[d] + scale_[d] * code[d];
    return row;
}

}  // namespace aetherindex
