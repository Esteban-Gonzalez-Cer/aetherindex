// Phase 2: parallel HNSW construction and concurrent search.
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <random>
#include <unordered_set>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/core/parallel.hpp"

using namespace aetherindex;

namespace {

constexpr std::size_t kDimension = 32, kVectors = 6000, kQueries = 200, kK = 10;

// Clustered rows with an explicit integer-to-float mapping (reproducible everywhere).
std::vector<std::vector<float>> rows(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 centers_engine(99), engine(seed);
    const auto real = [](std::mt19937_64& e) {
        return static_cast<float>(static_cast<double>(e() >> 11) / 9007199254740992.0 - 0.5);
    };
    std::vector<std::vector<float>> centers(40, std::vector<float>(kDimension));
    for (auto& c : centers)
        for (auto& v : c) v = 4.0f * real(centers_engine);
    std::vector<std::vector<float>> out(count, std::vector<float>(kDimension));
    for (auto& row : out) {
        const auto& c = centers[engine() % centers.size()];
        for (std::size_t d = 0; d < kDimension; ++d) row[d] = c[d] + real(engine);
    }
    return out;
}

double recall(const HnswIndex& index, const VectorStore& store,
              const std::vector<std::vector<float>>& queries) {
    double total = 0;
    for (const auto& q : queries) {
        std::unordered_set<VectorId> exact;
        for (const auto& hit : bruteForceSearch(store, q, kK)) exact.insert(hit.id);
        std::size_t matches = 0;
        for (const auto& hit : index.search(q, kK)) matches += exact.contains(hit.id);
        total += static_cast<double>(matches) / kK;
    }
    return total / static_cast<double>(queries.size());
}

void require_same_graph(const HnswIndex& a, const HnswIndex& b, const VectorStore& store) {
    require(a.size() == b.size() && a.entry_point() == b.entry_point() &&
                a.max_level() == b.max_level(),
            "Graphs differ in size, entry point or top level.");
    for (VectorId id : store.ids()) {
        const auto na = a.find_node(id), nb = b.find_node(id);
        require(na && nb && na->max_level == nb->max_level, "Node levels differ.");
        for (std::size_t level = 0; level <= na->max_level; ++level) {
            const auto la = a.neighbors(id, level), lb = b.neighbors(id, level);
            require(std::equal(la.begin(), la.end(), lb.begin(), lb.end()),
                    "Adjacency lists differ.");
        }
    }
}

}  // namespace

int main() {
    try {
        VectorStore store(kDimension, DistanceKind::Cosine);
        const auto data = rows(kVectors, 7);
        for (std::size_t i = 0; i < data.size(); ++i)
            require(
                store.insert(static_cast<VectorId>(5 * i + 3), data[i]) == InsertStatus::Success,
                "Fixture insert failed.");
        const auto queries = rows(kQueries, 1234);
        const std::vector<VectorId> ids(store.ids().begin(), store.ids().end());

        HnswIndex sequential(store);
        for (VectorId id : ids) sequential.insert(id);
        const double base_recall = recall(sequential, store, queries);

        // threads=1 is exactly the sequential construction.
        HnswIndex single(store);
        single.insert_parallel(ids, 1);
        require_same_graph(sequential, single, store);

        for (std::size_t threads : {2, 4, 14}) {
            HnswIndex parallel(store);
            parallel.insert_parallel(ids, threads);
            require(parallel.size() == kVectors, "Parallel build lost nodes.");
            require(parallel.validate_graph().valid(), "Parallel graph violates invariants.");
            // Levels come from the seeded generator in input order, independent of threads.
            for (VectorId id : ids)
                require(parallel.find_node(id)->max_level == sequential.find_node(id)->max_level,
                        "Parallel build changed level assignment.");
            const double parallel_recall = recall(parallel, store, queries);
            std::cout << "threads=" << threads << " recall@10=" << parallel_recall
                      << " (sequential " << base_recall << ")\n";
            require(parallel_recall >= base_recall - 0.005,
                    "Parallel build recall fell more than 0.005 below sequential.");
        }

        // Incremental: sequential prefix, parallel remainder.
        HnswIndex mixed(store);
        for (std::size_t i = 0; i < kVectors / 2; ++i) mixed.insert(ids[i]);
        mixed.insert_parallel(std::span(ids).subspan(kVectors / 2), 4);
        require(mixed.size() == kVectors && mixed.validate_graph().valid(),
                "Incremental parallel build is invalid.");

        // Validation happens before mutation: the index stays usable and unchanged.
        HnswIndex guarded(store);
        guarded.insert_parallel(std::span(ids).first(100), 4);
        std::vector<VectorId> missing{ids[100], 999999999};
        requireInvalidArgument([&] { guarded.insert_parallel(missing, 4); },
                               "Unknown VectorId accepted.");
        std::vector<VectorId> repeated{ids[100], ids[100]};
        requireInvalidArgument([&] { guarded.insert_parallel(repeated, 4); },
                               "Repeated VectorId accepted.");
        std::vector<VectorId> already{ids[0]};
        requireInvalidArgument([&] { guarded.insert_parallel(already, 4); },
                               "Indexed VectorId accepted again.");
        require(guarded.size() == 100 && guarded.validate_graph().valid() &&
                    !guarded.search(queries[0], kK).empty(),
                "Rejected batch changed or broke the index.");
        HnswConfig extended;
        extended.extend_candidates = true;
        HnswIndex extend(store, extended);
        requireInvalidArgument([&] { extend.insert_parallel(ids, 4); },
                               "extend_candidates with threads > 1 accepted.");

        // Concurrent read-only searches return exactly the sequential results.
        std::vector<std::vector<SearchResult>> expected(queries.size()), actual(queries.size());
        for (std::size_t q = 0; q < queries.size(); ++q)
            expected[q] = sequential.search(queries[q], kK);
        parallel_for(queries.size(), 8,
                     [&](std::size_t q) { actual[q] = sequential.search(queries[q], kK); });
        for (std::size_t q = 0; q < queries.size(); ++q) {
            require(actual[q].size() == expected[q].size(), "Concurrent search size differs.");
            for (std::size_t i = 0; i < actual[q].size(); ++i)
                require(actual[q][i].id == expected[q][i].id &&
                            actual[q][i].distance == expected[q][i].distance,
                        "Concurrent search differs from sequential search.");
        }
        // parallel_for rethrows the first worker exception.
        bool propagated = false;
        try {
            parallel_for(100, 4, [](std::size_t i) {
                if (i == 37) throw std::runtime_error("boom");
            });
        } catch (const std::runtime_error&) {
            propagated = true;
        }
        require(propagated, "parallel_for swallowed a worker exception.");
        std::cout << "Parallel HNSW construction and concurrent search checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
