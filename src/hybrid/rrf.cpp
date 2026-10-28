#include "aether/hybrid/rrf.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace aetherindex::hybrid {

void validate_rrf_config(const RrfConfig& config) {
    if (!std::isfinite(config.k_rrf) || config.k_rrf <= 0.0) {
        throw std::invalid_argument("RRF requires finite k_rrf > 0.");
    }
}

RrfFusion::RrfFusion(RrfConfig config) : config_(config) { validate_rrf_config(config_); }

std::vector<RankedResult> RrfFusion::fuse(std::span<const DocumentId> dense_ranking,
                                          std::span<const DocumentId> lexical_ranking,
                                          std::size_t top_k) const {
    if (top_k == 0) throw std::invalid_argument("RRF top_k must be greater than zero.");
    std::unordered_map<DocumentId, double> scores;
    const auto accumulate = [&](std::span<const DocumentId> ranking) {
        std::unordered_set<DocumentId> seen;
        for (std::size_t position = 0; position < ranking.size(); ++position) {
            const DocumentId id = ranking[position];
            if (!seen.insert(id).second) {
                throw std::invalid_argument("Duplicate DocumentId inside one input ranking.");
            }
            // Convert before adding to avoid an integer position+1 overflow.
            const double rank = static_cast<double>(position) + 1.0;
            const double contribution = 1.0 / (config_.k_rrf + rank);
            if (!std::isfinite(contribution) || contribution <= 0.0) {
                throw std::overflow_error("RRF contribution is not representable.");
            }
            auto& score = scores[id];
            score += contribution;
            if (!std::isfinite(score)) throw std::overflow_error("RRF score is not finite.");
        }
    };
    // Fixed accumulation order; a fresh seen set for each ranking permits overlap.
    accumulate(dense_ranking);
    accumulate(lexical_ranking);
    std::vector<RankedResult> results;
    results.reserve(scores.size());
    for (const auto& [id, score] : scores) results.push_back({id, score});
    std::sort(results.begin(), results.end(), [](const RankedResult& a, const RankedResult& b) {
        return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
    });
    if (results.size() > top_k) results.resize(top_k);
    return results;
}

RrfConfig RrfFusion::config() const noexcept { return config_; }

}  // namespace aetherindex::hybrid
