#include "cli_runner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <iomanip>
#include <limits>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "aether/application/search_service.hpp"
#include "cli_parser.hpp"

namespace aetherindex::cli {
namespace {

using Row = std::vector<std::string>;

std::string number(double value, int precision = 9) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::fixed << std::setprecision(precision) << value;
    return output.str();
}

// Escape bytes which could inject rows, delimiters or terminal controls. The
// application response retains the original source ID; this is presentation only.
std::string display_id(std::string_view id) {
    std::string output;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : id) {
        if (c >= 0x20 && c <= 0x7e && c != '|' && c != '\\')
            output += static_cast<char>(c);
        else {
            output += "\\x";
            output += hex[c >> 4];
            output += hex[c & 0xf];
        }
    }
    return output;
}

void print_table(std::ostream& output, const Row& headings, const std::vector<Row>& rows) {
    std::vector<std::size_t> widths;
    for (const auto& heading : headings) widths.push_back(heading.size());
    for (const auto& row : rows)
        for (std::size_t column = 0; column < widths.size(); ++column)
            widths[column] = std::max(widths[column], row[column].size());
    const auto border = [&] {
        output << '+';
        for (auto width : widths) output << std::string(width + 2, '-') << '+';
        output << '\n';
    };
    const auto print_row = [&](const Row& row, bool right) {
        output << '|';
        for (std::size_t column = 0; column < widths.size(); ++column) {
            const std::string padding(widths[column] - row[column].size(), ' ');
            output << ' ' << (right ? padding + row[column] : row[column] + padding) << " |";
        }
        output << '\n';
    };
    border();
    print_row(headings, false);
    border();
    for (const auto& row : rows) {
        print_row(row, true);
        border();
    }
}

std::string mib(std::size_t bytes) { return number(static_cast<double>(bytes) / 1048576.0, 3); }

// Printed only for quantized searches, so the default reports are unchanged.
void print_quantization(std::ostream& output, const SearchRequest& request,
                        const SearchResponse& response) {
    if (request.quantization == Quantization::None) return;
    output << "\nQuantization: " << (request.quantization == Quantization::Sq8 ? "SQ8" : "PQ")
           << " (codes " << mib(response.code_bytes) << " MiB vs FP32 " << mib(response.fp32_bytes)
           << " MiB; FP32 rows " << (response.fp32_released ? "released" : "kept")
           << ")\nRe-rank: ";
    if (response.rerank == 0)
        output << "none (approximate distances)";
    else if (response.rerank == std::numeric_limits<std::size_t>::max())
        output << "exact FP32 over all candidates";
    else
        output << "exact FP32 over " << response.rerank << " candidates";
    output << "\nQuantization time (ms): " << number(response.quantization_time.count(), 6);
}

const char* strategy_name(lexical::Bm25Strategy strategy) {
    switch (strategy) {
        case lexical::Bm25Strategy::TermAtATime:
            return "term-at-a-time (compressed)";
        case lexical::Bm25Strategy::Wand:
            return "WAND (compressed)";
        case lexical::Bm25Strategy::BlockMaxWand:
            return "Block-Max WAND (compressed)";
        case lexical::Bm25Strategy::MaxScore:
            return "MaxScore (compressed)";
        default:
            return "exhaustive";
    }
}

// Printed only for filtered searches, so the default reports are unchanged.
void print_filter(std::ostream& output, const SearchRequest& request,
                  const SearchResponse& response, bool dense) {
    if (!response.filtered) return;
    const double share = response.filter_population
                             ? 100.0 * static_cast<double>(response.filter_matches) /
                                   static_cast<double>(response.filter_population)
                             : 0.0;
    output << "\nFilter: " << request.filters.size()
           << " clause(s); matching documents: " << response.filter_matches << " of "
           << response.filter_population << " (" << number(share, 1) << "%)";
    if (dense)
        output << "\nDense filter strategy: "
               << (response.exact_filter ? "exact search over matching vectors"
                                         : "filtered HNSW traversal");
}

void print_lexical(std::ostream& output, const SearchRequest& request,
                   const SearchResponse& response) {
    output << "Search mode: Lexical\nAlgorithm: BM25\nDocuments indexed: "
           << response.documents_indexed
           << "\nIndexed fields: title + text (no field boosts)\nRequested K: " << request.k
           << "\nResults returned: " << response.documents.size()
           << "\nBM25 configuration: k1=" << std::setprecision(17) << request.bm25_config.k1
           << " b=" << request.bm25_config.b;
    if (request.bm25_config.strategy != lexical::Bm25Strategy::MaxScore)
        output << "\nBM25 strategy: " << strategy_name(request.bm25_config.strategy);
    if (request.bm25_config.analyzer != lexical::Analyzer::AsciiTechV1)
        output << "\nBM25 analyzer: " << lexical::analyzer_name(request.bm25_config.analyzer);
    if (request.bm25_config.query_weighting != lexical::QueryTermWeighting::Frequency)
        output << "\nBM25 query weighting: unique";
    print_filter(output, request, response, false);
    if (!response.selected_query_id.empty())
        output << "\nQuery ID: " << display_id(response.selected_query_id);
    output << "\nIndex build time (ms): " << number(response.index_build_time.count(), 6)
           << "\nIndex build time scope: lexical indexing (excludes input loading)"
           << "\nQuery time (ms): " << number(response.query_time.count(), 6)
           << "\nQuery time scope: BM25 search only (excludes input loading, index build and "
              "output)\n\n";
    std::vector<Row> rows;
    rows.reserve(response.documents.size());
    for (std::size_t i = 0; i < response.documents.size(); ++i)
        rows.push_back({std::to_string(i + 1), display_id(response.documents[i].source_id),
                        number(response.documents[i].score)});
    print_table(output, {"Rank", "Document ID", "BM25 Score"}, rows);
    if (rows.empty()) output << "\nNo matching documents.\n";
}

void print_vector(std::ostream& output, const SearchRequest& request,
                  const SearchResponse& response) {
    output << "Algorithm: " << (request.algorithm == SearchAlgorithm::Hnsw ? "HNSW" : "BruteForce")
           << "\nDistance metric: "
           << (request.metric == DistanceKind::Cosine ? "Cosine" : "SquaredEuclidean")
           << "\nVectors indexed: " << response.vectors_indexed
           << "\nDimensions: " << response.dimension << "\nRequested K: " << request.k
           << "\nResults returned: " << response.neighbors.size();
    if (!response.selected_query_id.empty())
        output << "\nQuery ID: " << display_id(response.selected_query_id);
    if (request.algorithm == SearchAlgorithm::Hnsw) {
        const auto& config = request.hnsw_config;
        output << "\nHNSW configuration: M=" << config.m << " M0=" << config.m_max_0
               << " efConstruction=" << config.ef_construction << " efSearch=" << config.ef_search
               << " seed=" << config.random_seed << "\nHNSW neighbor selection: "
               << (config.neighbor_selection == NeighborSelection::Heuristic
                       ? "Heuristic (Algorithm 4)"
                       : "Simple (Algorithm 3)")
               << "\nIndex build time (ms): " << number(response.index_build_time.count(), 6)
               << "\nIndex build time scope: graph construction only (excludes input loading)";
        print_quantization(output, request, response);
    }
    print_filter(output, request, response, true);
    output << "\nQuery time (ms): " << number(response.query_time.count(), 6)
           << "\nQuery time scope: search only (excludes input loading)\n\n";
    // JSONL vectors are stored under their DocumentId; show the source document identity.
    const bool documents = !response.neighbor_source_ids.empty();
    std::vector<Row> rows;
    rows.reserve(response.neighbors.size());
    for (std::size_t i = 0; i < response.neighbors.size(); ++i)
        rows.push_back({std::to_string(i + 1),
                        documents ? display_id(response.neighbor_source_ids[i])
                                  : std::to_string(response.neighbors[i].id),
                        number(response.neighbors[i].distance)});
    print_table(output, {"Rank", documents ? "Document ID" : "Vector ID", "Distance"}, rows);
}

std::string component_rank(std::size_t rank) { return rank ? std::to_string(rank) : "-"; }

// Shortest decimal that reads back as the same double (0.7, not 0.69999999999999996).
std::string shortest(double value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

void print_hybrid(std::ostream& output, const SearchRequest& request,
                  const SearchResponse& response) {
    const auto& hnsw = request.hnsw_config;
    const auto& fusion = request.rrf_config;
    const bool convex = fusion.method == hybrid::FusionMethod::Convex;
    output << "Search mode: Hybrid\nFusion: " << std::setprecision(17);
    if (convex)
        output << "convex (alpha=" << shortest(fusion.alpha) << ", min-max normalized scores)";
    else if (fusion.dense_weight)
        output << "weighted RRF (k_rrf=" << fusion.k_rrf
               << ", dense weight=" << shortest(*fusion.dense_weight) << ", lexical weight=1-dense)";
    else
        output << "RRF (k_rrf=" << fusion.k_rrf << ")";
    output << "\nDense retriever: HNSW ("
           << (request.metric == DistanceKind::Cosine ? "Cosine" : "SquaredEuclidean")
           << ")\nLexical retriever: BM25 (title + text, no field boosts)"
           << "\nDocuments indexed: " << response.documents_indexed
           << "\nDimensions: " << response.dimension << "\nRequested K: " << request.k
           << "\nCandidate K per retriever: " << response.candidate_k
           << "\nDense candidates: " << response.dense_candidates
           << "\nLexical candidates: " << response.lexical_candidates
           << "\nFused candidates (union): " << response.fused_candidates
           << "\nResults returned: " << response.fused.size()
           << "\nBM25 configuration: k1=" << request.bm25_config.k1
           << " b=" << request.bm25_config.b
           << (request.bm25_config.strategy != lexical::Bm25Strategy::MaxScore
                   ? std::string("\nBM25 strategy: ") + strategy_name(request.bm25_config.strategy)
                   : std::string())
           << (request.bm25_config.analyzer != lexical::Analyzer::AsciiTechV1
                   ? std::string("\nBM25 analyzer: ") +
                         lexical::analyzer_name(request.bm25_config.analyzer)
                   : std::string())
           << (request.bm25_config.query_weighting != lexical::QueryTermWeighting::Frequency
                   ? "\nBM25 query weighting: unique"
                   : "")
           << "\nHNSW configuration: M=" << hnsw.m << " M0=" << hnsw.m_max_0
           << " efConstruction=" << hnsw.ef_construction << " efSearch=" << hnsw.ef_search
           << " seed=" << hnsw.random_seed << "\nHNSW neighbor selection: "
           << (hnsw.neighbor_selection == NeighborSelection::Heuristic ? "Heuristic (Algorithm 4)"
                                                                       : "Simple (Algorithm 3)");
    if (!response.selected_query_id.empty())
        output << "\nQuery ID: " << display_id(response.selected_query_id);
    output << "\nDense index build time (ms): " << number(response.index_build_time.count(), 6)
           << "\nLexical index build time (ms): " << number(response.lexical_build_time.count(), 6)
           << "\nIndex build time scope: HNSW graph and BM25 index (excludes input loading)";
    print_quantization(output, request, response);
    print_filter(output, request, response, true);
    output << "\nQuery time (ms): " << number(response.query_time.count(), 6)
           << "\nQuery time scope: HNSW + BM25 + " << (convex ? "fusion" : "RRF")
           << " (excludes input loading, index builds and output)\n\n";
    std::vector<Row> rows;
    rows.reserve(response.fused.size());
    for (std::size_t i = 0; i < response.fused.size(); ++i) {
        const auto& row = response.fused[i];
        rows.push_back({std::to_string(i + 1), display_id(row.source_id), number(row.rrf_score, 12),
                        component_rank(row.dense_rank), component_rank(row.lexical_rank)});
    }
    print_table(output,
                {"Rank", "Document ID", convex ? "Fused Score" : "RRF Score", "Dense Rank",
                 "Lexical Rank"},
                rows);
    if (response.lexical_candidates == 0)
        output << "\nNo lexical matches; the fused ranking uses the dense candidates only.\n";
}

}  // namespace

int run(std::span<const std::string_view> arguments, std::ostream& output, std::ostream& errors) {
    CliOptions options;
    try {
        options = parse_arguments(arguments);
    } catch (const std::invalid_argument& error) {
        errors << "Error: " << error.what() << "\nUse aetherindex --help for usage.\n";
        return 2;
    } catch (const std::exception& error) {
        errors << "Error: " << error.what() << '\n';
        return 4;
    }
    try {
        if (options.show_help) {
            output << help_text();
            return 0;
        }
        if (options.convert) {
            const auto converted = convert_vector_file(options.conversion);
            std::ostringstream formatted;
            formatted.imbue(std::locale::classic());
            formatted << "AetherIndex Convert\n\nVectors written: " << converted.vectors
                      << "\nDimensions: " << converted.dimension
                      << "\nFormat: .avec v1 (raw FP32, little-endian, CRC-32C)";
            if (converted.documents_written)
                formatted << "\nDocuments written (text only): " << converted.documents_written;
            formatted << "\n\nConversion completed successfully.\n";
            output << formatted.str();
            return 0;
        }
        const auto response = SearchService{}.search(options.search);
        std::ostringstream formatted;
        formatted.imbue(std::locale::classic());
        formatted << "AetherIndex Search\n\n";
        const auto print = [&](const SearchResponse& result) {
            if (options.search.mode == SearchMode::Lexical)
                print_lexical(formatted, options.search, result);
            else if (options.search.mode == SearchMode::Hybrid)
                print_hybrid(formatted, options.search, result);
            else
                print_vector(formatted, options.search, result);
        };
        if (options.search.all_queries) {
            const double seconds = response.batch_time.count() / 1000.0;
            formatted << "Batch queries: " << response.batch.size()
                      << "\nQuery threads: " << options.search.query_threads
                      << "\nBatch time (ms): " << number(response.batch_time.count(), 6)
                      << "\nThroughput (queries/s): "
                      << number(seconds > 0 ? response.batch.size() / seconds : 0, 1)
                      << "\nBatch time scope: all queries, excluding input loading, index "
                         "builds and output\n";
            // Each block is exactly the single-query report for that query.
            for (std::size_t i = 0; i < response.batch.size(); ++i) {
                auto view = response.batch[i];
                view.vectors_indexed = response.vectors_indexed;
                view.documents_indexed = response.documents_indexed;
                view.dimension = response.dimension;
                view.index_build_time = response.index_build_time;
                view.lexical_build_time = response.lexical_build_time;
                view.quantization_time = response.quantization_time;
                view.fp32_bytes = response.fp32_bytes;
                view.code_bytes = response.code_bytes;
                view.fp32_released = response.fp32_released;
                view.rerank = response.rerank;
                view.filtered = response.filtered;
                view.filter_matches = response.filter_matches;
                view.filter_population = response.filter_population;
                view.exact_filter = response.exact_filter;
                formatted << "\n=== Query " << i + 1 << "/" << response.batch.size() << " ===\n";
                print(view);
            }
        } else {
            print(response);
        }
        formatted << "\nSearch completed successfully.\n";
        output << formatted.str();
        return 0;
    } catch (const InputDataError& error) {
        errors << "Error: " << error.what() << '\n';
        return 3;
    } catch (const std::exception& error) {
        errors << "Error during search: " << error.what() << '\n';
        return 4;
    }
}

}  // namespace aetherindex::cli
