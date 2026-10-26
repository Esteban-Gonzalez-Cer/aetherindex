#include "aether/storage/vector_store.hpp"

#include <cmath>
#include <stdexcept>

#include "aether/core/vector_math.hpp"

namespace aetherindex {

VectorStore::VectorStore(std::size_t dimension, DistanceKind metric)
    : dimension_(dimension), stride_(detail::padded_dimension(dimension)), metric_(metric) {
    if (dimension == 0) {
        throw std::invalid_argument("La dimension debe ser mayor que cero.");
    }
    if (metric != DistanceKind::Cosine && metric != DistanceKind::SquaredEuclidean) {
        throw std::invalid_argument("Metrica de distancia no soportada.");
    }
}

InsertStatus VectorStore::insert(VectorId id, std::span<const EmbeddingValue> embedding) {
    if (embedding.size() != dimension_) {
        return InsertStatus::DimensionMismatch;
    }
    if (contains(id)) {
        return InsertStatus::DuplicateId;
    }

    std::optional<detail::Normalization> norm;
    if (metric_ == DistanceKind::Cosine) {
        norm = detail::normalization(embedding, 1e-7f);
        if (!norm) return InsertStatus::InvalidEmbedding;
    } else {
        for (float value : embedding) {
            if (!std::isfinite(value)) return InsertStatus::InvalidEmbedding;
        }
    }

    // Stage first: embedding may refer to data_ which insertion can reallocate.
    AlignedVector<float> pending(stride_, 0.0f);
    for (std::size_t i = 0; i < dimension_; ++i) {
        pending[i] = norm ? norm->apply(embedding[i]) : embedding[i];
    }
    const std::size_t old_data_size = data_.size();
    const std::size_t old_size = ids_.size();
    try {
        data_.insert(data_.end(), pending.begin(), pending.end());
        ids_.push_back(id);
        // Commit last. A throwing emplace does not add a map entry; no throwing
        // operation follows a successful emplace. Shrinking float/ID vectors
        // in the rollback below does not allocate or throw.
        positions_.emplace(id, old_size);
    } catch (...) {
        data_.resize(old_data_size);
        ids_.resize(old_size);
        throw;
    }
    return InsertStatus::Success;
}

std::optional<std::span<const EmbeddingValue>> VectorStore::find(VectorId id) const {
    const auto position = positions_.find(id);
    if (position == positions_.end()) {
        return std::nullopt;
    }
    return std::span<const EmbeddingValue>(data_).subspan(position->second * stride_, dimension_);
}

std::size_t VectorStore::size() const noexcept { return ids_.size(); }

std::size_t VectorStore::dimension() const noexcept { return dimension_; }

DistanceKind VectorStore::distance_kind() const noexcept { return metric_; }

bool VectorStore::contains(VectorId id) const { return positions_.contains(id); }

std::span<const VectorId> VectorStore::ids() const noexcept { return ids_; }

std::size_t VectorStore::padded_dimension() const noexcept { return stride_; }

std::optional<std::size_t> VectorStore::position_of(VectorId id) const {
    const auto it = positions_.find(id);
    return it == positions_.end() ? std::nullopt : std::optional<std::size_t>(it->second);
}

const float* VectorStore::data_by_position(std::size_t position) const noexcept {
    return data_.data() + position * stride_;
}

void VectorStore::reserve(std::size_t count) {
    if (count > data_.max_size() / stride_)
        throw std::length_error("VectorStore capacity overflow.");
    data_.reserve(count * stride_);
    ids_.reserve(count);
    positions_.reserve(count);
}

}  // namespace aetherindex
