#include "aether/quantization/pq.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>

#include "aether/core/parallel.hpp"
#include "aether/core/prefetch.hpp"

namespace aetherindex {
namespace {

float squared_distance(const float* a, const float* b, std::size_t length) {
    float sum = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const float d = a[i] - b[i];
        sum = std::fma(d, d, sum);
    }
    return sum;
}

double unit(std::mt19937_64& engine) {
    return static_cast<double>(engine() >> 11) * (1.0 / 9007199254740992.0);
}

std::size_t nearest(const float* point, const float* centroids, std::size_t count,
                    std::size_t length) {
    std::size_t best = 0;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t c = 0; c < count; ++c) {
        const float d = squared_distance(point, centroids + c * length, length);
        if (d < best_distance) {
            best_distance = d;
            best = c;
        }
    }
    return best;
}

// k-means on `points` (n x length, contiguous) into `out` (k x length). Deterministic
// for a seed: k-means++ seeding, Lloyd iterations, empty clusters keep their centroid.
void kmeans(const std::vector<float>& points, std::size_t n, std::size_t length, std::size_t k,
            std::size_t iterations, std::uint64_t seed, float* out) {
    std::mt19937_64 engine(seed);
    std::vector<float> closest(n, std::numeric_limits<float>::max());
    std::size_t chosen = engine() % n;
    for (std::size_t c = 0; c < k; ++c) {
        std::copy_n(points.data() + chosen * length, length, out + c * length);
        double total = 0;
        for (std::size_t i = 0; i < n; ++i) {
            closest[i] = std::min(
                closest[i], squared_distance(points.data() + i * length, out + c * length, length));
            total += closest[i];
        }
        if (c + 1 == k) break;
        if (total <= 0) {  // Fewer distinct points than centroids: reuse sequentially.
            chosen = (chosen + 1) % n;
            continue;
        }
        double target = unit(engine) * total;
        chosen = n - 1;
        for (std::size_t i = 0; i < n; ++i) {
            target -= closest[i];
            if (target <= 0) {
                chosen = i;
                break;
            }
        }
    }
    std::vector<std::size_t> assignment(n);
    std::vector<double> sums(k * length);
    std::vector<std::size_t> counts(k);
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        for (std::size_t i = 0; i < n; ++i)
            assignment[i] = nearest(points.data() + i * length, out, k, length);
        std::fill(sums.begin(), sums.end(), 0.0);
        std::fill(counts.begin(), counts.end(), 0);
        for (std::size_t i = 0; i < n; ++i) {
            ++counts[assignment[i]];
            for (std::size_t d = 0; d < length; ++d)
                sums[assignment[i] * length + d] += points[i * length + d];
        }
        for (std::size_t c = 0; c < k; ++c)
            if (counts[c])
                for (std::size_t d = 0; d < length; ++d)
                    out[c * length + d] = static_cast<float>(sums[c * length + d] / counts[c]);
    }
}

}  // namespace

std::size_t default_pq_subspaces(std::size_t dimension) noexcept {
    if (dimension % 4 == 0) return dimension / 4;
    for (std::size_t sub = 4; sub <= dimension; ++sub)
        if (dimension % sub == 0) return dimension / sub;
    return 1;
}

class PqVectors::Query final : public QuantizedQuery {
   public:
    Query(const PqVectors& owner, const PreparedQuery& query)
        : owner_(owner), table_(owner.subspaces_ * 256, 0.0f) {
        const float* q = query.data();
        for (std::size_t s = 0; s < owner.subspaces_; ++s) {
            const float* sub = q + s * owner.sub_dimension_;
            for (std::size_t c = 0; c < owner.centroids_; ++c) {
                const float* centroid =
                    owner.codebooks_.data() + (s * owner.centroids_ + c) * owner.sub_dimension_;
                float value = 0;
                if (owner.metric_ == DistanceKind::Cosine)
                    for (std::size_t d = 0; d < owner.sub_dimension_; ++d)
                        value = std::fma(sub[d], centroid[d], value);
                else
                    value = squared_distance(sub, centroid, owner.sub_dimension_);
                table_[s * 256 + c] = value;
            }
        }
    }

    float distance(std::size_t position) const noexcept override {
        const std::uint8_t* code = owner_.codes(position);
        float sum = 0;
        for (std::size_t s = 0; s < owner_.subspaces_; ++s) sum += table_[s * 256 + code[s]];
        if (owner_.metric_ == DistanceKind::Cosine) return 1.0f - std::clamp(sum, -1.0f, 1.0f);
        return sum;
    }

    void prefetch(std::size_t position) const noexcept override {
        prefetch_bytes(owner_.codes(position), owner_.subspaces_);
    }

   private:
    const PqVectors& owner_;
    std::vector<float> table_;
};

PqVectors::PqVectors(const VectorStore& store, PqConfig config)
    : dimension_(store.dimension()),
      rows_(store.size()),
      metric_(store.distance_kind()),
      subspaces_(config.subspaces ? config.subspaces : default_pq_subspaces(store.dimension())) {
    if (rows_ == 0) throw std::invalid_argument("PQ training requires a nonempty VectorStore.");
    if (!store.has_rows())
        throw std::invalid_argument("PQ training requires the FP32 rows of the VectorStore.");
    if (subspaces_ == 0 || subspaces_ > dimension_ || dimension_ % subspaces_ != 0)
        throw std::invalid_argument("PQ subspaces must divide the vector dimension.");
    if (config.iterations == 0 || config.training_sample == 0)
        throw std::invalid_argument("PQ training needs positive iterations and sample size.");
    sub_dimension_ = dimension_ / subspaces_;

    // Seeded sample of training rows (all rows when the store is small enough).
    std::vector<std::size_t> sample(rows_);
    std::iota(sample.begin(), sample.end(), std::size_t{0});
    if (rows_ > config.training_sample) {
        std::mt19937_64 engine(config.seed);
        for (std::size_t i = 0; i < config.training_sample; ++i)
            std::swap(sample[i], sample[i + engine() % (rows_ - i)]);
        sample.resize(config.training_sample);
    }
    centroids_ = std::min<std::size_t>(256, sample.size());
    codebooks_.assign(subspaces_ * centroids_ * sub_dimension_, 0.0f);
    const std::size_t threads = config.threads ? config.threads : hardware_threads();
    parallel_for(subspaces_, threads, [&](std::size_t s) {
        std::vector<float> points(sample.size() * sub_dimension_);
        for (std::size_t i = 0; i < sample.size(); ++i)
            std::copy_n(store.data_by_position(sample[i]) + s * sub_dimension_, sub_dimension_,
                        points.data() + i * sub_dimension_);
        kmeans(points, sample.size(), sub_dimension_, centroids_, config.iterations,
               config.seed + 0x9E3779B97F4A7C15ULL * (s + 1),
               codebooks_.data() + s * centroids_ * sub_dimension_);
    });
    const std::size_t rows = rows_;
    rows_ = 0;
    encode(store, rows, threads);
}

void PqVectors::extend(const VectorStore& store) {
    if (store.dimension() != dimension_ || store.distance_kind() != metric_ || !store.has_rows())
        throw std::invalid_argument("PQ extend requires the same store with FP32 rows.");
    encode(store, store.size(), 1);
}

void PqVectors::encode(const VectorStore& store, std::size_t rows, std::size_t threads) {
    const std::size_t first = rows_;
    codes_.resize(rows * subspaces_, 0);
    parallel_for(rows - first, threads, [&](std::size_t i) {
        const std::size_t p = first + i;
        const float* row = store.data_by_position(p);
        for (std::size_t s = 0; s < subspaces_; ++s)
            codes_[p * subspaces_ + s] = static_cast<std::uint8_t>(nearest(
                row + s * sub_dimension_, codebooks_.data() + s * centroids_ * sub_dimension_,
                centroids_, sub_dimension_));
    });
    rows_ = rows;
}

std::unique_ptr<QuantizedQuery> PqVectors::prepare(const PreparedQuery& query) const {
    if (query.dimension() != dimension_ || query.distance_kind() != metric_)
        throw std::invalid_argument("Prepared query does not match the PQ codes.");
    return std::make_unique<Query>(*this, query);
}

std::size_t PqVectors::memory_bytes() const noexcept {
    return codes_.capacity() + codebooks_.capacity() * sizeof(float);
}

std::vector<float> PqVectors::decode(std::size_t position) const {
    if (position >= rows_) throw std::out_of_range("PQ position is absent.");
    std::vector<float> row(dimension_);
    for (std::size_t s = 0; s < subspaces_; ++s) {
        const float* centroid =
            codebooks_.data() + (s * centroids_ + codes(position)[s]) * sub_dimension_;
        std::copy_n(centroid, sub_dimension_, row.data() + s * sub_dimension_);
    }
    return row;
}

}  // namespace aetherindex
