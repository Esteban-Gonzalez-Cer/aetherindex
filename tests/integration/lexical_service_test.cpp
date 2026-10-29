#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <unordered_map>

#include "../test_helpers.hpp"
#include "aether/application/json_loader.hpp"
#include "aether/application/search_service.hpp"

using namespace aetherindex;

namespace {
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output << text;
    output.close();
}

template <class Function>
void input_error(Function function, std::string_view fragment) {
    try {
        function();
    } catch (const InputDataError& error) {
        require(std::string_view(error.what()).find(fragment) != std::string_view::npos,
                "Missing expected data diagnostic.");
        return;
    }
    throw std::runtime_error("Expected an input-data error.");
}

void same(const SearchResponse& first, const SearchResponse& second) {
    require(first.documents.size() == second.documents.size(), "Result count changed.");
    for (std::size_t i = 0; i < first.documents.size(); ++i) {
        require(first.documents[i].source_id == second.documents[i].source_id &&
                    first.documents[i].id == second.documents[i].id &&
                    first.documents[i].score == second.documents[i].score,
                "Reordering documents or changing labels changed retrieval.");
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Pass an isolated test directory.");
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        SearchRequest request{root / "documents.JSONL", root / "queries.JSONL"};
        request.mode = SearchMode::Lexical;
        request.k = 100;
        const SearchService service;
        write(
            request.dataset_path,
            "{\"document_id\":101,\"title\":\"\",\"text\":\"radio radio "
            "dish\",\"embedding\":null}\n"
            "{\"document_id\":102,\"text\":\"radio wave\"}\n"
            "{\"document_id\":103,\"text\":\"mirror\",\"embedding\":\"unused in lexical mode\"}\n");
        write(request.query_path,
              "{\"query_id\":\"Q1\",\"query_text\":\"radio\",\"relevant_document_ids\":[103]}\n");
        const auto response = service.search(request);
        require(response.documents_indexed == 3 && response.vectors_indexed == 0 &&
                    response.neighbors.empty() && response.documents.size() == 2,
                "BM25 metadata, candidates or vector independence incorrect.");
        const double idf = std::log(1.0 + 1.5 / 2.5);
        // Independent reference: N=3, avgdl=2, DF=2, lengths 3/2/1.
        const double first = idf * (2.0 * 2.2) / (2.0 + 1.2 * (0.25 + 0.75 * 3.0 / 2.0));
        require(response.documents[0].source_id == "101" && response.documents[0].id == 101 &&
                    std::abs(response.documents[0].score - first) <= 1e-12,
                "CLI service changed BM25 reference score or numeric identity.");
        require(response.documents[1].source_id == "102" &&
                    std::abs(response.documents[1].score - idf) <= 1e-12,
                "Second reference score incorrect or labels affected ranking.");
        require(response.selected_query_id == "Q1" && response.query_time.count() >= 0 &&
                    std::isfinite(response.query_time.count()) &&
                    response.index_build_time.count() >= 0,
                "Query selection/timing metadata incorrect.");
        request.bm25_config = {.k1 = 2, .b = 0};
        const auto configured = service.search(request);
        require(std::abs(configured.documents[0].score - idf * 1.5) <= 1e-12,
                "BM25 configuration was not forwarded through QueryEngine.");
        request.bm25_config = {};
        request.k = 1;
        require(service.search(request).documents.size() == 1, "Top-K was ignored.");
        request.k = std::numeric_limits<std::size_t>::max();
        require(service.search(request).documents.size() == 2,
                "Large K must not pad results or drive allocation.");
        request.query_path.clear();
        for (const auto query : {"", "!!!", "absentterm"}) {
            request.query_text = query;
            require(service.search(request).documents.empty(),
                    "Empty/OOV query must produce no results.");
        }
        request.query_text = "TITLE_ONLY";
        const std::string a =
            "{\"document_id\":\"D002\",\"title\":\"TITLE_ONLY\",\"text\":\"same\"}\n";
        const std::string b =
            "{\"document_id\":\"D001\",\"title\":\"TITLE_ONLY\",\"text\":\"same\"}\n";
        write(request.dataset_path, a + b);
        const auto ordered = service.search(request);
        require(ordered.documents.size() == 2 && ordered.documents[0].source_id == "D001",
                "Titles must be indexed and opaque ID ties must be deterministic.");
        write(request.dataset_path, b + a);
        same(ordered, service.search(request));
        write(request.dataset_path,
              "{\"document_id\":0,\"text\":\"token\"}\n{\"document_id\":4294967295,\"text\":"
              "\"token\"}\n{\"document_id\":\"D\",\"text\":\"token\"}\n");
        request.query_text = "token";
        const auto mixed = service.search(request);
        require(mixed.documents.size() == 3 && mixed.documents[0].id == 0 &&
                    mixed.documents[1].source_id == "D" &&
                    mixed.documents[2].id == std::numeric_limits<DocumentId>::max(),
                "Opaque mapping collided with a numeric ID or lost the full uint32 range.");
        write(request.dataset_path, "{\"document_id\":\"D1\",\"text\":\"\"}\n");
        require(service.search(request).documents.empty(),
                "Empty documents are counted but never padded into results.");

        const std::pair<std::string_view, std::string_view> invalid[] = {
            {"", "empty"},
            {"not json\n", "line 1"},
            {"{\"document_id\":\"a\",\"text\":\"one\"}\n{\"document_id\":\"a\",\"text\":\"two\"}\n",
             "line 2"},
            {"{\"text\":\"missing ID\"}\n", "document_id"},
            {"{\"metadata\":{\"document_id\":\"fake\"},\"text\":\"x\"}\n", "document_id"},
            {"{\"document_id\":\"a\"}\n", "text"},
            {"{\"document_id\":\"a\",\"text\":null}\n", "text"},
            {"{\"document_id\":-1,\"text\":\"x\"}\n", "uint32"},
            {"{\"document_id\":4294967296,\"text\":\"x\"}\n", "uint32"},
            {"{\"document_id\":\"a\",\"id\":\"b\",\"text\":\"x\"}\n", "conflicting"},
            {"{\"document_id\":\"a\",\"text\":\"x\",\"vector\":[1oops]}\n", "JSON column"},
        };
        for (const auto& [text, diagnostic] : invalid) {
            write(request.dataset_path, text);
            input_error([&] { static_cast<void>(service.search(request)); }, diagnostic);
        }
        write(request.dataset_path, "{\"document_id\":\"D\",\"text\":\"radio wave\"}\n");
        request.query_text.reset();
        request.query_path = root / "queries.JSONL";
        write(request.query_path,
              "{\"query_id\":\"Q1\",\"query_text\":\"radio\"}\n{\"query_id\":\"Q2\",\"query_text\":"
              "\"wave\"}\n");
        input_error([&] { static_cast<void>(service.search(request)); }, "--query-id");
        request.query_id = "Q2";
        const auto selected = service.search(request);
        require(selected.selected_query_id == "Q2" && selected.documents[0].source_id == "D",
                "Wrong query selected.");
        write(request.query_path,
              "{\"query_id\":\"Q1\",\"query_text\":\"radio\",\"relevant_document_ids\":[\"bogus\"]}"
              "\n{\"query_id\":\"Q2\",\"query_text\":\"wave\",\"relevant_document_ids\":[]}\n");
        same(selected, service.search(request));
        request.query_id = "missing";
        input_error([&] { static_cast<void>(service.search(request)); }, "not found");
        request.query_id = "Q1";
        write(request.query_path,
              "{\"query_id\":\"Q1\",\"query_text\":\"x\"}\n{\"query_id\":\"Q1\",\"query_text\":"
              "\"y\"}\n");
        input_error([&] { static_cast<void>(service.search(request)); }, "duplicate query");
        write(request.query_path, "{\"query_id\":\"Q1\"}\n");
        input_error([&] { static_cast<void>(service.search(request)); }, "requires 'query_text'");
        request.query_path = root;
        request.query_id.reset();
        requireInvalidArgument([&] { static_cast<void>(service.search(request)); },
                               "Invalid format must fail before reading.");
        request.query_path = root / "missing.jsonl";
        input_error([&] { static_cast<void>(service.search(request)); }, "Cannot open");
        request.query_path.clear();
        request.query_text = "radio";
        request.bm25_config.b = 2;
        requireInvalidArgument([&] { static_cast<void>(service.search(request)); },
                               "Invalid BM25 config must fail before file access.");
        std::cout << "BM25 service: reference scores, titles, identities, query selection, labels, "
                     "errors and timing passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
