// Phase 6: metadata filtering for HNSW, brute force, BM25 and hybrid search.
#include "aether/application/filter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/search_service.hpp"
#include "aether/lexical/bm25.hpp"

using namespace aetherindex;

namespace {

float real(std::mt19937_64& engine) {
    return static_cast<float>(static_cast<double>(engine() >> 11) / 9007199254740992.0 - 0.5);
}

void check_parsing() {
    require(parse_date("1970-01-01") == 0 && parse_date("1970-01-02") == 1 &&
                parse_date("2000-03-01") == 11017 && parse_date("2024-02-29").has_value() &&
                parse_date("1969-12-31") == -1,
            "Date conversion incorrect.");
    for (const char* bad : {"2023-02-29", "1900-02-29", "2026-13-01", "2026-04-31", "2026-4-01",
                            "2026/04/01", "", "abcd-ef-gh", "2026-00-10"})
        require(!parse_date(bad), "Invalid date accepted.");
    const auto any = parse_equality_filter("tenant=acme|globex");
    require(any.field == "tenant" && any.values.size() == 2 && any.values[1] == "globex" &&
                !any.date_range,
            "Equality filter parsing incorrect.");
    const auto range = parse_date_filter("published=2026-01-01..");
    require(range.date_range && range.from_day == parse_date("2026-01-01") && !range.to_day,
            "Open date range parsing incorrect.");
    for (const char* bad : {"tenant", "=acme", "tenant=", "tenant=a||b", "tenant=a|"})
        requireInvalidArgument([&] { parse_equality_filter(bad); }, "Bad --filter accepted.");
    for (const char* bad :
         {"d=2026-01-01", "d=..", "d=2026-02-30..", "d=2026-05-01..2026-01-01", "=2026-01-01.."})
        requireInvalidArgument([&] { parse_date_filter(bad); }, "Bad --filter-date accepted.");
    const Metadata metadata{{"tenant", "acme"}, {"published", "2026-03-15"}};
    require(matches_filters(metadata, {any, parse_date_filter("published=2026-03-01..2026-03-31")}),
            "Matching metadata rejected.");
    require(!matches_filters(metadata, {parse_equality_filter("tenant=globex")}) &&
                !matches_filters(metadata, {parse_date_filter("published=..2026-03-14")}) &&
                !matches_filters(metadata, {parse_equality_filter("region=eu")}),
            "Non-matching metadata accepted.");
}

// Filtered HNSW against an exact filtered ground truth at several selectivities.
void check_dense() {
    constexpr std::size_t dimension = 32, count = 8000, k = 10;
    std::mt19937_64 engine(8), centers_engine(9);
    std::vector<std::vector<float>> centers(40, std::vector<float>(dimension));
    for (auto& c : centers)
        for (auto& v : c) v = 3.0f * real(centers_engine);
    VectorStore store(dimension, DistanceKind::Cosine);
    std::vector<std::uint32_t> bucket(count);  // Uniform 0..99, independent of geometry.
    for (std::size_t i = 0; i < count; ++i) {
        std::vector<float> row(dimension);
        const auto& c = centers[engine() % centers.size()];
        for (std::size_t d = 0; d < dimension; ++d) row[d] = c[d] + real(engine);
        store.insert(static_cast<VectorId>(i), row);
        bucket[i] = static_cast<std::uint32_t>(engine() % 100);
    }
    HnswIndex index(store);
    for (VectorId id : store.ids()) index.insert(id);
    std::vector<std::vector<float>> queries(200, std::vector<float>(dimension));
    for (auto& q : queries) {
        const auto& c = centers[engine() % centers.size()];
        for (std::size_t d = 0; d < dimension; ++d) q[d] = c[d] + real(engine);
    }
    for (std::uint32_t percent : {1u, 10u, 50u}) {
        std::vector<VectorId> allowed;
        for (std::size_t i = 0; i < count; ++i)
            if (bucket[i] < percent) allowed.push_back(static_cast<VectorId>(i));
        const std::unordered_set<VectorId> allowed_set(allowed.begin(), allowed.end());
        const auto allow = [&](VectorId id) { return allowed_set.contains(id); };
        double graph_hits = 0;
        for (const auto& q : queries) {
            const PreparedQuery prepared(q, dimension, DistanceKind::Cosine);
            const auto exact = bruteForceSearchSubset(store, prepared, k, allowed);
            // The exact path equals a full brute force followed by filtering.
            auto full = bruteForceSearch(store, q, count);
            full.erase(std::remove_if(full.begin(), full.end(),
                                      [&](const SearchResult& r) { return !allow(r.id); }),
                       full.end());
            full.resize(std::min(full.size(), k));
            require(exact.size() == full.size(), "Exact filtered count incorrect.");
            for (std::size_t i = 0; i < exact.size(); ++i)
                require(exact[i].id == full[i].id, "Exact filtered ranking incorrect.");
            const auto graph = index.search_filtered(prepared, k, 64, allow);
            std::unordered_set<VectorId> truth;
            for (const auto& r : exact) truth.insert(r.id);
            for (const auto& r : graph) {
                require(allow(r.id), "Filtered graph returned a non-matching vector.");
                graph_hits += truth.contains(r.id);
            }
            require(graph.size() == std::min(k, allowed.size()),
                    "Filtered graph returned too few.");
        }
        const double recall = graph_hits / (static_cast<double>(queries.size()) * k);
        std::cout << "selectivity " << percent << "% graph recall@10=" << recall << '\n';
        if (percent >= 10) require(recall >= 0.95, "Filtered graph recall below 0.95.");
    }
}

// Filtered BM25: every strategy equals filtering the exhaustive full ranking.
void check_lexical() {
    std::mt19937_64 engine(12);
    std::vector<std::string> docs(3000);
    for (auto& doc : docs)
        for (std::size_t t = 0, n = 5 + engine() % 40; t < n; ++t)
            doc += " w" + std::to_string(engine() % (1 + engine() % 400));
    const auto allow = [](DocumentId id) { return id % 7 == 0 || id % 11 == 3; };
    for (auto strategy : {lexical::Bm25Strategy::Exhaustive, lexical::Bm25Strategy::TermAtATime,
                          lexical::Bm25Strategy::Wand, lexical::Bm25Strategy::BlockMaxWand,
                          lexical::Bm25Strategy::MaxScore}) {
        lexical::Bm25Index reference, variant({1.2, 0.75, strategy});
        for (std::size_t i = 0; i < docs.size(); ++i) {
            reference.add_document(static_cast<DocumentId>(i), docs[i]);
            variant.add_document(static_cast<DocumentId>(i), docs[i]);
        }
        reference.build();
        variant.build();
        for (std::size_t q = 0; q < 300; ++q) {
            const std::string query =
                "w" + std::to_string(engine() % 50) + " w" + std::to_string(engine() % 300);
            auto expected = reference.search(query, docs.size());
            expected.erase(
                std::remove_if(expected.begin(), expected.end(),
                               [&](const RankedResult& r) { return !allow(r.document_id); }),
                expected.end());
            expected.resize(std::min<std::size_t>(expected.size(), 10));
            const auto actual = variant.search(query, 10, allow);
            require(actual.size() == expected.size(), "Filtered BM25 count differs.");
            for (std::size_t i = 0; i < actual.size(); ++i)
                require(actual[i].document_id == expected[i].document_id &&
                            actual[i].score == expected[i].score,
                        "Filtered BM25 differs from filtered exhaustive ranking.");
        }
    }
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

// Service level: hybrid never fuses a non-matching document; strategy selection;
// unknown fields and non-JSONL inputs are rejected.
void check_service(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    const auto corpus = folder / "corpus.jsonl", queries = folder / "queries.jsonl";
    std::string text;
    std::mt19937_64 engine(5);
    for (int i = 0; i < 3000; ++i) {
        const char* tenant = i % 3 == 0 ? "acme" : i % 3 == 1 ? "globex" : "initech";
        text += "{\"document_id\":" + std::to_string(i) + ",\"text\":\"motor bomba w" +
                std::to_string(i % 97) + "\",\"metadata\":{\"tenant\":\"" + tenant +
                "\",\"published\":\"2026-" + (i % 2 ? "01" : "06") +
                "-15\",\"rank\":" + std::to_string(i % 5) + "},\"embedding\":[" +
                std::to_string(real(engine)) + "," + std::to_string(real(engine)) + "," +
                std::to_string(1 + real(engine)) + "]}\n";
    }
    write(corpus, text);
    write(queries, "{\"query_id\":\"Q1\",\"query_text\":\"motor w5\",\"embedding\":[0.1,0.2,1]}\n");
    const SearchService service;
    SearchRequest request{corpus, queries};
    request.mode = SearchMode::Hybrid;
    request.k = 20;
    request.filters = {parse_equality_filter("tenant=acme|initech"),
                       parse_date_filter("published=2026-06-01..2026-06-30")};
    const auto response = service.search(request);
    require(response.filtered && response.filter_population == 3000, "Filter metadata missing.");
    require(response.filter_matches == 1000 && response.exact_filter,
            "Expected 1000 matches (acme/initech in June) on the exact path.");
    require(!response.fused.empty(), "Filtered hybrid returned nothing.");
    for (const auto& row : response.fused) {
        const int id = std::stoi(row.source_id);
        require(id % 2 == 0 && id % 3 != 1, "Hybrid returned a document outside the filter.");
    }
    // Numbers in metadata are compared as text; a broad filter takes the graph path.
    request.filters = {parse_equality_filter("rank=0|1|2|3")};
    const auto broad = service.search(request);
    require(broad.filter_matches == 2400 && !broad.exact_filter, "Expected the graph strategy.");
    for (const auto& row : broad.fused)
        require(std::stoi(row.source_id) % 5 != 4, "rank=4 returned.");
    // Lexical and vector JSONL modes honor the same filter.
    request.mode = SearchMode::Lexical;
    request.filters = {parse_equality_filter("tenant=globex")};
    for (const auto& row : service.search(request).documents)
        require(row.id % 3 == 1, "Lexical returned a document outside the filter.");
    request.mode = SearchMode::Vector;
    for (auto algorithm : {SearchAlgorithm::BruteForce, SearchAlgorithm::Hnsw}) {
        request.algorithm = algorithm;
        const auto vector = service.search(request);
        require(vector.neighbors.size() == 20, "Filtered vector search returned too few.");
        for (const auto& row : vector.neighbors)
            require(row.id % 3 == 1, "Vector search returned a document outside the filter.");
    }
    // No match: success with zero results.
    request.mode = SearchMode::Hybrid;
    request.filters = {parse_equality_filter("tenant=umbrella")};
    const auto none = service.search(request);
    require(none.filter_matches == 0 && none.fused.empty(), "Empty filter returned results.");
    request.filters = {parse_equality_filter("region=eu")};
    bool unknown = false;
    try {
        static_cast<void>(service.search(request));
    } catch (const InputDataError&) {
        unknown = true;
    }
    require(unknown, "Unknown metadata field accepted.");
    request.mode = SearchMode::Vector;
    request.dataset_path = folder / "vectors.csv";
    request.query_path = folder / "query.csv";
    request.filters = {parse_equality_filter("tenant=acme")};
    requireInvalidArgument([&] { validate_search_request(request); }, "Filter on CSV accepted.");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Pass a scratch directory.");
        check_parsing();
        check_dense();
        check_lexical();
        check_service(argv[1]);
        std::cout << "Metadata filtering checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
