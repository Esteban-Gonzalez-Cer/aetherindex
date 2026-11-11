// Sprint 2 / OKR 1 acceptance evaluator.
//
//   kr1_3 CORPUS QUERIES OUT   BM25 Hit Rate@10 over pre-labelled queries (>= 90%, >= 20 queries).
//   kr1_5 CORPUS QUERIES OUT   End-to-end hybrid queries through SearchService (all must pass).
//
// Both modes use SearchService, the same path the CLI calls. Labels are read only
// after retrieval. KR1.5 re-derives every fused ranking independently: it runs the
// standalone HNSW and BM25 searches with the same configuration, recomputes RRF
// without RrfFusion and compares IDs, component ranks and scores.
#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../test_helpers.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/search_service.hpp"

using namespace aetherindex;

namespace {

constexpr std::size_t kTopK = 10;
constexpr std::size_t kMinimumQueries = 20;
constexpr double kHitRateTarget = 0.90;
constexpr double kScoreTolerance = 1e-12;

std::ofstream open_report(const std::filesystem::path& path) {
    std::ofstream file(path);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.imbue(std::locale::classic());
    file << std::setprecision(17);
    return file;
}

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) out += (out.empty() ? "" : " ") + value;
    return out;
}

int evaluate_bm25(const std::filesystem::path& corpus, const std::filesystem::path& queries_path,
                  const std::filesystem::path& output) {
    const auto queries = detail::load_jsonl_queries(queries_path.string(), false);
    const auto documents = detail::load_jsonl_documents(corpus);
    require(queries.size() >= kMinimumQueries, "KR1.3 requires at least 20 labelled queries.");
    const SearchService service;
    auto report = open_report(output / "kr1_3_bm25.csv");
    report << "query_id,relevant_document_ids,top10_document_ids,first_relevant_rank,"
              "hit_at_1,hit_at_10,contract_ok\n";
    std::size_t hits = 0, hits_at_1 = 0;
    double reciprocal_sum = 0;
    for (const auto& query : queries) {
        require(!query.relevant_doc_ids.empty() && !query.id.empty(),
                "Every KR1.3 query needs an ID and relevance labels.");
        SearchRequest request{corpus, queries_path};
        request.mode = SearchMode::Lexical;
        request.query_id = query.id;
        request.k = kTopK;
        const auto response = service.search(request);
        // Output contract: <= K unique documents, score descending, ties by DocumentId.
        std::set<DocumentId> unique;
        bool contract = response.documents.size() <= kTopK;
        for (std::size_t i = 0; i < response.documents.size(); ++i) {
            const auto& row = response.documents[i];
            contract = contract && unique.insert(row.id).second && std::isfinite(row.score);
            if (i)
                contract = contract && (response.documents[i - 1].score > row.score ||
                                        (response.documents[i - 1].score == row.score &&
                                         response.documents[i - 1].id < row.id));
        }
        require(contract, "BM25 Top-K output contract violated.");
        const std::set<std::string> relevant(query.relevant_doc_ids.begin(),
                                             query.relevant_doc_ids.end());
        for (const auto& id : relevant)
            require(std::any_of(documents.begin(), documents.end(),
                                [&](const auto& doc) { return doc.id == id; }),
                    "A relevance label names a document absent from the corpus.");
        std::size_t first = 0;
        std::vector<std::string> top;
        for (std::size_t i = 0; i < response.documents.size(); ++i) {
            top.push_back(response.documents[i].source_id);
            if (!first && relevant.contains(response.documents[i].source_id)) first = i + 1;
        }
        hits += first != 0;
        hits_at_1 += first == 1;
        reciprocal_sum += first ? 1.0 / static_cast<double>(first) : 0.0;
        report << query.id << ',' << join(query.relevant_doc_ids) << ',' << join(top) << ','
               << first << ',' << (first == 1) << ',' << (first != 0) << ",1\n";
    }
    report.close();
    const double count = static_cast<double>(queries.size());
    const double hit_rate = static_cast<double>(hits) / count;
    const bool passed = hit_rate >= kHitRateTarget;
    auto summary = open_report(output / "kr1_3_summary.json");
    summary << "{\"kr\":\"KR1.3\",\"metric\":\"HitRate@10 (any labelled relevant document in "
               "Top-10)\",\"target\":"
            << kHitRateTarget << ",\"documents\":" << documents.size()
            << ",\"queries\":" << queries.size() << ",\"successes\":" << hits
            << ",\"hit_rate_at_10\":" << hit_rate << ",\"hit_rate_at_1\":" << hits_at_1 / count
            << ",\"mrr_at_10\":" << reciprocal_sum / count
            << ",\"bm25\":{\"k1\":1.2,\"b\":0.75},\"indexed_fields\":\"title + text\","
               "\"result\":\""
            << (passed ? "PASS" : "FAIL") << "\"}\n";
    summary.close();
    std::cout << "KR1.3 BM25 HitRate@10 = " << hits << '/' << queries.size() << " ("
              << hit_rate * 100 << "%), target >= 90%: " << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}

struct HybridCheck {
    bool ok = true;
    std::string failure;
    void expect(bool condition, const std::string& message) {
        if (!condition && ok) {
            ok = false;
            failure = message;
        }
    }
};

int evaluate_hybrid(const std::filesystem::path& corpus, const std::filesystem::path& queries_path,
                    const std::filesystem::path& output) {
    const auto queries = detail::load_jsonl_queries(queries_path.string(), true);
    const auto documents = detail::load_jsonl_documents(corpus);
    std::set<std::string> corpus_ids;
    for (const auto& doc : documents) corpus_ids.insert(doc.id);
    require(queries.size() >= kMinimumQueries, "KR1.5 requires at least 20 hybrid queries.");
    const SearchService service;
    auto report = open_report(output / "kr1_5_hybrid.csv");
    report << "query_id,dense_candidates,lexical_candidates,fused_union,results_returned,"
              "in_both_rankings,max_abs_score_error,top10_document_ids,relevant_hit_at_10,"
              "passed,failure\n";
    std::size_t passed = 0, informative_hits = 0;
    for (const auto& query : queries) {
        SearchRequest base{corpus, queries_path};
        base.query_id = query.id;
        base.k = kTopK;
        SearchRequest hybrid = base;
        hybrid.mode = SearchMode::Hybrid;
        hybrid.rrf_config = hybrid::RrfConfig{};  // KR1.5 is defined on classic RRF, k = 60.
        const std::size_t depth = effective_candidate_k(hybrid);
        HybridCheck check;
        SearchResponse fused, dense, lexical;
        try {
            fused = service.search(hybrid);
            SearchRequest dense_request = base;
            dense_request.algorithm = SearchAlgorithm::Hnsw;
            dense_request.k = depth;
            dense = service.search(dense_request);
            SearchRequest lexical_request = base;
            lexical_request.mode = SearchMode::Lexical;
            lexical_request.k = depth;
            lexical = service.search(lexical_request);
        } catch (const std::exception& error) {
            check.expect(false, std::string("exception: ") + error.what());
        }
        double max_error = 0;
        std::size_t shared = 0;
        if (check.ok) {
            // Standalone rankings keyed by source document identity.
            std::map<std::string, std::size_t> dense_rank, lexical_rank;
            for (std::size_t i = 0; i < dense.neighbor_source_ids.size(); ++i)
                dense_rank.emplace(dense.neighbor_source_ids[i], i + 1);
            for (std::size_t i = 0; i < lexical.documents.size(); ++i)
                lexical_rank.emplace(lexical.documents[i].source_id, i + 1);
            check.expect(dense_rank.size() == dense.neighbors.size(),
                         "dense ranking lacks unique document identities");
            check.expect(fused.dense_candidates == dense.neighbors.size() &&
                             fused.lexical_candidates == lexical.documents.size(),
                         "hybrid candidate counts differ from standalone retrievers");
            // Independent RRF: k_rrf=60, ranks from 1, ties by internal DocumentId.
            std::map<std::string, std::pair<double, DocumentId>> expected;
            std::map<std::string, DocumentId> internal;
            for (const auto& row : lexical.documents) internal[row.source_id] = row.id;
            for (std::size_t i = 0; i < dense.neighbors.size(); ++i)
                internal[dense.neighbor_source_ids[i]] = dense.neighbors[i].id;
            for (const auto& [id, rank] : dense_rank) expected[id].first += 1.0 / (60.0 + rank);
            for (const auto& [id, rank] : lexical_rank) expected[id].first += 1.0 / (60.0 + rank);
            for (auto& [id, value] : expected) value.second = internal.at(id);
            std::vector<std::pair<std::string, std::pair<double, DocumentId>>> order(
                expected.begin(), expected.end());
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
                return a.second.first != b.second.first ? a.second.first > b.second.first
                                                        : a.second.second < b.second.second;
            });
            check.expect(fused.fused_candidates == order.size(), "fused union size incorrect");
            check.expect(fused.fused.size() == std::min(kTopK, order.size()),
                         "fused Top-K size is not min(k, union)");
            check.expect(!fused.fused.empty(), "hybrid query returned no documents");
            std::set<std::string> seen;
            for (std::size_t i = 0; check.ok && i < fused.fused.size(); ++i) {
                const auto& row = fused.fused[i];
                check.expect(seen.insert(row.source_id).second, "duplicate fused document");
                check.expect(corpus_ids.contains(row.source_id),
                             "fused ID does not resolve to a corpus document");
                check.expect(row.source_id == order[i].first, "fused order differs from reference");
                const auto d = dense_rank.find(row.source_id);
                const auto l = lexical_rank.find(row.source_id);
                check.expect(row.dense_rank == (d == dense_rank.end() ? 0 : d->second) &&
                                 row.lexical_rank == (l == lexical_rank.end() ? 0 : l->second),
                             "component ranks were not preserved");
                const double error = std::abs(row.rrf_score - order[i].second.first);
                max_error = std::max(max_error, error);
                check.expect(error <= kScoreTolerance, "RRF score differs from reference");
                if (i) check.expect(fused.fused[i - 1].rrf_score >= row.rrf_score, "not sorted");
                shared += row.dense_rank && row.lexical_rank;
            }
        }
        std::vector<std::string> top;
        bool relevant_hit = false;
        for (const auto& row : fused.fused) {
            top.push_back(row.source_id);
            relevant_hit = relevant_hit ||
                           std::find(query.relevant_doc_ids.begin(), query.relevant_doc_ids.end(),
                                     row.source_id) != query.relevant_doc_ids.end();
        }
        passed += check.ok;
        informative_hits += relevant_hit;
        report << query.id << ',' << fused.dense_candidates << ',' << fused.lexical_candidates << ','
               << fused.fused_candidates << ',' << fused.fused.size() << ',' << shared << ','
               << max_error << ',' << join(top) << ',' << relevant_hit << ',' << check.ok << ','
               << check.failure << '\n';
    }
    report.close();
    const bool success = passed == queries.size();
    auto summary = open_report(output / "kr1_5_summary.json");
    summary << "{\"kr\":\"KR1.5\",\"metric\":\"successful end-to-end hybrid queries\","
               "\"target\":\"all valid queries (>= 20)\",\"documents\":"
            << documents.size() << ",\"queries\":" << queries.size() << ",\"passed\":" << passed
            << ",\"k\":" << kTopK << ",\"k_rrf\":60,\"absolute_score_tolerance\":" << kScoreTolerance
            << ",\"informative_relevant_hit_rate_at_10\":"
            << static_cast<double>(informative_hits) / queries.size()
            << ",\"note\":\"relevance hit rate is informative only; embeddings are a non-semantic "
               "hashing baseline\",\"result\":\""
            << (success ? "PASS" : "FAIL") << "\"}\n";
    summary.close();
    std::cout << "KR1.5 hybrid end-to-end = " << passed << '/' << queries.size() << ": "
              << (success ? "PASS" : "FAIL") << '\n';
    return success ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 5, "Usage: kr1_3|kr1_5 CORPUS QUERIES OUTPUT_DIR");
        const std::string mode = argv[1];
        const std::filesystem::path output(argv[4]);
        std::filesystem::create_directories(output);
        if (mode == "kr1_3") return evaluate_bm25(argv[2], argv[3], output);
        if (mode == "kr1_5") return evaluate_hybrid(argv[2], argv[3], output);
        throw std::runtime_error("Unknown mode: " + mode);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
