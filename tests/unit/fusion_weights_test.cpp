// Opt-in hybrid fusion: weighted RRF and convex combination of min-max normalized scores.
// The default configuration must stay classic RRF.
#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/hybrid/rrf.hpp"

using namespace aetherindex;
using namespace aetherindex::hybrid;

namespace {

using Ranking = std::vector<DocumentId>;
using Scored = std::vector<RankedResult>;

void same(const std::vector<RankedResult>& actual, const std::vector<RankedResult>& expected,
          const char* message) {
    require(actual.size() == expected.size(), message);
    for (std::size_t i = 0; i < actual.size(); ++i)
        require(actual[i].document_id == expected[i].document_id &&
                    actual[i].score == expected[i].score,
                message);
}

Scored scored(const Ranking& ids, const std::vector<double>& scores) {
    Scored out;
    for (std::size_t i = 0; i < ids.size(); ++i) out.push_back({ids[i], scores[i]});
    return out;
}

void check_weighted_rrf() {
    const RrfFusion classic;
    const Ranking dense{4, 9, 2, 7}, lexical{2, 5, 4};
    // The default is classic RRF, also through fuse_scored.
    require(!RrfConfig{}.dense_weight && RrfConfig{}.method == FusionMethod::Rrf,
            "Default fusion is not classic RRF.");
    same(classic.fuse_scored(scored(dense, {0.9, 0.8, 0.7, 0.6}), scored(lexical, {3, 2, 1}), 10),
         classic.fuse(dense, lexical, 10), "fuse_scored changed classic RRF.");

    RrfConfig config;
    config.dense_weight = 0.25;
    const RrfFusion weighted(config);
    const auto fused = weighted.fuse(dense, lexical, 10);
    const auto contribution = [](double weight, double rank) { return weight / (60.0 + rank); };
    const auto expected_score = [&](DocumentId id) {
        double score = 0.0;
        for (std::size_t r = 0; r < dense.size(); ++r)
            if (dense[r] == id) score += contribution(0.25, r + 1.0);
        for (std::size_t r = 0; r < lexical.size(); ++r)
            if (lexical[r] == id) score += contribution(0.75, r + 1.0);
        return score;
    };
    require(fused.size() == 5, "Weighted RRF lost documents.");
    for (const auto& hit : fused)
        require(hit.score == expected_score(hit.document_id), "Weighted RRF score differs.");
    require(fused.front().document_id == 2, "Lexical-heavy weighting did not favor document 2.");

    // W = 0.5 halves every classic score exactly, so the ranking is unchanged.
    std::mt19937 random(9);
    config.dense_weight = 0.5;
    const RrfFusion half(config);
    for (int trial = 0; trial < 200; ++trial) {
        Ranking a, b;
        for (DocumentId id = 0; id < 40; ++id) {
            if (random() % 3 == 0) a.push_back(id);
            if (random() % 3 == 0) b.push_back(id);
        }
        std::shuffle(a.begin(), a.end(), random);
        std::shuffle(b.begin(), b.end(), random);
        const auto expected = classic.fuse(a, b, 15);
        const auto actual = half.fuse(a, b, 15);
        require(actual.size() == expected.size(), "W = 0.5 changed the result count.");
        for (std::size_t i = 0; i < actual.size(); ++i)
            require(actual[i].document_id == expected[i].document_id &&
                        actual[i].score == expected[i].score * 0.5,
                    "W = 0.5 is not classic RRF scaled by one half.");
    }
    // W = 1 keeps lexical-only documents with score 0, ordered by DocumentId.
    config.dense_weight = 1.0;
    const auto dense_only = RrfFusion(config).fuse(Ranking{8, 3}, Ranking{6, 1}, 10);
    require(dense_only.size() == 4 && dense_only[0].document_id == 8 &&
                dense_only[1].document_id == 3 && dense_only[2].document_id == 1 &&
                dense_only[3].document_id == 6 && dense_only[3].score == 0.0,
            "W = 1 did not reduce to the dense ranking.");
}

void check_convex() {
    RrfConfig config;
    config.method = FusionMethod::Convex;
    config.alpha = 0.6;
    const RrfFusion convex(config);
    // Dense normalizes to 1, 0.5, 0; lexical to 1, 1/3, 0.
    const auto fused = convex.fuse_scored(scored({1, 2, 3}, {0.9, 0.7, 0.5}),
                                          scored({3, 4, 5}, {10.0, 4.0, 1.0}), 10);
    const double third = (4.0 - 1.0) / (10.0 - 1.0);
    const double a = 0.6, l = 1.0 - 0.6;
    const std::vector<RankedResult> expected{{1, a * 1.0},
                                             {3, a * 0.0 + l * 1.0},
                                             {2, a * ((0.7 - 0.5) / (0.9 - 0.5))},
                                             {4, l * third},
                                             {5, l * 0.0}};
    same(fused, expected, "Convex fusion scores differ from the definition.");

    // alpha = 1 and 0 reduce to one retriever; missing documents score 0 (ties by ID).
    config.alpha = 1.0;
    const auto dense = RrfFusion(config).fuse_scored(scored({7, 2}, {0.3, 0.1}),
                                                     scored({9, 4}, {5.0, 1.0}), 10);
    require(dense[0].document_id == 7 && dense[1].document_id == 2 && dense[2].document_id == 4 &&
                dense[3].document_id == 9,
            "alpha = 1 did not keep the dense order.");
    config.alpha = 0.0;
    const auto lexical = RrfFusion(config).fuse_scored(scored({7, 2}, {0.3, 0.1}),
                                                       scored({9, 4}, {5.0, 1.0}), 10);
    require(lexical[0].document_id == 9 && lexical[1].document_id == 2,
            "alpha = 0 did not keep the lexical order.");
    // A single candidate, or equal scores, normalize to 1.
    config.alpha = 0.5;
    const auto single = RrfFusion(config).fuse_scored(scored({5}, {0.2}),
                                                      scored({6, 7}, {2.0, 2.0}), 10);
    require(single.size() == 3 && single[0].score == 0.5 && single[1].score == 0.5 &&
                single[2].score == 0.5 && single[0].document_id == 5,
            "Degenerate lists did not normalize to 1.");
    require(RrfFusion(config).fuse_scored({}, {}, 3).empty(), "Empty inputs returned results.");
    requireInvalidArgument(
        [&] { static_cast<void>(RrfFusion(config).fuse_scored(scored({1, 1}, {1, 0}), {}, 3)); },
        "Duplicate DocumentId accepted by convex fusion.");
    requireInvalidArgument(
        [&] {
            static_cast<void>(RrfFusion(config).fuse_scored(
                scored({1}, {std::numeric_limits<double>::quiet_NaN()}), {}, 3));
        },
        "Non-finite score accepted by convex fusion.");
    requireInvalidArgument([&] { static_cast<void>(RrfFusion(config).fuse_scored({}, {}, 0)); },
                           "top_k = 0 accepted by convex fusion.");
}

void check_validation() {
    for (double bad : {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()}) {
        RrfConfig weight;
        weight.dense_weight = bad;
        requireInvalidArgument([&] { validate_rrf_config(weight); }, "Bad dense weight accepted.");
        RrfConfig alpha;
        alpha.method = FusionMethod::Convex;
        alpha.alpha = bad;
        requireInvalidArgument([&] { validate_rrf_config(alpha); }, "Bad alpha accepted.");
    }
    RrfConfig method;
    method.method = static_cast<FusionMethod>(7);
    requireInvalidArgument([&] { validate_rrf_config(method); }, "Unknown method accepted.");
}

}  // namespace

int main() {
    try {
        check_weighted_rrf();
        check_convex();
        check_validation();
        std::cout << "Weighted RRF and convex fusion checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
