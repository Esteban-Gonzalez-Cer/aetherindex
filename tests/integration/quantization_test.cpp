// Phases 3-4: quantized codes and quantized HNSW traversal.
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/quantization/pq.hpp"
#include "aether/quantization/sq8.hpp"

using namespace aetherindex;

namespace {

float real(std::mt19937_64& engine) {
    return static_cast<float>(static_cast<double>(engine() >> 11) / 9007199254740992.0 - 0.5);
}

std::vector<std::vector<float>> clustered(std::size_t count, std::size_t dimension,
                                          std::uint64_t seed) {
    std::mt19937_64 centers_engine(17), engine(seed);
    std::vector<std::vector<float>> centers(50, std::vector<float>(dimension));
    for (auto& c : centers)
        for (auto& v : c) v = 3.0f * real(centers_engine);
    std::vector<std::vector<float>> rows(count, std::vector<float>(dimension));
    for (auto& row : rows) {
        const auto& c = centers[engine() % centers.size()];
        for (std::size_t d = 0; d < dimension; ++d) row[d] = c[d] + real(engine);
    }
    return rows;
}

template <class Error, class Function>
void require_throws(Function action, const char* message) {
    try {
        action();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(message);
}

void check_kernel() {
    std::mt19937_64 engine(5);
    for (std::size_t length : {16, 32, 48, 128, 1024}) {
        std::vector<float> weights(length);
        std::vector<std::uint8_t> codes(length);
        for (std::size_t i = 0; i < length; ++i) {
            weights[i] = real(engine);
            codes[i] = static_cast<std::uint8_t>(engine() % 256);
        }
        const float fast = detail::weighted_code_sum(weights.data(), codes.data(), length);
        const float reference =
            detail::weighted_code_sum_scalar(weights.data(), codes.data(), length);
        double exact = 0;
        for (std::size_t i = 0; i < length; ++i)
            exact += static_cast<double>(weights[i]) * codes[i];
        require(std::abs(fast - exact) <= 1e-3 * (1.0 + std::abs(exact)) &&
                    std::abs(reference - exact) <= 1e-3 * (1.0 + std::abs(exact)),
                "SQ8 kernel differs from the exact weighted sum.");
    }
}

// Reconstruction bound, asymmetric distance == distance to the decoded row, and both metrics.
void check_codes(DistanceKind metric) {
    for (std::size_t dimension : {1, 15, 16, 17, 37, 64}) {
        VectorStore store(dimension, metric);
        const auto rows = clustered(300, dimension, 9 + dimension);
        for (std::size_t i = 0; i < rows.size(); ++i)
            require(store.insert(static_cast<VectorId>(i), rows[i]) == InsertStatus::Success,
                    "Fixture insert failed.");
        const Sq8Vectors codes(store);
        require(codes.size() == store.size() && codes.dimension() == dimension &&
                    codes.metric() == metric,
                "SQ8 shape incorrect.");
        require(codes.memory_bytes() < store.size() * store.padded_dimension() * sizeof(float),
                "SQ8 codes are not smaller than FP32.");
        for (std::size_t p = 0; p < store.size(); ++p) {
            const auto decoded = codes.decode(p);
            const float* row = store.data_by_position(p);
            for (std::size_t d = 0; d < dimension; ++d)
                require(std::abs(decoded[d] - row[d]) <= codes.scale(d) * 0.5f + 1e-6f,
                        "SQ8 reconstruction exceeds scale/2.");
        }
        const auto queries = clustered(20, dimension, 77);
        for (const auto& q : queries) {
            const PreparedQuery prepared(q, dimension, metric);
            const auto approximate = codes.prepare(prepared);
            for (std::size_t p = 0; p < store.size(); p += 7) {
                const auto decoded = codes.decode(p);
                double expected = 0;
                for (std::size_t d = 0; d < dimension; ++d) {
                    if (metric == DistanceKind::Cosine)
                        expected += static_cast<double>(prepared.data()[d]) * decoded[d];
                    else {
                        const double diff = static_cast<double>(prepared.data()[d]) - decoded[d];
                        expected += diff * diff;
                    }
                }
                if (metric == DistanceKind::Cosine)
                    expected = 1.0 - std::clamp(expected, -1.0, 1.0);
                require(std::abs(approximate->distance(p) - expected) <=
                            2e-4 * (1.0 + std::abs(expected)),
                        "SQ8 asymmetric distance differs from the decoded-row distance.");
            }
        }
        const std::vector<float> wrong(dimension + 1, 1.0f);
        requireInvalidArgument([&] { codes.prepare(PreparedQuery(wrong, dimension + 1, metric)); },
                               "Mismatched query accepted by SQ8.");
    }
    // A constant dimension (zero range) is coded exactly.
    VectorStore constant(3, DistanceKind::SquaredEuclidean);
    constant.insert(1, std::vector<float>{2.0f, 0.0f, 5.0f});
    constant.insert(2, std::vector<float>{2.0f, 1.0f, 5.0f});
    const Sq8Vectors flat(constant);
    require(flat.decode(0)[0] == 2.0f && flat.decode(1)[2] == 5.0f, "Constant dimension lost.");
}

double recall_of(const std::function<std::vector<SearchResult>(const std::vector<float>&)>& search,
                 const std::vector<std::vector<float>>& queries,
                 const std::vector<std::unordered_set<VectorId>>& truth) {
    double total = 0;
    for (std::size_t q = 0; q < queries.size(); ++q) {
        std::size_t hits = 0;
        for (const auto& r : search(queries[q])) hits += truth[q].contains(r.id);
        total += hits / 10.0;
    }
    return total / static_cast<double>(queries.size());
}

using CodeFactory = std::function<std::unique_ptr<QuantizedVectors>(const VectorStore&)>;

// Recall of quantized traversal (with/without exact re-rank) against FP32 traversal,
// then the FP32-release contract.
// PQ needs a deeper candidate list than SQ8 for the exact re-rank to recover recall
// (measured: m=16 on this fixture reaches 1.0 at ef=128 but 0.90 at ef=64).
void check_hnsw(const std::string& label, const CodeFactory& make, std::size_t ef,
                double rerank_tolerance, double approximate_floor) {
    constexpr std::size_t dimension = 64, k = 10;
    for (auto metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        VectorStore store(dimension, metric);
        const auto rows = clustered(5000, dimension, 3);
        for (std::size_t i = 0; i < rows.size(); ++i)
            store.insert(static_cast<VectorId>(3 * i + 1), rows[i]);
        HnswIndex index(store);
        for (VectorId id : store.ids()) index.insert(id);
        const auto codes = make(store);
        const auto queries = clustered(200, dimension, 4242);
        std::vector<std::unordered_set<VectorId>> truth;
        for (const auto& q : queries) {
            std::unordered_set<VectorId> ids;
            for (const auto& hit : bruteForceSearch(store, q, k)) ids.insert(hit.id);
            truth.push_back(std::move(ids));
        }
        const auto prepared = [&](const std::vector<float>& q) {
            return std::make_unique<PreparedQuery>(q, dimension, metric);
        };
        const double fp32 =
            recall_of([&](const auto& q) { return index.search(q, k, 64); }, queries, truth);
        const double reranked = recall_of(
            [&](const auto& q) { return index.search_quantized(*prepared(q), *codes, k, ef, ef); },
            queries, truth);
        const double approximate = recall_of(
            [&](const auto& q) { return index.search_quantized(*prepared(q), *codes, k, ef, 0); },
            queries, truth);
        std::cout << label << " metric=" << static_cast<int>(metric) << " recall@10 fp32=" << fp32
                  << " reranked=" << reranked << " approximate=" << approximate << '\n';
        require(reranked >= fp32 - rerank_tolerance,
                "Re-ranked quantized recall dropped too much.");
        require(approximate >= approximate_floor, "Approximate quantized recall too low.");
        // Re-ranked distances are exact FP32 distances, ordered.
        const auto exact = index.search_quantized(*prepared(queries[0]), *codes, k, 64, 64);
        for (std::size_t i = 1; i < exact.size(); ++i)
            require(exact[i - 1].distance <= exact[i].distance, "Re-ranked results not sorted.");

        requireInvalidArgument(
            [&] { index.search_quantized(*prepared(queries[0]), *codes, k, 64, 5); },
            "rerank < k accepted.");
        VectorStore other(dimension, metric);
        other.insert(1, rows[0]);
        const auto foreign = make(other);
        requireInvalidArgument(
            [&] { index.search_quantized(*prepared(queries[0]), *foreign, k, 64, 0); },
            "Codes of another store accepted.");

        // Releasing FP32 rows: approximate search still works and is unchanged.
        std::vector<std::vector<SearchResult>> before;
        for (std::size_t q = 0; q < 20; ++q)
            before.push_back(index.search_quantized(*prepared(queries[q]), *codes, k, 64, 0));
        store.release_rows();
        require(!store.has_rows(), "Rows not released.");
        for (std::size_t q = 0; q < 20; ++q) {
            const auto after = index.search_quantized(*prepared(queries[q]), *codes, k, 64, 0);
            require(after.size() == before[q].size(), "Release changed result count.");
            for (std::size_t i = 0; i < after.size(); ++i)
                require(
                    after[i].id == before[q][i].id && after[i].distance == before[q][i].distance,
                    "Release changed approximate results.");
        }
        require_throws<std::logic_error>(
            [&] { index.search_quantized(*prepared(queries[0]), *codes, k, 64, 64); },
            "Re-rank without FP32 rows accepted.");
        require_throws<std::logic_error>([&] { index.search(queries[0], k); },
                                         "FP32 HNSW search without rows accepted.");
        require_throws<std::logic_error>([&] { bruteForceSearch(store, queries[0], k); },
                                         "Brute force without rows accepted.");
        require_throws<std::logic_error>([&] { store.insert(999999, rows[0]); },
                                         "Insert after release accepted.");
        require_throws<std::logic_error>([&] { store.find(1); }, "find after release accepted.");
        requireInvalidArgument([&] { make(store); }, "Training without rows accepted.");
    }
}

void check_pq() {
    constexpr std::size_t dimension = 32;
    for (auto metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
        VectorStore store(dimension, metric);
        const auto rows = clustered(3000, dimension, 21);
        for (std::size_t i = 0; i < rows.size(); ++i)
            store.insert(static_cast<VectorId>(i), rows[i]);
        PqConfig one, many;
        one.threads = 1;
        many.threads = 6;
        const PqVectors a(store, one), b(store, many);
        require(a.subspaces() == 8 && a.centroids() == 256, "Automatic PQ shape incorrect.");
        require(a.memory_bytes() > 0 && store.size() * a.subspaces() <= a.memory_bytes(),
                "PQ memory accounting incorrect.");
        for (std::size_t p = 0; p < store.size(); ++p)
            require(std::equal(a.codes(p), a.codes(p) + a.subspaces(), b.codes(p)),
                    "PQ training depends on the thread count.");
        PqConfig other_seed;
        other_seed.seed = 7;
        const PqVectors c(store, other_seed);
        bool differs = false;
        for (std::size_t p = 0; p < store.size() && !differs; ++p)
            differs = !std::equal(a.codes(p), a.codes(p) + a.subspaces(), c.codes(p));
        require(differs, "PQ seed has no effect.");
        // ADC equals the distance to the reconstructed row.
        const auto queries = clustered(10, dimension, 55);
        for (const auto& q : queries) {
            const PreparedQuery prepared(q, dimension, metric);
            const auto table = a.prepare(prepared);
            for (std::size_t p = 0; p < store.size(); p += 13) {
                const auto decoded = a.decode(p);
                double expected = 0;
                for (std::size_t d = 0; d < dimension; ++d) {
                    if (metric == DistanceKind::Cosine)
                        expected += static_cast<double>(prepared.data()[d]) * decoded[d];
                    else {
                        const double diff = static_cast<double>(prepared.data()[d]) - decoded[d];
                        expected += diff * diff;
                    }
                }
                if (metric == DistanceKind::Cosine)
                    expected = 1.0 - std::clamp(expected, -1.0, 1.0);
                require(
                    std::abs(table->distance(p) - expected) <= 1e-4 * (1.0 + std::abs(expected)),
                    "PQ ADC differs from the reconstructed-row distance.");
            }
        }
        PqConfig invalid;
        invalid.subspaces = 5;
        requireInvalidArgument([&] { PqVectors(store, invalid); }, "Non-dividing m accepted.");
        invalid.subspaces = 64;
        requireInvalidArgument([&] { PqVectors(store, invalid); }, "m > dimension accepted.");
    }
    // Fewer rows than 256 centroids still trains (k = rows).
    VectorStore tiny(8, DistanceKind::SquaredEuclidean);
    const auto rows = clustered(40, 8, 3);
    for (std::size_t i = 0; i < rows.size(); ++i) tiny.insert(static_cast<VectorId>(i), rows[i]);
    const PqVectors small(tiny);
    require(small.centroids() == 40 && small.subspaces() == 2, "Small-store PQ shape incorrect.");
    require(default_pq_subspaces(1024) == 256 && default_pq_subspaces(128) == 32 &&
                default_pq_subspaces(37) == 1 && default_pq_subspaces(30) == 6,
            "Automatic PQ subspaces incorrect.");
}

}  // namespace

int main() {
    try {
        check_kernel();
        check_codes(DistanceKind::Cosine);
        check_codes(DistanceKind::SquaredEuclidean);
        check_hnsw(
            "SQ8", [](const VectorStore& s) { return std::make_unique<Sq8Vectors>(s); }, 64, 0.01,
            0.85);
        check_pq();
        check_hnsw(
            "PQ", [](const VectorStore& s) { return std::make_unique<PqVectors>(s); }, 128, 0.02,
            0.25);
        std::cout << "Quantization checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
