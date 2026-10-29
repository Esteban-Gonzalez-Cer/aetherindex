#include "cli_parser.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

namespace aetherindex::cli {
namespace {

template <typename Integer>
Integer parse_unsigned(std::string_view text, std::string_view option, bool allow_zero = false) {
    Integer value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || (!allow_zero && value == 0)) {
        throw std::invalid_argument("Invalid value for " + std::string(option) +
                                    ": expected a representable " +
                                    (allow_zero ? "unsigned" : "positive") + " integer.");
    }
    return value;
}

double parse_real(std::string_view text, std::string_view option) {
    double value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        throw std::invalid_argument("Invalid value for " + std::string(option) +
                                    ": expected a finite number.");
    return value;
}

CliOptions parse_convert(std::span<const std::string_view> arguments) {
    CliOptions options;
    options.convert = true;
    bool have_dataset = false, have_out = false, have_documents = false;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto name = arguments[i];
        if (name == "--help") {
            if (options.show_help) throw std::invalid_argument("Repeated option: --help.");
            options.show_help = true;
            continue;
        }
        bool* seen = name == "--dataset"         ? &have_dataset
                     : name == "--out"           ? &have_out
                     : name == "--documents-out" ? &have_documents
                                                 : nullptr;
        if (!seen) throw std::invalid_argument("Unknown option: " + std::string(name) + ".");
        if (*seen) throw std::invalid_argument("Repeated option: " + std::string(name) + ".");
        *seen = true;
        if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
            arguments[i + 1].starts_with("--"))
            throw std::invalid_argument("Missing value for " + std::string(name) + ".");
        const std::string value(arguments[++i]);
        if (name == "--dataset")
            options.conversion.dataset_path = value;
        else if (name == "--out")
            options.conversion.output_path = value;
        else
            options.conversion.documents_output = value;
    }
    if (!options.show_help && (!have_dataset || !have_out))
        throw std::invalid_argument("convert requires --dataset FILE and --out FILE.avec.");
    if (!options.show_help) validate_convert_request(options.conversion);
    return options;
}

}  // namespace

CliOptions parse_arguments(std::span<const std::string_view> arguments) {
    CliOptions options;
    if (arguments.empty() || (arguments.size() == 1 && arguments[0] == "--help")) {
        options.show_help = true;
        return options;
    }
    if (arguments[0] == "convert") return parse_convert(arguments);
    if (arguments[0] != "search") {
        throw std::invalid_argument("Unknown command: " + std::string(arguments[0]) + ".");
    }
    bool have_dataset = false;
    bool have_query = false;
    bool have_algorithm = false;
    bool have_k = false;
    bool have_metric = false;
    bool have_m = false;
    bool have_m0 = false;
    bool have_ef_construction = false;
    bool have_ef_search = false;
    bool have_seed = false;
    bool have_selection = false;
    bool have_mode = false;
    bool have_query_text = false;
    bool have_query_id = false;
    bool have_k1 = false;
    bool have_b = false;
    bool have_candidate_k = false;
    bool have_rrf_k = false;
    bool have_vectors = false;
    bool have_build_threads = false;
    bool have_query_threads = false;
    bool have_quantization = false;
    bool have_rerank = false;
    bool have_pq_subspaces = false;
    bool have_bm25_strategy = false;
    bool have_analyzer = false;
    bool have_query_weighting = false;
    bool bm25_selected = false;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto name = arguments[i];
        if (name == "--help") {
            if (options.show_help) {
                throw std::invalid_argument("Repeated option: --help.");
            }
            options.show_help = true;
            continue;
        }
        // Valueless switches.
        if (name == "--all-queries" || name == "--parallel-retrievers" || name == "--drop-fp32") {
            bool& flag = name == "--all-queries"           ? options.search.all_queries
                         : name == "--parallel-retrievers" ? options.search.parallel_retrievers
                                                           : options.search.drop_fp32;
            if (flag) throw std::invalid_argument("Repeated option: " + std::string(name) + ".");
            flag = true;
            continue;
        }
        // Repeatable filters (combined with AND).
        if (name == "--filter" || name == "--filter-date") {
            if (i + 1 == arguments.size() || arguments[i + 1].starts_with("--"))
                throw std::invalid_argument("Missing value for " + std::string(name) + ".");
            const auto value = arguments[++i];
            options.search.filters.push_back(name == "--filter" ? parse_equality_filter(value)
                                                                : parse_date_filter(value));
            continue;
        }
        bool* seen = nullptr;
        if (name == "--dataset") {
            seen = &have_dataset;
        } else if (name == "--query") {
            seen = &have_query;
        } else if (name == "--mode") {
            seen = &have_mode;
        } else if (name == "--query-text") {
            seen = &have_query_text;
        } else if (name == "--query-id") {
            seen = &have_query_id;
        } else if (name == "--bm25-k1") {
            seen = &have_k1;
        } else if (name == "--bm25-b") {
            seen = &have_b;
        } else if (name == "--candidate-k") {
            seen = &have_candidate_k;
        } else if (name == "--rrf-k") {
            seen = &have_rrf_k;
        } else if (name == "--vectors") {
            seen = &have_vectors;
        } else if (name == "--build-threads") {
            seen = &have_build_threads;
        } else if (name == "--query-threads") {
            seen = &have_query_threads;
        } else if (name == "--quantization") {
            seen = &have_quantization;
        } else if (name == "--rerank") {
            seen = &have_rerank;
        } else if (name == "--pq-subspaces") {
            seen = &have_pq_subspaces;
        } else if (name == "--bm25-strategy") {
            seen = &have_bm25_strategy;
        } else if (name == "--analyzer") {
            seen = &have_analyzer;
        } else if (name == "--bm25-query-weighting") {
            seen = &have_query_weighting;
        } else if (name == "--algorithm") {
            seen = &have_algorithm;
        } else if (name == "--k") {
            seen = &have_k;
        } else if (name == "--metric") {
            seen = &have_metric;
        } else if (name == "--m") {
            seen = &have_m;
        } else if (name == "--m0") {
            seen = &have_m0;
        } else if (name == "--ef-construction") {
            seen = &have_ef_construction;
        } else if (name == "--ef-search") {
            seen = &have_ef_search;
        } else if (name == "--seed") {
            seen = &have_seed;
        } else if (name == "--neighbor-selection") {
            seen = &have_selection;
        } else {
            throw std::invalid_argument("Unknown option: " + std::string(name) + ".");
        }
        if (*seen) {
            throw std::invalid_argument("Repeated option: " + std::string(name) + ".");
        }
        *seen = true;
        if (i + 1 == arguments.size() || (arguments[i + 1].empty() && name != "--query-text") ||
            arguments[i + 1].starts_with("--")) {
            throw std::invalid_argument("Missing value for " + std::string(name) + ".");
        }
        const auto value = arguments[++i];
        if (name == "--dataset") {
            options.search.dataset_path = std::string(value);
        } else if (name == "--query") {
            options.search.query_path = std::string(value);
        } else if (name == "--query-text") {
            options.search.query_text = std::string(value);
        } else if (name == "--query-id") {
            options.search.query_id = std::string(value);
        } else if (name == "--mode") {
            if (value == "vector")
                options.search.mode = SearchMode::Vector;
            else if (value == "lexical")
                options.search.mode = SearchMode::Lexical;
            else if (value == "hybrid")
                options.search.mode = SearchMode::Hybrid;
            else
                throw std::invalid_argument("Invalid --mode: use vector, lexical or hybrid.");
        } else if (name == "--bm25-k1") {
            options.search.bm25_config.k1 = parse_real(value, name);
        } else if (name == "--bm25-b") {
            options.search.bm25_config.b = parse_real(value, name);
        } else if (name == "--rrf-k") {
            options.search.rrf_config.k_rrf = parse_real(value, name);
        } else if (name == "--vectors") {
            options.search.vectors_path = std::string(value);
        } else if (name == "--build-threads") {
            options.search.build_threads = parse_unsigned<std::size_t>(value, name);
        } else if (name == "--query-threads") {
            options.search.query_threads = parse_unsigned<std::size_t>(value, name);
        } else if (name == "--quantization") {
            if (value == "none")
                options.search.quantization = Quantization::None;
            else if (value == "sq8")
                options.search.quantization = Quantization::Sq8;
            else if (value == "pq")
                options.search.quantization = Quantization::Pq;
            else
                throw std::invalid_argument("Invalid --quantization: use none, sq8 or pq.");
        } else if (name == "--rerank") {
            options.search.rerank = parse_unsigned<std::size_t>(value, name, true);
        } else if (name == "--pq-subspaces") {
            options.search.pq_subspaces = parse_unsigned<std::size_t>(value, name);
        } else if (name == "--bm25-strategy") {
            auto& strategy = options.search.bm25_config.strategy;
            if (value == "exhaustive")
                strategy = lexical::Bm25Strategy::Exhaustive;
            else if (value == "taat")
                strategy = lexical::Bm25Strategy::TermAtATime;
            else if (value == "wand")
                strategy = lexical::Bm25Strategy::Wand;
            else if (value == "bmw")
                strategy = lexical::Bm25Strategy::BlockMaxWand;
            else if (value == "maxscore")
                strategy = lexical::Bm25Strategy::MaxScore;
            else
                throw std::invalid_argument(
                    "Invalid --bm25-strategy: use exhaustive, taat, wand, bmw or maxscore.");
        } else if (name == "--analyzer") {
            if (value == "ascii-tech-v1")
                options.search.bm25_config.analyzer = lexical::Analyzer::AsciiTechV1;
            else if (value == "english-v1")
                options.search.bm25_config.analyzer = lexical::Analyzer::EnglishV1;
            else
                throw std::invalid_argument("Invalid --analyzer: use ascii-tech-v1 or english-v1.");
        } else if (name == "--bm25-query-weighting") {
            if (value == "unique")
                options.search.bm25_config.query_weighting = lexical::QueryTermWeighting::Unique;
            else if (value == "frequency")
                options.search.bm25_config.query_weighting = lexical::QueryTermWeighting::Frequency;
            else
                throw std::invalid_argument(
                    "Invalid --bm25-query-weighting: use unique or frequency.");
        } else if (name == "--candidate-k") {
            options.search.candidate_k = parse_unsigned<std::size_t>(value, name);
        } else if (name == "--algorithm") {
            if (value == "brute-force") {
                options.search.algorithm = SearchAlgorithm::BruteForce;
            } else if (value == "hnsw") {
                options.search.algorithm = SearchAlgorithm::Hnsw;
            } else if (value == "bm25") {
                bm25_selected = true;
            } else {
                throw std::invalid_argument("Unknown algorithm: " + std::string(value) +
                                            ". Available algorithms: brute-force, hnsw, bm25.");
            }
        } else if (name == "--metric") {
            if (value == "cosine") {
                options.search.metric = DistanceKind::Cosine;
            } else if (value == "squared-euclidean") {
                options.search.metric = DistanceKind::SquaredEuclidean;
            } else {
                throw std::invalid_argument("Invalid --metric: use cosine or squared-euclidean.");
            }
        } else if (name == "--neighbor-selection") {
            if (value == "simple")
                options.search.hnsw_config.neighbor_selection = NeighborSelection::Simple;
            else if (value == "heuristic")
                options.search.hnsw_config.neighbor_selection = NeighborSelection::Heuristic;
            else
                throw std::invalid_argument(
                    "Invalid --neighbor-selection: use simple or heuristic.");
        } else if (name == "--k") {
            options.search.k = parse_unsigned<std::size_t>(value, name);
        } else if (name == "--seed") {
            options.search.hnsw_config.random_seed =
                parse_unsigned<std::uint64_t>(value, name, true);
        } else {
            const auto parameter = parse_unsigned<std::size_t>(value, name);
            auto& config = options.search.hnsw_config;
            if (name == "--m")
                config.m = parameter;
            else if (name == "--m0")
                config.m_max_0 = parameter;
            else if (name == "--ef-construction")
                config.ef_construction = parameter;
            else if (name == "--ef-search")
                config.ef_search = parameter;
        }
    }
    if (!options.show_help) {
        if (!have_dataset) {
            throw std::invalid_argument("Missing required option: --dataset.");
        }
        if (!have_query && !have_query_text) {
            throw std::invalid_argument("Missing required option: --query.");
        }
        const auto mode = options.search.mode;
        if (!have_mode && bm25_selected) options.search.mode = SearchMode::Lexical;
        if (options.search.mode == SearchMode::Vector && !have_algorithm) {
            throw std::invalid_argument("Missing required option: --algorithm.");
        }
        if ((options.search.mode == SearchMode::Lexical && have_algorithm && !bm25_selected) ||
            (options.search.mode == SearchMode::Vector && bm25_selected))
            throw std::invalid_argument(
                "Incompatible --mode and --algorithm: BM25 is lexical; brute-force and hnsw are "
                "vector algorithms.");
        // Hybrid always fuses HNSW with BM25; --algorithm hnsw is accepted as a restatement.
        if (mode == SearchMode::Hybrid) {
            if (have_algorithm &&
                (bm25_selected || options.search.algorithm != SearchAlgorithm::Hnsw))
                throw std::invalid_argument(
                    "Incompatible --mode and --algorithm: hybrid fuses HNSW with BM25.");
            options.search.algorithm = SearchAlgorithm::Hnsw;
        }
        if (options.search.mode == SearchMode::Lexical && have_metric)
            throw std::invalid_argument(
                "--metric requires vector or hybrid mode; BM25 returns scores, not distances.");
        if (options.search.mode == SearchMode::Vector &&
            (have_k1 || have_b || have_bm25_strategy || have_analyzer || have_query_weighting))
            throw std::invalid_argument(
                "BM25 options require lexical or hybrid mode (--mode lexical|hybrid or "
                "--algorithm bm25).");
        if (options.search.mode != SearchMode::Hybrid &&
            (have_candidate_k || have_rrf_k || have_vectors))
            throw std::invalid_argument(
                "--candidate-k, --rrf-k and --vectors require --mode hybrid.");
        const bool hnsw_selected = options.search.mode == SearchMode::Hybrid ||
                                   (options.search.mode == SearchMode::Vector &&
                                    options.search.algorithm == SearchAlgorithm::Hnsw);
        if (!hnsw_selected && (have_m || have_m0 || have_ef_construction || have_ef_search ||
                               have_seed || have_selection || have_build_threads)) {
            throw std::invalid_argument("HNSW options require --algorithm hnsw or --mode hybrid.");
        }
        if (have_query_threads && !options.search.all_queries)
            throw std::invalid_argument("--query-threads requires --all-queries.");
        validate_search_request(options.search);
    }
    return options;
}

std::string_view help_text() {
    return "AetherIndex CLI\n\n"
           "Usage:\n"
           "  aetherindex search --dataset FILE --query FILE --algorithm NAME [options]\n"
           "  aetherindex search --mode lexical --dataset FILE --query-text TEXT [options]\n"
           "  aetherindex search --algorithm bm25 --dataset FILE --query FILE --query-id ID "
           "[options]\n"
           "  aetherindex search --mode hybrid --dataset FILE --query FILE --query-id ID "
           "[options]\n"
           "  aetherindex convert --dataset FILE.(csv|jsonl) --out FILE.avec "
           "[--documents-out FILE.jsonl]\n"
           "  aetherindex --help\n\n"
           "Options:\n"
           "  --dataset FILE       Required: vector CSV/JSONL, or document JSONL for BM25\n"
           "  --query FILE         Query CSV (vector) or JSONL (either mode)\n"
           "  --query-text TEXT    BM25 query text instead of a query file; empty text is valid\n"
           "  --query-id ID        Select exactly one query from a JSONL query file\n"
           "  --mode NAME          vector (default) | lexical (BM25) | hybrid (HNSW+BM25+RRF)\n"
           "  --algorithm NAME     brute-force (exact) | hnsw (approximate) | bm25 (lexical)\n"
           "                       Required in vector mode; bm25 implies lexical mode\n"
           "  --k INTEGER          Positive maximum result count (default: 10)\n"
           "  --bm25-k1 NUMBER     BM25 finite positive term saturation (default: 1.2)\n"
           "  --bm25-b NUMBER      BM25 finite length normalization in [0,1] (default: 0.75)\n"
           "  --bm25-strategy NAME exhaustive (default) | taat | wand | bmw (Block-Max WAND) |\n"
           "                       maxscore;\n"
           "                       identical results, compressed postings except exhaustive\n"
           "  --analyzer NAME      ascii-tech-v1 (default) | english-v1 (Lucene-style English:\n"
           "                       stopwords, possessives, Porter stemming)\n"
           "  --bm25-query-weighting NAME\n"
           "                       unique (default) | frequency (a repeated query term counts\n"
           "                       once per occurrence, as in Lucene/Anserini)\n"
           "  --filter F=V[|V2]    Keep documents whose metadata F equals V (or V2); repeatable\n"
           "  --filter-date F=A..B Keep documents whose metadata date F (YYYY-MM-DD) is in [A,B];\n"
           "                       A or B may be empty; all filters must hold (JSONL only)\n"
           "  --candidate-k N      Hybrid: candidates per retriever, >= k (default: max(k,50))\n"
           "  --rrf-k NUMBER       Hybrid: finite positive RRF smoothing constant (default: 60)\n"
           "  --vectors FILE.avec  Hybrid: binary embeddings for the JSONL documents\n"
           "  --all-queries        Run every query of a JSONL --query file with one index build\n"
           "  --query-threads N    Threads for --all-queries (default: 1; output keeps file "
           "order)\n"
           "  --build-threads N    HNSW construction threads (default: 1 = reproducible graph;\n"
           "                       more threads are faster but the graph is not reproducible)\n"
           "  --parallel-retrievers  Hybrid: run BM25 concurrently with HNSW in each query\n"
           "  --quantization NAME  HNSW traversal on none (default) | sq8 | pq codes\n"
           "  --rerank N           Exact FP32 re-rank depth for quantized search (default: all\n"
           "                       candidates; 0 = approximate distances only)\n"
           "  --drop-fp32          Free FP32 rows after building (implies --rerank 0)\n"
           "  --pq-subspaces M     PQ subspaces dividing the dimension (default: dimension/4)\n"
           "                       PQ usually needs a larger --ef-search for the re-rank\n"
           "  --metric NAME        Vector/hybrid: cosine (default) | squared-euclidean\n"
           "  --m INTEGER          HNSW upper-layer neighbor limit (default: 16; >= 2)\n"
           "  --m0 INTEGER         HNSW base-layer neighbor limit (default: 32; >= M)\n"
           "  --ef-construction N  HNSW construction search width (default: 100; >= M)\n"
           "  --ef-search N        HNSW query search width (default: 50; > 0)\n"
           "  --seed INTEGER       HNSW unsigned 64-bit seed, including zero (default: 42)\n"
           "  --neighbor-selection NAME  HNSW heuristic (default, Algorithm 4) | simple (Algorithm "
           "3)\n"
           "  --help               Show help (also: search --help)\n\n"
           "CSV: no headers or quotes; finite floats; unique unsigned 32-bit IDs.\n"
           "JSONL: one UTF-8 object per line. Documents: document_id, text, optional title.\n"
           "Queries: query_id, query_text. Legacy id/query/vector fields are accepted.\n"
           "Multiple queries require --query-id; relevance labels never affect ranking.\n"
           "BM25 indexes title + text, requires no embeddings and sorts by decreasing score.\n"
           "BM25 returns only matching documents; empty/unknown terms may return zero results.\n"
           "Vector JSONL needs nonempty embedding arrays in documents and queries.\n"
           "Spaces/tabs around fields and blank lines are allowed. All dimensions must match.\n"
           "Cosine stores unit vectors and rejects embedding L2 norms below 1e-7.\n"
           "Cosine queries must be nonzero; squared Euclidean permits zero vectors.\n"
           "Vector results are sorted by increasing distance; at most min(k, dataset size) are "
           "returned.\n"
           "Distances use nine decimal places; IDs stay integers in the bordered results table.\n"
           "Query time is reported in milliseconds and excludes input loading and output.\n"
           "HNSW graph construction is reported separately and excluded from query time.\n"
           "HNSW uses max(efSearch,k), capped by index size; sparse graphs may return fewer "
           "neighbors.\n"
           "HNSW options require --algorithm hnsw or --mode hybrid; the graph is rebuilt in each "
           "execution.\n"
           "Hybrid needs JSONL documents with text and embeddings, and one JSONL query with\n"
           "query_text and embedding. HNSW and BM25 rankings share DocumentIds and are fused\n"
           "with RRF: score = sum 1/(k_rrf + rank), ranks from 1, ties by DocumentId.\n"
           "Data is loaded and searched in one execution. There is no persistence.\n"
           "Binary .avec embeddings (from convert) load without text parsing: use them as\n"
           "--dataset in vector mode or as --vectors with JSONL documents in hybrid mode.\n\n"
           "Exit codes: 0 success/help, 2 arguments, 3 input data, 4 "
           "search/internal error.\n";
}

}  // namespace aetherindex::cli
