// Audit-only consumer of the public libraries. No product implementation is changed.
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/hybrid/rrf.hpp"
#include "aether/lexical/bm25.hpp"

using namespace aetherindex;

namespace {
void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

std::string json_quote(std::string_view text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 0x20) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(c);
        else out << static_cast<char>(c);
    }
    out << '"';
    return out.str();
}

template <class Error, class Function>
void rejected(Function function, const char* message) {
    try { function(); }
    catch (const Error&) { return; }
    throw std::runtime_error(message);
}

void binding_case() {
    VectorStore store(2);
    const std::array<std::pair<VectorId, std::array<float, 2>>, 4> rows = {{
        {9010, {1, 0}}, {7000, {1, 0.5f}}, {42, {-1, 0}}, {11, {0, 1}}
    }};
    for (const auto& [id, vector] : rows)
        check(store.insert(id, vector) == InsertStatus::Success, "Binding fixture insert failed.");
    HnswIndex dense(store);
    for (VectorId id : store.ids()) dense.insert(id);
    lexical::Bm25Index lexical;
    lexical.add_document(101, "alpha beta gamma delta");
    lexical.add_document(102, "alpha alpha");
    lexical.add_document(103, "beta");
    lexical.add_document(104, "alpha beta");
    lexical.build();
    const VectorDocumentBinding binding{{9010, 101}, {7000, 102}, {42, 103}, {11, 104}};
    const std::array<float, 2> query{1, 0};
    const QueryEngine engine;
    const hybrid::RrfFusion fusion;
    const auto raw = engine.search(dense, query, 50);
    const auto result = engine.search(dense, binding, lexical, fusion, HybridQuery{"alpha", query, 10, 50});
    check(raw.size() == 4 && result.dense.size() == 4, "Missing fixture dense hits.");
    for (std::size_t i = 0; i < raw.size(); ++i)
        check(result.dense[i].document_id == binding.at(raw[i].id), "VectorId was treated as DocumentId.");
    const std::array<DocumentId, 4> expected{102, 101, 104, 103};
    check(result.fused.size() == expected.size(), "Wrong binding fixture union size.");
    for (std::size_t i = 0; i < expected.size(); ++i)
        check(result.fused[i].document_id == expected[i], "Wrong mapped fused order.");
    auto missing = binding;
    missing.erase(42);
    rejected<std::logic_error>([&] {
        static_cast<void>(engine.search(dense, missing, lexical, fusion, HybridQuery{"alpha", query, 10, 50}));
    }, "Missing binding accepted.");
    auto collapsed = binding;
    collapsed[7000] = 101;
    rejected<std::invalid_argument>([&] {
        static_cast<void>(engine.search(dense, collapsed, lexical, fusion, HybridQuery{"alpha", query, 10, 50}));
    }, "Non-injective binding accepted.");
    std::cout << "{\"check\":\"1.5\",\"passed\":true,\"corpus_documents\":4,\"dense_vectors\":4,"
                 "\"explicit_bindings\":[[9010,101],[7000,102],[42,103],[11,104]],"
                 "\"fused_document_ids\":[102,101,104,103],\"missing_binding_rejected\":true,"
                 "\"non_injective_binding_rejected\":true}\n";
}

void corpus_case(const std::string& corpus, const std::string& queries_path) {
    const auto [store, docs] = detail::load_jsonl_corpus(corpus, DistanceKind::Cosine);
    const auto queries = detail::load_jsonl_queries(queries_path);
    check(!docs.empty() && !queries.empty(), "Empty audit corpus or queries.");
    HnswIndex dense(*store);
    for (VectorId id : store->ids()) dense.insert(id);
    lexical::Bm25Index lexical;
    VectorDocumentBinding binding;
    std::map<DocumentId, std::string> identities;
    std::set<std::string> sources;
    for (const auto& doc : docs) {
        check(binding.emplace(doc.vector_id, doc.document_id).second, "Duplicate VectorId binding.");
        check(identities.emplace(doc.document_id, doc.id).second, "Duplicate DocumentId.");
        check(sources.insert(doc.id).second, "Duplicate source ID.");
        lexical.add_document(doc.document_id, doc.title + "\n" + doc.text);
    }
    lexical.build();
    check(store->size() == docs.size() && dense.size() == docs.size() && lexical.document_count() == docs.size(),
          "The retrievers do not index the same document count.");
    for (const auto& doc : docs) {
        check(store->contains(doc.vector_id) && dense.contains(doc.vector_id), "Document missing from dense corpus.");
        check(lexical.inverted_index().document_length(doc.document_id).has_value(), "Document missing from lexical corpus.");
    }
    const QueryEngine engine;
    const hybrid::RrfFusion fusion;
    constexpr std::size_t candidates = 50, final_k = 10;
    double maximum_error = 0;
    std::cout << "{\n\"checks\":[\"1.4\",\"1.5\",\"1.6\",\"1.7\",\"1.8\"],\n"
              << "\"documents\":" << docs.size() << ",\"dense_vectors\":" << store->size()
              << ",\"lexical_documents\":" << lexical.document_count() << ",\"dimension\":" << store->dimension()
              << ",\"candidate_k\":50,\"final_k\":10,\"k_rrf\":60,\n\"bindings\":[";
    bool first = true;
    for (const auto& doc : docs) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"vector_id\":" << doc.vector_id << ",\"document_id\":" << doc.document_id
                  << ",\"source_id\":" << json_quote(doc.id) << '}';
    }
    std::cout << "],\n\"queries\":[\n";
    bool first_query = true;
    for (const auto& query : queries) {
        check(query.has_text && query.vector.size() == store->dimension(), "Invalid audit query.");
        const auto independent_dense = engine.search(dense, query.vector, candidates);
        const auto independent_lexical = engine.search(lexical, query.text, candidates);
        const auto received = engine.search(dense, binding, lexical, fusion,
                                            HybridQuery{query.text, query.vector, candidates, candidates});
        const auto top = engine.search(dense, binding, lexical, fusion,
                                       HybridQuery{query.text, query.vector, final_k, candidates});
        check(received.dense.size() == independent_dense.size(), "Dense candidate count changed before fusion.");
        check(received.lexical.size() == independent_lexical.size(), "Lexical candidate count changed before fusion.");
        std::set<DocumentId> union_ids;
        for (std::size_t i = 0; i < independent_dense.size(); ++i) {
            check(received.dense[i].document_id == binding.at(independent_dense[i].id) &&
                      received.dense[i].distance == independent_dense[i].distance, "Dense rank changed before fusion.");
            check(identities.contains(received.dense[i].document_id), "Unknown dense document.");
            union_ids.insert(received.dense[i].document_id);
        }
        for (std::size_t i = 0; i < independent_lexical.size(); ++i) {
            check(received.lexical[i].document_id == independent_lexical[i].document_id &&
                      received.lexical[i].score == independent_lexical[i].score, "Lexical rank changed before fusion.");
            check(identities.contains(received.lexical[i].document_id), "Unknown lexical document.");
            union_ids.insert(received.lexical[i].document_id);
        }
        // Independent reference: enumerate the union and scan the original ranks;
        // no fusion implementation or scoring helper is called for these scores.
        std::vector<RankedResult> expected;
        for (DocumentId id : union_ids) {
            double score = 0;
            const auto d = std::find_if(received.dense.begin(), received.dense.end(), [&](const auto& hit) { return hit.document_id == id; });
            const auto l = std::find_if(independent_lexical.begin(), independent_lexical.end(), [&](const auto& hit) { return hit.document_id == id; });
            if (d != received.dense.end()) score += 1.0 / (61.0 + std::distance(received.dense.begin(), d));
            if (l != independent_lexical.end()) score += 1.0 / (61.0 + std::distance(independent_lexical.begin(), l));
            expected.push_back({id, score});
        }
        std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
            return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
        });
        check(received.fused.size() == expected.size(), "Fusion union lost or added documents.");
        check(top.fused.size() == std::min(final_k, expected.size()), "Final Top-K count is wrong.");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto& actual = received.fused[i];
            check(actual.document_id == expected[i].document_id && identities.contains(actual.document_id), "Fused order/identity incorrect.");
            const double error = std::abs(actual.score - expected[i].score);
            check(std::isfinite(actual.score) && error <= 1e-12, "RRF reference formula disagrees.");
            maximum_error = std::max(maximum_error, error);
            if (i < top.fused.size())
                check(top.fused[i].document_id == actual.document_id && top.fused[i].score == actual.score, "Top-K is not an ordered prefix.");
        }
        if (!first_query) std::cout << ",\n";
        first_query = false;
        std::cout << "{\"query_id\":" << json_quote(query.id) << ",\"dense_independent_and_to_rrf\":[";
        for (std::size_t i = 0; i < received.dense.size(); ++i) {
            if (i) std::cout << ',';
            const auto& row = received.dense[i];
            std::cout << "{\"rank\":" << i+1 << ",\"vector_id\":" << independent_dense[i].id
                      << ",\"document_id\":" << row.document_id << ",\"source_id\":" << json_quote(identities.at(row.document_id))
                      << ",\"distance\":" << row.distance << '}';
        }
        std::cout << "],\"lexical_independent_and_to_rrf\":[";
        for (std::size_t i = 0; i < received.lexical.size(); ++i) {
            if (i) std::cout << ',';
            const auto& row = received.lexical[i];
            std::cout << "{\"rank\":" << i+1 << ",\"document_id\":" << row.document_id
                      << ",\"source_id\":" << json_quote(identities.at(row.document_id)) << ",\"score\":" << row.score << '}';
        }
        std::cout << "],\"fused_union\":[";
        for (std::size_t i = 0; i < received.fused.size(); ++i) {
            if (i) std::cout << ',';
            const auto& row = received.fused[i];
            std::cout << "{\"rank\":" << i+1 << ",\"document_id\":" << row.document_id
                      << ",\"source_id\":" << json_quote(identities.at(row.document_id))
                      << ",\"score\":" << row.score << ",\"expected_score\":" << expected[i].score
                      << ",\"in_final_top_k\":" << (i < final_k ? "true" : "false") << '}';
        }
        std::cout << "],\"independent_rankings_preserved\":true,\"ordered\":true,\"all_documents_exist\":true}";
    }
    std::cout << "\n],\n\"queries_passed\":" << queries.size() << ",\"max_absolute_rrf_error\":" << maximum_error
              << ",\"absolute_tolerance\":1e-12,\"same_corpus\":true,\"passed\":true}\n";
}
}  // namespace

int main(int argc, char** argv) {
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(17);
    try {
        if (argc == 2 && std::string_view(argv[1]) == "bindings") binding_case();
        else if (argc == 4 && std::string_view(argv[1]) == "corpus") corpus_case(argv[2], argv[3]);
        else throw std::invalid_argument("Usage: probe bindings | probe corpus CORPUS.jsonl QUERIES.jsonl");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
