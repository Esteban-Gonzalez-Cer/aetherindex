#include "aether/hybrid/rrf.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace aetherindex::hybrid {

namespace {

bool unit_interval(double value) { return std::isfinite(value) && value >= 0.0 && value <= 1.0; }

std::vector<RankedResult> ranked(std::unordered_map<DocumentId, double>& scores,
                                 std::size_t top_k) {
    std::vector<RankedResult> results;
    results.reserve(scores.size());
    for (const auto& [id, score] : scores) results.push_back({id, score});
    std::sort(results.begin(), results.end(), [](const RankedResult& a, const RankedResult& b) {
        return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
    });
    if (results.size() > top_k) results.resize(top_k);
    return results;
}

}  // namespace

void validate_rrf_config(const RrfConfig& config) {
    if (!std::isfinite(config.k_rrf) || config.k_rrf <= 0.0) {
        throw std::invalid_argument("RRF requires finite k_rrf > 0.");
    }
    if (config.method != FusionMethod::Rrf && config.method != FusionMethod::Convex) {
        throw std::invalid_argument("Unsupported fusion method.");
    }
    if (config.dense_weight && !unit_interval(*config.dense_weight)) {
        throw std::invalid_argument("RRF dense weight must be finite and in [0, 1].");
    }
    if (!unit_interval(config.alpha)) {
        throw std::invalid_argument("Convex fusion alpha must be finite and in [0, 1].");
    }
}

RrfFusion::RrfFusion(RrfConfig config) : config_(config) { validate_rrf_config(config_); }

std::vector<RankedResult> RrfFusion::fuse(std::span<const DocumentId> dense_ranking,
                                          std::span<const DocumentId> lexical_ranking,
                                          std::size_t top_k) const {
    if (top_k == 0) throw std::invalid_argument("RRF top_k must be greater than zero.");
    std::unordered_map<DocumentId, double> scores;
    const auto accumulate = [&](std::span<const DocumentId> ranking, double weight) {
        std::unordered_set<DocumentId> seen;
        for (std::size_t position = 0; position < ranking.size(); ++position) {
            const DocumentId id = ranking[position];
            if (!seen.insert(id).second) {
                throw std::invalid_argument("Duplicate DocumentId inside one input ranking.");
            }
            // Convert before adding to avoid an integer position+1 overflow.
            const double rank = static_cast<double>(position) + 1.0;
            const double contribution = weight / (config_.k_rrf + rank);
            if (!std::isfinite(contribution) || contribution < 0.0 ||
                (weight > 0.0 && contribution == 0.0)) {
                throw std::overflow_error("RRF contribution is not representable.");
            }
            auto& score = scores[id];
            score += contribution;
            if (!std::isfinite(score)) throw std::overflow_error("RRF score is not finite.");
        }
    };
    // Fixed accumulation order; a fresh seen set for each ranking permits overlap.
    // Without dense_weight both weights are exactly 1, so classic RRF is unchanged.
    const double dense_weight = config_.dense_weight.value_or(1.0);
    const double lexical_weight = config_.dense_weight ? 1.0 - *config_.dense_weight : 1.0;
    accumulate(dense_ranking, dense_weight);
    accumulate(lexical_ranking, lexical_weight);
    return ranked(scores, top_k);
}

std::vector<RankedResult> RrfFusion::fuse_scored(std::span<const RankedResult> dense,
                                                 std::span<const RankedResult> lexical,
                                                 std::size_t top_k) const {
    if (config_.method == FusionMethod::Rrf) {
        std::vector<DocumentId> dense_ids, lexical_ids;
        dense_ids.reserve(dense.size());
        lexical_ids.reserve(lexical.size());
        for (const auto& hit : dense) dense_ids.push_back(hit.document_id);
        for (const auto& hit : lexical) lexical_ids.push_back(hit.document_id);
        return fuse(dense_ids, lexical_ids, top_k);
    }
    if (top_k == 0) throw std::invalid_argument("Fusion top_k must be greater than zero.");
    std::unordered_map<DocumentId, double> scores;
    // Products and sums are separate statements so that no fused multiply-add changes the
    // rounding (the reference implementation in benchmarks/market/fuse_rrf.py agrees).
    const auto accumulate = [&](std::span<const RankedResult> list, double factor) {
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (const auto& hit : list) {
            if (!std::isfinite(hit.score))
                throw std::invalid_argument("Convex fusion requires finite scores.");
            low = std::min(low, hit.score);
            high = std::max(high, hit.score);
        }
        std::unordered_set<DocumentId> seen;
        for (const auto& hit : list) {
            if (!seen.insert(hit.document_id).second) {
                throw std::invalid_argument("Duplicate DocumentId inside one input ranking.");
            }
            const double normalized = high > low ? (hit.score - low) / (high - low) : 1.0;
            const double weighted = factor * normalized;
            auto& score = scores[hit.document_id];
            score += weighted;
        }
    };
    accumulate(dense, config_.alpha);
    accumulate(lexical, 1.0 - config_.alpha);
    return ranked(scores, top_k);
}

RrfConfig RrfFusion::config() const noexcept { return config_; }

}  // namespace aetherindex::hybrid
