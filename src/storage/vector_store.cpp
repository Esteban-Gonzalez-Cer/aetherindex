#include "aether/storage/vector_store.hpp"

#include <cmath>
#include <stdexcept>

namespace aetherindex {

VectorStore::VectorStore(std::size_t dimension, DistanceKind metric)
    : dimension_(dimension), metric_(metric) {
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

    bool nonzero = false;
    for (EmbeddingValue value : embedding) {
        if (!std::isfinite(value)) {
            return InsertStatus::InvalidEmbedding;
        }
        nonzero = nonzero || value != 0.0f;
    }
    if (metric_ == DistanceKind::Cosine && !nonzero) {
        return InsertStatus::InvalidEmbedding;
    }

    // Stage first: embedding may refer to data_ which insertion can reallocate.
    const std::vector<EmbeddingValue> pending(embedding.begin(), embedding.end());
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
    return std::span<const EmbeddingValue>(data_).subspan(position->second * dimension_,
                                                          dimension_);
}

std::size_t VectorStore::size() const noexcept { return ids_.size(); }

std::size_t VectorStore::dimension() const noexcept { return dimension_; }

DistanceKind VectorStore::distance_kind() const noexcept { return metric_; }

bool VectorStore::contains(VectorId id) const { return positions_.contains(id); }

std::span<const VectorId> VectorStore::ids() const noexcept { return ids_; }

}  // namespace aetherindex
