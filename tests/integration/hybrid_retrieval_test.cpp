// CP-04 / DoD level 2: hybrid QueryEngine and SearchService scenarios.
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string_view>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/application/search_service.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/lexical/bm25.hpp"

using namespace aetherindex;

namespace {

void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << text;
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

void require_exact(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) <= 1e-12, message);
}

// VectorIds 7, 8, 9 are bound to documents 101, 102, 103; document 104 is text-only.
// Dense order is 101, 102, 103 and BM25 order for "alpha" is 102, 104, 101, which is
// the mathematical reference case of KR1.4 reached through the engine.
struct Fixture {
    VectorStore store{2, DistanceKind::Cosine};
    std::unique_ptr<HnswIndex> dense;
    lexical::Bm25Index lexical;
    VectorDocumentBinding binding{{7, 101}, {8, 102}, {9, 103}};
    hybrid::RrfFusion fusion;

    Fixture() {
        const std::array<std::pair<VectorId, std::array<float, 2>>, 3> rows{
            {{7, {1.0f, 0.0f}}, {8, {1.0f, 0.5f}}, {9, {1.0f, 1.5f}}}};
        for (const auto& [id, values] : rows)
            require(store.insert(id, values) == InsertStatus::Success, "fixture insert failed");
        dense = std::make_unique<HnswIndex>(store, HnswConfig{});
        for (VectorId id : store.ids()) dense->insert(id);
        lexical.add_document(101, "alpha beta gamma delta");
        lexical.add_document(102, "alpha alpha");
        lexical.add_document(103, "beta");
        lexical.add_document(104, "alpha beta");
        lexical.build();
    }

    HybridRankings run(std::string_view text, std::span<const float> embedding, std::size_t k,
                       std::size_t candidates) const {
        return QueryEngine{}.search(*dense, binding, lexical, fusion,
                                    HybridQuery{text, embedding, k, candidates});
    }
};

void engine_scenarios() {
    const Fixture fixture;
    const std::array<float, 2> query{1.0f, 0.0f};

    // Shared documents receive both contributions and appear exactly once.
    const auto reference = fixture.run("alpha", query, 10, 10);
    require(reference.dense.size() == 3 && reference.dense[0].document_id == 101 &&
                reference.dense[1].document_id == 102 && reference.dense[2].document_id == 103,
            "Dense ranking was not resolved through the binding in distance order.");
    require(reference.dense[0].distance <= reference.dense[1].distance &&
                reference.dense[1].distance <= reference.dense[2].distance,
            "Dense ranking is not ordered by increasing distance.");
    require(reference.lexical.size() == 3 && reference.lexical[0].document_id == 102 &&
                reference.lexical[1].document_id == 104 && reference.lexical[2].document_id == 101,
            "Lexical ranking order was not preserved.");
    const std::array<DocumentId, 4> order{102, 101, 104, 103};
    require(reference.fused.size() == order.size(), "Fused union has the wrong size.");
    for (std::size_t i = 0; i < order.size(); ++i)
        require(reference.fused[i].document_id == order[i], "Reference fused order incorrect.");
    require_exact(reference.fused[0].score, 1.0 / 62 + 1.0 / 61, "RRF(102) incorrect.");
    require_exact(reference.fused[1].score, 1.0 / 61 + 1.0 / 63, "RRF(101) incorrect.");
    require_exact(reference.fused[2].score, 1.0 / 62, "RRF(104) incorrect.");
    require_exact(reference.fused[3].score, 1.0 / 63, "RRF(103) incorrect.");

    // Top-K truncation keeps the best fused documents.
    const auto top2 = fixture.run("alpha", query, 2, 10);
    require(top2.fused.size() == 2 && top2.fused[0].document_id == 102 &&
                top2.fused[1].document_id == 101,
            "Fused topK was not respected.");

    // No lexical matches: BM25 is empty and the dense candidates are preserved.
    const auto unmatched = fixture.run("zzz", query, 10, 10);
    require(unmatched.lexical.empty() && unmatched.fused.size() == 3, "Empty BM25 mishandled.");
    for (std::size_t i = 0; i < 3; ++i) {
        require(unmatched.fused[i].document_id == unmatched.dense[i].document_id,
                "Dense order lost with an empty lexical ranking.");
        require_exact(unmatched.fused[i].score, 1.0 / (61.0 + i), "Dense-only RRF incorrect.");
    }

    // K larger than the available documents: no manufactured results.
    require(fixture.run("alpha", query, 50, 50).fused.size() == 4, "Results were padded.");

    // Invalid arguments and vectors fail cleanly; the indexes remain usable afterwards.
    const std::array<float, 3> wrong{1.0f, 0.0f, 0.0f};
    require_throws<std::invalid_argument>([&] { fixture.run("alpha", wrong, 2, 10); },
                                          "Wrong query dimension was accepted.");
    require_throws<std::invalid_argument>([&] { fixture.run("alpha", query, 0, 10); },
                                          "k=0 was accepted.");
    require_throws<std::invalid_argument>([&] { fixture.run("alpha", query, 5, 4); },
                                          "candidate_k < k was accepted.");
    const auto again = fixture.run("alpha", query, 10, 10);
    require(again.fused.size() == reference.fused.size(), "Index state changed after errors.");
    for (std::size_t i = 0; i < again.fused.size(); ++i)
        require(again.fused[i].document_id == reference.fused[i].document_id &&
                    again.fused[i].score == reference.fused[i].score,
                "Hybrid search is not repeatable after error cases.");

    // A dense hit without a document binding is a release-blocking identity defect.
    const VectorDocumentBinding partial{{7, 101}, {8, 102}};
    require_throws<std::logic_error>(
        [&] {
            QueryEngine{}.search(*fixture.dense, partial, fixture.lexical, fixture.fusion,
                                 HybridQuery{"alpha", query, 3, 3});
        },
        "Unbound VectorId was silently accepted.");
    // A non-injective binding would merge two vectors into one document.
    const VectorDocumentBinding collapsed{{7, 101}, {8, 101}, {9, 103}};
    require_throws<std::invalid_argument>(
        [&] {
            QueryEngine{}.search(*fixture.dense, collapsed, fixture.lexical, fixture.fusion,
                                 HybridQuery{"alpha", query, 3, 3});
        },
        "Non-injective binding produced a duplicated dense DocumentId.");
}

void service_scenarios(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    const auto corpus = folder / "corpus.jsonl";
    const auto queries = folder / "queries.jsonl";
    // Opaque IDs listed out of order: the DocumentId mapping must not follow file rows.
    write(corpus,
          "{\"document_id\":\"D3\",\"title\":\"bomba\",\"text\":\"bomba presion\","
          "\"embedding\":[0,1]}\n"
          "{\"document_id\":\"D1\",\"title\":\"motor\",\"text\":\"motor e47 temperatura\","
          "\"embedding\":[1,0]}\n"
          "{\"document_id\":\"D2\",\"title\":\"sensor\",\"text\":\"sensor e47\","
          "\"embedding\":[0.6,0.8]}\n");
    write(queries,
          "{\"query_id\":\"Q1\",\"query_text\":\"e47\",\"embedding\":[1,0],"
          "\"relevant_document_ids\":[\"D1\"]}\n"
          "{\"query_id\":\"Q2\",\"query_text\":\"zzz\",\"embedding\":[0,1]}\n"
          "{\"query_id\":\"QBAD\",\"query_text\":\"e47\",\"embedding\":[1,0,0]}\n"
          "{\"query_id\":\"QNOVEC\",\"query_text\":\"e47\"}\n"
          "{\"query_id\":\"QNOTEXT\",\"embedding\":[1,0]}\n");
    const SearchService service;
    SearchRequest request{corpus, queries};
    request.mode = SearchMode::Hybrid;
    request.query_id = "Q1";
    request.k = 10;
    // Default fusion: convex, 0.65 * dense + 0.35 * BM25 after min-max normalization. Dense
    // similarities (1 - cosine distance) are D1 1, D2 0.6, D3 0; BM25 ranks the shorter D2
    // above D1, so the normalized lexical scores are D2 1 and D1 0.
    require(request.rrf_config.method == hybrid::FusionMethod::Convex &&
                request.rrf_config.alpha == 0.65,
            "Hybrid search default is not convex fusion with alpha 0.65.");
    const auto convex = service.search(request);
    require(convex.fused.size() == 3 && convex.fused[0].source_id == "D2" &&
                convex.fused[1].source_id == "D1" && convex.fused[2].source_id == "D3",
            "Default convex fusion order incorrect.");
    require(std::abs(convex.fused[0].rrf_score - (0.65 * 0.6 + 0.35)) < 1e-6 &&
                convex.fused[1].rrf_score == 0.65 && convex.fused[2].rrf_score == 0.0,
            "Default convex fusion scores incorrect.");
    // The rest of the scenario checks RRF accounting.
    request.rrf_config = hybrid::RrfConfig{};
    const auto hybrid = service.search(request);
    require(hybrid.documents_indexed == 3 && hybrid.dimension == 2 && hybrid.candidate_k == 50,
            "Hybrid metadata incorrect.");
    require(hybrid.dense_candidates == 3 && hybrid.lexical_candidates == 2 &&
                hybrid.fused_candidates == 3 && hybrid.fused.size() == 3,
            "Hybrid candidate accounting incorrect.");
    require(hybrid.fused[0].source_id == "D1" && hybrid.fused[0].dense_rank == 1 &&
                hybrid.fused[0].lexical_rank != 0,
            "Shared document was not ranked first with both contributions.");

    // Identity invariant: vector, lexical and hybrid paths agree on DocumentIds.
    SearchRequest vector = request;
    vector.mode = SearchMode::Vector;
    vector.algorithm = SearchAlgorithm::BruteForce;
    const auto dense = service.search(vector);
    SearchRequest text = request;
    text.mode = SearchMode::Lexical;
    const auto lexical = service.search(text);
    std::map<std::string, DocumentId> ids;
    for (const auto& row : lexical.documents) ids.emplace(row.source_id, row.id);
    require(dense.neighbor_source_ids.size() == dense.neighbors.size(), "Dense source IDs lost.");
    for (std::size_t i = 0; i < dense.neighbors.size(); ++i) {
        const auto found = ids.find(dense.neighbor_source_ids[i]);
        if (found != ids.end())
            require(found->second == dense.neighbors[i].id,
                    "Dense VectorId and lexical DocumentId disagree for one document.");
    }
    for (const auto& row : hybrid.fused)
        if (ids.contains(row.source_id))
            require(ids.at(row.source_id) == row.id, "Hybrid DocumentId differs from BM25.");

    request.query_id = "Q2";
    const auto unmatched = service.search(request);
    require(unmatched.lexical_candidates == 0 && unmatched.fused.size() == 3 &&
                unmatched.fused[0].source_id == "D3" && unmatched.fused[0].lexical_rank == 0,
            "Hybrid query without lexical matches did not keep dense candidates.");

    for (const char* id : {"QBAD", "QNOVEC", "QNOTEXT", "MISSING"}) {
        request.query_id = id;
        require_throws<InputDataError>([&] { static_cast<void>(service.search(request)); },
                                       "Invalid hybrid query was accepted.");
    }
    request.query_id = "Q1";
    SearchRequest missing = request;
    missing.dataset_path = folder / "absent.jsonl";
    require_throws<InputDataError>([&] { static_cast<void>(service.search(missing)); },
                                   "Missing corpus was accepted.");
    const auto malformed = folder / "malformed.jsonl";
    write(malformed, "{\"document_id\":\"D1\",\"text\":\"motor\",\"embedding\":[1,0]\n");
    missing.dataset_path = malformed;
    require_throws<InputDataError>([&] { static_cast<void>(service.search(missing)); },
                                   "Malformed corpus was accepted.");
    const auto textual = folder / "textual.jsonl";
    write(textual, "{\"document_id\":\"D1\",\"text\":\"motor\",\"embedding\":null}\n");
    missing.dataset_path = textual;
    require_throws<InputDataError>([&] { static_cast<void>(service.search(missing)); },
                                   "Hybrid search fabricated a missing document embedding.");

    SearchRequest invalid = request;
    invalid.candidate_k = 5;
    require_throws<std::invalid_argument>([&] { static_cast<void>(service.search(invalid)); },
                                          "candidate_k < k was accepted by the service.");
    invalid = request;
    invalid.rrf_config.k_rrf = 0;
    require_throws<std::invalid_argument>([&] { static_cast<void>(service.search(invalid)); },
                                          "k_rrf=0 was accepted.");
    invalid = request;
    invalid.query_text = "e47";
    require_throws<std::invalid_argument>([&] { static_cast<void>(service.search(invalid)); },
                                          "Hybrid accepted text without a query embedding.");
    invalid = request;
    invalid.mode = SearchMode::Lexical;
    invalid.candidate_k = 20;
    require_throws<std::invalid_argument>([&] { static_cast<void>(service.search(invalid)); },
                                          "candidate_k was accepted outside hybrid mode.");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Pass a fixture output directory.");
        engine_scenarios();
        service_scenarios(argv[1]);
        std::cout << "Hybrid QueryEngine and SearchService scenarios passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
