// Phase 5: compressed BM25 strategies must equal the exhaustive scorer exactly.
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/lexical/bm25.hpp"

using namespace aetherindex;
using namespace aetherindex::lexical;

namespace {

constexpr Bm25Strategy kStrategies[] = {Bm25Strategy::TermAtATime, Bm25Strategy::Wand,
                                        Bm25Strategy::BlockMaxWand};

// Zipf-like ranks via an explicit inverse transform (portable across libraries).
std::size_t zipf_rank(std::mt19937_64& engine, std::size_t vocabulary) {
    const double u = (static_cast<double>(engine() >> 11) + 0.5) / 9007199254740992.0;
    return std::min<std::size_t>(vocabulary - 1,
                                 static_cast<std::size_t>(std::pow(vocabulary, u)) - 1);
}

std::vector<std::string> zipf_corpus(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::vector<std::string> docs(count);
    for (auto& doc : docs) {
        const std::size_t length = 5 + engine() % 120;
        for (std::size_t t = 0; t < length; ++t)
            doc += (t ? " w" : "w") + std::to_string(zipf_rank(engine, 30000));
    }
    return docs;
}

// Many byte-identical documents: equal scores at the Top-K cut stress the tie rule.
std::vector<std::string> tie_corpus() {
    const std::vector<std::string> templates{"alpha beta", "alpha alpha gamma", "beta gamma delta",
                                             "alpha", "delta delta delta epsilon"};
    std::vector<std::string> docs;
    for (std::size_t i = 0; i < 1500; ++i) docs.push_back(templates[i % templates.size()]);
    return docs;
}

std::string random_query(std::mt19937_64& engine) {
    const std::size_t terms = engine() % 6;  // Includes the empty query.
    std::string query;
    for (std::size_t t = 0; t < terms; ++t) {
        const auto pick = engine() % 10;
        std::size_t rank = pick < 2   ? engine() % 50
                           : pick < 8 ? 50 + engine() % 3000
                                      : 30000 + engine() % 100;  // OOV
        query += " w" + std::to_string(rank);
        if (engine() % 7 == 0) query += " w" + std::to_string(rank);  // Repeated term.
    }
    return query;
}

std::unique_ptr<Bm25Index> build(const std::vector<std::string>& docs, Bm25Config config,
                                 std::uint64_t id_seed) {
    auto index = std::make_unique<Bm25Index>(config);
    std::mt19937_64 engine(id_seed);
    // Sparse, unordered DocumentIds: ordinals must follow DocumentId order, not insertion.
    std::vector<DocumentId> ids(docs.size());
    for (std::size_t i = 0; i < docs.size(); ++i) ids[i] = static_cast<DocumentId>(7 * i + 3);
    for (std::size_t i = ids.size(); i > 1; --i) std::swap(ids[i - 1], ids[engine() % i]);
    for (std::size_t i = 0; i < docs.size(); ++i) index->add_document(ids[i], docs[i]);
    index->build();
    return index;
}

std::size_t compare(const std::vector<std::string>& docs, const std::vector<std::string>& queries,
                    Bm25Config config) {
    const auto reference = build(docs, config, 11);
    std::size_t checked = 0;
    for (auto strategy : kStrategies) {
        auto variant_config = config;
        variant_config.strategy = strategy;
        const auto variant = build(docs, variant_config, 11);
        const auto memory = variant->posting_memory();
        std::cout << "docs=" << docs.size() << " postings=" << memory.postings
                  << " raw=" << memory.raw_bytes << " compressed=" << memory.compressed_bytes
                  << '\n';
        // Real corpora must shrink; a 5-term toy corpus is dominated by per-document arrays.
        require(memory.compressed_bytes > 0 &&
                    (memory.postings < 100000 || memory.compressed_bytes < memory.raw_bytes),
                "Compressed postings are not smaller than raw postings.");
        require(memory.postings == reference->posting_memory().postings, "Posting count differs.");
        for (std::size_t q = 0; q < queries.size(); ++q) {
            const std::size_t k = std::array<std::size_t, 5>{1, 5, 10, 100, 1000}[q % 5];
            const auto expected = reference->search(queries[q], k);
            const auto actual = variant->search(queries[q], k);
            require(actual.size() == expected.size(), "Strategy returned a different count.");
            for (std::size_t i = 0; i < actual.size(); ++i)
                require(actual[i].document_id == expected[i].document_id &&
                            actual[i].score == expected[i].score,
                        "Strategy differs from exhaustive (ID or bitwise score).");
            ++checked;
        }
    }
    return checked;
}

template <class Function>
void require_logic_error(Function action, const char* message) {
    try {
        action();
    } catch (const std::logic_error&) {
        return;
    }
    throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        std::mt19937_64 engine(2026);
        std::vector<std::string> queries(3000);
        for (auto& q : queries) q = random_query(engine);
        std::size_t checked = 0;
        const auto zipf = zipf_corpus(20000, 5);
        checked += compare(zipf, queries, {});
        checked += compare(zipf, std::vector<std::string>(queries.begin(), queries.begin() + 500),
                           {0.6, 0.0, Bm25Strategy::Exhaustive});  // k1 < 1, b = 0.
        checked += compare(zipf, std::vector<std::string>(queries.begin(), queries.begin() + 500),
                           {2.5, 1.0, Bm25Strategy::Exhaustive});
        const std::vector<std::string> tie_queries{"alpha",       "beta",  "gamma delta",
                                                   "alpha beta",  "delta", "epsilon alpha",
                                                   "alpha gamma", "zeta",  ""};
        std::vector<std::string> repeated;
        for (int r = 0; r < 60; ++r)
            repeated.insert(repeated.end(), tie_queries.begin(), tie_queries.end());
        checked += compare(tie_corpus(), repeated, {});

        // Compressed strategies release raw postings but keep statistics.
        Bm25Config wand;
        wand.strategy = Bm25Strategy::BlockMaxWand;
        const auto index = build(tie_corpus(), wand, 3);
        require(index->document_count() == 1500 && index->average_document_length() > 0 &&
                    index->inverted_index().document_length(3).has_value() &&
                    !index->inverted_index().has_postings(),
                "Statistics lost after releasing raw postings.");
        require_logic_error([&] { static_cast<void>(index->inverted_index().find("alpha")); },
                            "Raw postings still served after release.");
        // Empty corpus and k validation behave like the exhaustive path.
        Bm25Index empty(wand);
        empty.build();
        require(empty.search("alpha", 10).empty(), "Empty corpus returned results.");
        requireInvalidArgument([&] { static_cast<void>(index->search("alpha", 0)); },
                               "k=0 accepted by a compressed strategy.");
        Bm25Config bad;
        bad.strategy = static_cast<Bm25Strategy>(99);
        requireInvalidArgument([&] { validate_bm25_config(bad); }, "Unknown strategy accepted.");
        std::cout << checked << " query/strategy comparisons identical to exhaustive BM25.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
