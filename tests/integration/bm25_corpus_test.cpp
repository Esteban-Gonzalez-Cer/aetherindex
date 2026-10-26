// Standalone preliminary evaluator, linked only to aether_lexical. File I/O
// belongs to this driver, never to the lexical library or production CLI.
#include <charconv>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "aether/lexical/bm25.hpp"
#include "aether/lexical/tokenizer.hpp"

namespace {
using namespace aetherindex;
using namespace aetherindex::lexical;

struct Query {
    std::string id;
    DocumentId expected;
    std::string text;
};

DocumentId parse_id(std::string_view value) {
    DocumentId id;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        throw std::invalid_argument("Invalid uint32 DocumentId in TSV.");
    }
    return id;
}

std::vector<std::string_view> fields(std::string_view line, std::size_t expected) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (true) {
        const auto stop = line.find('\t', start);
        result.push_back(
            line.substr(start, stop == line.npos ? line.size() - start : stop - start));
        if (stop == line.npos) break;
        start = stop + 1;
    }
    if (result.size() != expected) throw std::invalid_argument("Unexpected TSV column count.");
    return result;
}

template <class Consumer>
void read_tsv(const std::filesystem::path& path, Consumer consume) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open TSV: " + path.string());
    std::string line;
    std::size_t number = 0;
    while (std::getline(file, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty())
            throw std::invalid_argument("Blank TSV row at line " + std::to_string(number));
        try {
            consume(std::string_view(line));
        } catch (const std::exception& error) {
            throw std::runtime_error(path.filename().string() + ":" + std::to_string(number) +
                                     ": " + error.what());
        }
    }
    if (file.bad()) throw std::runtime_error("TSV read failed: " + path.string());
}

std::string csv(std::string_view value) {
    std::string escaped = "\"";
    for (char ch : value) {
        if (ch == '"') escaped += '"';
        escaped += ch;
    }
    return escaped + '"';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4)
            throw std::invalid_argument(
                "Usage: aether_bm25_evaluate DOCUMENTS.tsv QUERIES.tsv OUTPUT_DIR");
        Bm25Index index;
        read_tsv(argv[1], [&](std::string_view line) {
            const auto row = fields(line, 2);
            index.add_document(parse_id(row[0]), row[1]);
        });
        index.build();
        std::vector<Query> queries;
        std::unordered_set<std::string> query_ids;
        read_tsv(argv[2], [&](std::string_view line) {
            const auto row = fields(line, 3);
            static_cast<void>(
                Tokenizer{}.tokenize(row[0]));  // Validate UTF-8, preserve ID verbatim.
            if (row[0].empty() || !query_ids.insert(std::string(row[0])).second) {
                throw std::invalid_argument("Empty or duplicate query ID.");
            }
            const auto id = parse_id(row[1]);
            if (!index.inverted_index().document_length(id)) {
                throw std::invalid_argument("Expected DocumentId is absent from corpus.");
            }
            // Validate before creating output files. Empty/OOV queries are
            // legitimate misses; malformed UTF-8 is a data error.
            static_cast<void>(index.search(row[2], 10));
            queries.push_back({std::string(row[0]), id, std::string(row[2])});
        });
        if (queries.empty()) throw std::invalid_argument("Evaluation needs at least one query.");
        const std::filesystem::path output(argv[3]);
        std::filesystem::create_directories(output);
        std::ofstream rankings(output / "rankings.csv");
        std::ofstream positions(output / "queries.csv");
        rankings.exceptions(std::ios::failbit | std::ios::badbit);
        positions.exceptions(std::ios::failbit | std::ios::badbit);
        rankings.imbue(std::locale::classic());
        positions.imbue(std::locale::classic());
        rankings << std::setprecision(17) << "query_id,rank,document_id,bm25_score\n";
        positions << "query_id,query,expected_document_id,expected_rank_in_top10,hit_at_10,results_"
                     "returned\n";
        std::size_t hits = 0;
        for (const auto& query : queries) {
            const auto results = index.search(query.text, 10);
            std::size_t rank = 0;
            for (std::size_t i = 0; i < results.size(); ++i) {
                rankings << csv(query.id) << ',' << i + 1 << ',' << results[i].document_id << ','
                         << results[i].score << '\n';
                if (results[i].document_id == query.expected) rank = i + 1;
            }
            if (rank != 0) ++hits;
            positions << csv(query.id) << ',' << csv(query.text) << ',' << query.expected << ',';
            if (rank != 0) positions << rank;
            positions << ',' << (rank != 0 ? 1 : 0) << ',' << results.size() << '\n';
        }
        std::ofstream summary(output / "summary.json");
        summary.exceptions(std::ios::failbit | std::ios::badbit);
        summary.imbue(std::locale::classic());
        summary
            << std::setprecision(17)
            << "{\n  \"scope\": \"preliminary_only_not_CP05_certification\",\n"
            << "  \"tokenizer\": \"ascii-tech-v1\",\n  \"k1\": 1.2,\n  \"b\": 0.75,\n  \"k\": 10,\n"
            << "  \"documents\": " << index.document_count()
            << ",\n  \"vocabulary_size\": " << index.inverted_index().vocabulary_size()
            << ",\n  \"total_tokens\": " << index.inverted_index().total_token_count()
            << ",\n  \"average_document_length\": " << index.average_document_length()
            << ",\n  \"queries\": " << queries.size() << ",\n  \"hits_at_10\": " << hits
            << ",\n  \"hit_rate_at_10\": " << static_cast<double>(hits) / queries.size() << "\n}\n";
        rankings.close();
        positions.close();
        summary.close();
        std::cout << "Preliminary BM25: documents=" << index.document_count()
                  << "; vocabulary=" << index.inverted_index().vocabulary_size()
                  << "; HitRate@10=" << hits << '/' << queries.size()
                  << "; report=" << output.string() << ". CP-05 certification is separate.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
