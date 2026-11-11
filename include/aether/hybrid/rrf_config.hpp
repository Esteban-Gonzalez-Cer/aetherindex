#pragma once

#include <optional>

namespace aetherindex::hybrid {

// How the dense and lexical rankings are fused.
//   Rrf     score = w_d / (k_rrf + rank_d) + w_l / (k_rrf + rank_l), ranks from 1. Without
//           dense_weight both weights are 1 (classic RRF); with it, w_d = dense_weight and
//           w_l = 1 - dense_weight.
//   Convex  score = alpha * dense + (1 - alpha) * lexical, where each retriever's scores
//           are min-max normalized over its own candidate list. A document missing from a
//           list contributes 0 there; a list whose scores are all equal normalizes to 1.
// RrfConfig{} is classic RRF, the only method RrfFusion::fuse (rankings without scores)
// supports. Hybrid search (SearchRequest) defaults to kDefaultSearchFusion instead.
enum class FusionMethod { Rrf, Convex };

// alpha chosen on BEIR dev splits only (mean dev nDCG@10 over SciFact, NFCorpus, FiQA and
// Quora with bge-small-en-v1.5 and english-v1 BM25); see docs/reports/MARKET_COMPARISON.md.
inline constexpr double kTunedConvexAlpha = 0.65;

struct RrfConfig {
    double k_rrf = 60.0;
    FusionMethod method = FusionMethod::Rrf;
    std::optional<double> dense_weight{};  // Rrf only, in [0, 1].
    double alpha = kTunedConvexAlpha;      // Convex only, in [0, 1].
};

// Default fusion of hybrid search: convex with the dev-tuned alpha.
inline constexpr RrfConfig kDefaultSearchFusion{.method = FusionMethod::Convex};

// Throws invalid_argument unless k_rrf is finite and strictly positive, the method is
// known, and dense_weight (when set) and alpha are finite and in [0, 1].
void validate_rrf_config(const RrfConfig& config);

}  // namespace aetherindex::hybrid
