#include "cli_parser.hpp"

#include <algorithm>
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

// Who reads the options: a one-shot search, an index build, or a search on a saved index.
enum class Purpose { Search, Build, IndexQuery };

bool is_query_option(std::string_view name) {
    for (std::string_view option :
         {"--query", "--query-text", "--query-id", "--k", "--candidate-k", "--rrf-k", "--rrf-weight",
          "--alpha", "--fusion", "--all-queries", "--query-threads", "--parallel-retrievers",
          "--rerank", "--filter", "--filter-date"})
        if (name == option) return true;
    return false;
}

bool is_build_option(std::string_view name) {
    for (std::string_view option :
         {"--dataset", "--vectors", "--algorithm", "--mode", "--metric", "--m", "--m0",
          "--ef-construction", "--seed", "--neighbor-selection", "--bm25-k1", "--bm25-b",
          "--analyzer", "--quantization", "--pq-subspaces", "--drop-fp32", "--build-threads"})
        if (name == option) return true;
    return false;
}

// Search-style options, from arguments[first] on. Search: dataset + query (the original
// one-shot command). Build: dataset and build options only. IndexQuery: query options
// only; the build options come from the saved index.
CliOptions parse_search(std::span<const std::string_view> arguments, std::size_t first,
                        Purpose purpose, CliOptions options) {
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
    bool have_rrf_weight = false;
    bool have_alpha = false;
    bool have_fusion = false;
    auto fusion_choice = hybrid::FusionMethod::Convex;
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
    bool have_index = false;
    bool have_out = false;
    for (std::size_t i = first; i < arguments.size(); ++i) {
        const auto name = arguments[i];
        if (name == "--help") {
            if (options.show_help) {
                throw std::invalid_argument("Repeated option: --help.");
            }
            options.show_help = true;
            continue;
        }
        if (purpose == Purpose::Build && is_query_option(name))
            throw std::invalid_argument(std::string(name) +
                                        " is a query option; pass it to search --index DIR.");
        if (purpose == Purpose::IndexQuery && is_build_option(name))
            throw std::invalid_argument(std::string(name) +
                                        " is fixed when the index is built (index build).");
        if (purpose == Purpose::IndexQuery && is_query_option(name))
            options.index_query_options.emplace_back(name);
        if (name == "--no-verify") {
            if (purpose != Purpose::IndexQuery)
                throw std::invalid_argument("--no-verify requires search --index DIR.");
            if (!options.verify_checksums)
                throw std::invalid_argument("Repeated option: --no-verify.");
            options.verify_checksums = false;
            continue;
        }
        if (name == "--index" || name == "--out") {
            const bool index = name == "--index";
            if (index ? purpose != Purpose::IndexQuery : purpose != Purpose::Build)
                throw std::invalid_argument(std::string(name) + (index ? " requires search." : " requires index build."));
            bool& seen = index ? have_index : have_out;
            if (seen) throw std::invalid_argument("Repeated option: " + std::string(name) + ".");
            seen = true;
            if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
                arguments[i + 1].starts_with("--"))
                throw std::invalid_argument("Missing value for " + std::string(name) + ".");
            options.index_path = std::string(arguments[++i]);
            continue;
        }
        if (purpose == Purpose::IndexQuery &&
            (name == "--ef-search" || name == "--bm25-strategy" || name == "--bm25-query-weighting"))
            options.index_query_options.emplace_back(name);
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
        } else if (name == "--rrf-weight") {
            seen = &have_rrf_weight;
        } else if (name == "--alpha") {
            seen = &have_alpha;
        } else if (name == "--fusion") {
            seen = &have_fusion;
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
        } else if (name == "--rrf-weight") {
            options.search.rrf_config.dense_weight = parse_real(value, name);
        } else if (name == "--alpha") {
            options.search.rrf_config.alpha = parse_real(value, name);
        } else if (name == "--fusion") {
            if (value == "convex")
                fusion_choice = hybrid::FusionMethod::Convex;
            else if (value == "rrf")
                fusion_choice = hybrid::FusionMethod::Rrf;
            else
                throw std::invalid_argument("Invalid --fusion: use convex or rrf.");
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
    if (options.show_help) return options;
    if (purpose == Purpose::Build && !have_out)
        throw std::invalid_argument("Missing required option: --out DIR.");
    if (purpose != Purpose::IndexQuery && !have_dataset) {
        throw std::invalid_argument("Missing required option: --dataset.");
    }
    if (purpose != Purpose::Build && !have_query && !have_query_text) {
        throw std::invalid_argument("Missing required option: --query.");
    }
    if (purpose == Purpose::IndexQuery) {
        if (have_alpha && (have_rrf_k || have_rrf_weight))
            throw std::invalid_argument(
                "--alpha selects convex fusion; it cannot be combined with --rrf-k or "
                "--rrf-weight.");
        const bool rrf_options = have_rrf_k || have_rrf_weight;
        if (have_fusion && fusion_choice == hybrid::FusionMethod::Rrf && have_alpha)
            throw std::invalid_argument("--alpha requires --fusion convex.");
        if (have_fusion && fusion_choice == hybrid::FusionMethod::Convex && rrf_options)
            throw std::invalid_argument("--rrf-k and --rrf-weight require --fusion rrf.");
        if (have_fusion || rrf_options)
            options.search.rrf_config.method =
                rrf_options ? hybrid::FusionMethod::Rrf : fusion_choice;
        if (have_query_threads && !options.search.all_queries)
            throw std::invalid_argument("--query-threads requires --all-queries.");
        return options;
    }
    {
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
            (have_candidate_k || have_rrf_k || have_rrf_weight || have_alpha || have_fusion ||
             have_vectors))
            throw std::invalid_argument(
                "--candidate-k, --fusion, --rrf-k, --rrf-weight, --alpha and --vectors require "
                "--mode hybrid.");
        if (have_alpha && (have_rrf_k || have_rrf_weight))
            throw std::invalid_argument(
                "--alpha selects convex fusion; it cannot be combined with --rrf-k or "
                "--rrf-weight.");
        // Convex is the default; --rrf-k or --rrf-weight alone select RRF.
        const bool rrf_options = have_rrf_k || have_rrf_weight;
        if (have_fusion && fusion_choice == hybrid::FusionMethod::Rrf && have_alpha)
            throw std::invalid_argument("--alpha requires --fusion convex.");
        if (have_fusion && fusion_choice == hybrid::FusionMethod::Convex && rrf_options)
            throw std::invalid_argument("--rrf-k and --rrf-weight require --fusion rrf.");
        if (have_fusion || rrf_options)
            options.search.rrf_config.method =
                rrf_options ? hybrid::FusionMethod::Rrf : fusion_choice;
        const bool hnsw_selected = options.search.mode == SearchMode::Hybrid ||
                                   (options.search.mode == SearchMode::Vector &&
                                    options.search.algorithm == SearchAlgorithm::Hnsw);
        if (!hnsw_selected && (have_m || have_m0 || have_ef_construction || have_ef_search ||
                               have_seed || have_selection || have_build_threads)) {
            throw std::invalid_argument("HNSW options require --algorithm hnsw or --mode hybrid.");
        }
        if (have_query_threads && !options.search.all_queries)
            throw std::invalid_argument("--query-threads requires --all-queries.");
        if (purpose == Purpose::Build)
            validate_build_request(options.search);
        else
            validate_search_request(options.search);
    }
    return options;
}

CliOptions parse_index(std::span<const std::string_view> arguments) {
    CliOptions options;
    if (arguments.size() < 2)
        throw std::invalid_argument("Missing index command: use index build, info or verify.");
    if (arguments[1] == "--help") {
        options.show_help = true;
        return options;
    }
    const auto sub = arguments[1];
    if (sub == "build") {
        options.command = Command::IndexBuild;
        return parse_search(arguments, 2, Purpose::Build, std::move(options));
    }
    if (sub == "info")
        options.command = Command::IndexInfo;
    else if (sub == "verify")
        options.command = Command::IndexVerify;
    else if (sub == "add")
        options.command = Command::IndexAdd;
    else if (sub == "delete")
        options.command = Command::IndexDelete;
    else if (sub == "compact")
        options.command = Command::IndexCompact;
    else if (sub == "checkpoint")
        options.command = Command::IndexCheckpoint;
    else
        throw std::invalid_argument("Unknown index command: " + std::string(sub) +
                                    ". Use build, add, delete, compact, checkpoint, info or verify.");
    const auto accepts = [&](std::string_view name) {
        if (name == "--index") return true;
        if (options.command == Command::IndexAdd)
            return name == "--dataset" || name == "--vectors" || name == "--replace" ||
                   name == "--no-sync";
        if (options.command == Command::IndexDelete) return name == "--id" || name == "--no-sync";
        return false;
    };
    bool have_index = false, have_dataset = false, have_vectors = false;
    for (std::size_t i = 2; i < arguments.size(); ++i) {
        const auto name = arguments[i];
        if (name == "--help") {
            options.show_help = true;
            continue;
        }
        if (!accepts(name))
            throw std::invalid_argument("Unknown option for index " + std::string(sub) + ": " +
                                        std::string(name) + ".");
        if (name == "--replace") {
            if (options.replace) throw std::invalid_argument("Repeated option: --replace.");
            options.replace = true;
            continue;
        }
        if (name == "--no-sync") {
            if (!options.sync) throw std::invalid_argument("Repeated option: --no-sync.");
            options.sync = false;
            continue;
        }
        if (i + 1 == arguments.size() || arguments[i + 1].empty() ||
            arguments[i + 1].starts_with("--"))
            throw std::invalid_argument("Missing value for " + std::string(name) + ".");
        const std::string value(arguments[++i]);
        if (name == "--id") {
            options.delete_ids.push_back(value);
            continue;
        }
        bool& seen = name == "--index" ? have_index : name == "--dataset" ? have_dataset : have_vectors;
        if (seen) throw std::invalid_argument("Repeated option: " + std::string(name) + ".");
        seen = true;
        if (name == "--index")
            options.index_path = value;
        else if (name == "--dataset")
            options.search.dataset_path = value;
        else
            options.search.vectors_path = value;
    }
    if (options.show_help) return options;
    if (!have_index) throw std::invalid_argument("Missing required option: --index DIR.");
    if (options.command == Command::IndexAdd && !have_dataset)
        throw std::invalid_argument("index add requires --dataset FILE.");
    if (options.command == Command::IndexDelete && options.delete_ids.empty())
        throw std::invalid_argument("index delete requires at least one --id ID.");
    return options;
}

}  // namespace

CliOptions parse_arguments(std::span<const std::string_view> arguments) {
    CliOptions options;
    if (arguments.empty() || (arguments.size() == 1 && arguments[0] == "--help")) {
        options.show_help = true;
        return options;
    }
    if (arguments[0] == "convert") {
        options = parse_convert(arguments);
        options.command = Command::Convert;
        return options;
    }
    if (arguments[0] == "index") return parse_index(arguments);
    if (arguments[0] != "search") {
        throw std::invalid_argument("Unknown command: " + std::string(arguments[0]) + ".");
    }
    const bool on_index = std::find(arguments.begin(), arguments.end(), "--index") != arguments.end();
    return parse_search(arguments, 1, on_index ? Purpose::IndexQuery : Purpose::Search,
                        std::move(options));
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
           "  aetherindex index build --out DIR --dataset FILE [build options]\n"
           "  aetherindex search --index DIR --query FILE [query options]\n"
           "  aetherindex index add --index DIR --dataset FILE [--vectors FILE.avec] [--replace]\n"
           "  aetherindex index delete --index DIR --id ID [--id ID ...]\n"
           "  aetherindex index compact --index DIR\n"
           "  aetherindex index checkpoint --index DIR\n"
           "  aetherindex index info|verify --index DIR\n"
           "  aetherindex --help\n\n"
           "Saved indexes:\n"
           "  index build --out DIR  Build once and save to a new or empty DIR. Takes the\n"
           "                       dataset and build options (--mode, --algorithm, --metric,\n"
           "                       HNSW, BM25, --quantization, --vectors, --build-threads).\n"
           "  search --index DIR   Search a saved index. Takes query options only (--query,\n"
           "                       --k, fusion, filters, --rerank, batching); --ef-search,\n"
           "                       --bm25-strategy (compressed strategies) and\n"
           "                       --bm25-query-weighting override the saved defaults.\n"
           "  --no-verify          search --index: skip payload checksums while loading\n"
           "  index add            Add records in the format the index was built from;\n"
           "                       --replace updates records whose ID already exists\n"
           "  index delete         Delete records by ID (document_id, or the vector ID)\n"
           "                       add and delete append to the index's write-ahead log and\n"
           "                       are durable when they return (--no-sync: survive a crash\n"
           "                       of the process, not necessarily a power loss)\n"
           "  index compact        Drop deleted records for good, rebuild, and checkpoint\n"
           "  index checkpoint     Write a snapshot with every logged change; empty the log\n"
           "  index info           Summarize a saved index\n"
           "  index verify         Check every checksum and the graph structure\n\n"
           "Options:\n"
           "  --dataset FILE       Required: vector CSV/JSONL, or document JSONL for BM25\n"
           "  --query FILE         Query CSV (vector) or JSONL (either mode)\n"
           "  --query-text TEXT    BM25 query text instead of a query file; empty text is valid\n"
           "  --query-id ID        Select exactly one query from a JSONL query file\n"
           "  --mode NAME          vector (default) | lexical (BM25) | hybrid (HNSW+BM25)\n"
           "  --algorithm NAME     brute-force (exact) | hnsw (approximate) | bm25 (lexical)\n"
           "                       Required in vector mode; bm25 implies lexical mode\n"
           "  --k INTEGER          Positive maximum result count (default: 10)\n"
           "  --bm25-k1 NUMBER     BM25 finite positive term saturation (default: 1.2)\n"
           "  --bm25-b NUMBER      BM25 finite length normalization in [0,1] (default: 0.75)\n"
           "  --bm25-strategy NAME maxscore (default) | bmw (Block-Max WAND) | wand | taat |\n"
           "                       exhaustive; identical results, compressed postings except\n"
           "                       exhaustive\n"
           "  --analyzer NAME      ascii-tech-v1 (default) | english-v1 (Lucene-style English:\n"
           "                       stopwords, possessives, Porter stemming)\n"
           "  --bm25-query-weighting NAME\n"
           "                       frequency (default; a repeated query term counts once per\n"
           "                       occurrence, as in Lucene/Anserini) | unique (once)\n"
           "  --filter F=V[|V2]    Keep documents whose metadata F equals V (or V2); repeatable\n"
           "  --filter-date F=A..B Keep documents whose metadata date F (YYYY-MM-DD) is in [A,B];\n"
           "                       A or B may be empty; all filters must hold (JSONL only)\n"
           "  --candidate-k N      Hybrid: candidates per retriever, >= k (default: max(k,50))\n"
           "  --fusion NAME        Hybrid: convex (default) | rrf (reciprocal rank fusion)\n"
           "  --alpha A            Hybrid convex: A * dense + (1 - A) * BM25 of min-max\n"
           "                       normalized scores, A in [0,1] (default: 0.65, tuned on BEIR\n"
           "                       dev splits)\n"
           "  --rrf-k NUMBER       Hybrid RRF: finite positive smoothing constant (default: 60);\n"
           "                       selects RRF\n"
           "  --rrf-weight W       Hybrid RRF: dense weight W in [0,1], lexical 1 - W (default:\n"
           "                       both weights 1, classic RRF); selects RRF\n"
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
           "HNSW options require --algorithm hnsw or --mode hybrid; search without --index\n"
           "rebuilds the graph in each execution.\n"
           "Hybrid needs JSONL documents with text and embeddings, and one JSONL query with\n"
           "query_text and embedding. HNSW and BM25 rankings share DocumentIds and are fused\n"
           "by default as 0.65 * dense + 0.35 * BM25 of min-max normalized candidate scores\n"
           "(dense score = 1 - distance); --fusion rrf uses score = sum 1/(k_rrf + rank),\n"
           "ranks from 1. Ties go by DocumentId.\n"
           "Without --index, data is loaded, indexed and searched in one execution; use\n"
           "index build once and search --index to reuse an index across executions.\n"
           "Binary .avec embeddings (from convert) load without text parsing: use them as\n"
           "--dataset in vector mode or as --vectors with JSONL documents in hybrid mode.\n\n"
           "Exit codes: 0 success/help, 2 arguments, 3 input data, 4 "
           "search/internal error.\n";
}

}  // namespace aetherindex::cli
