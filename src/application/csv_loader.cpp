#include "csv_loader.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "aether/application/search_service.hpp"

namespace aetherindex::detail {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

std::string location(const std::filesystem::path& path, std::size_t line) {
    return path.string() + ":line " + std::to_string(line) + ": ";
}

std::vector<std::string_view> fields(std::string_view row) {
    std::vector<std::string_view> result;
    for (;;) {
        const auto comma = row.find(',');
        result.push_back(trim(row.substr(0, comma)));
        if (comma == std::string_view::npos) {
            return result;
        }
        row.remove_prefix(comma + 1);
    }
}

VectorId parse_id(std::string_view text, const std::string& prefix) {
    VectorId id = 0;
    if (!text.empty()) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
        if (error == std::errc{} && end == text.data() + text.size()) {
            return id;
        }
    }
    throw InputDataError(prefix + "column 1: expected an unsigned vector ID in [0, 4294967295].");
}

EmbeddingValue parse_component(std::string_view text, const std::string& prefix,
                               std::size_t column) {
    // Classic-locale streams keep CSV decimal syntax independent of the user's locale.
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    EmbeddingValue value = 0;
    input >> std::noskipws >> value;
    if (text.empty() || !input || !input.eof() || !std::isfinite(value)) {
        throw InputDataError(prefix + "column " + std::to_string(column) +
                             ": expected a finite float (decimal or scientific notation).");
    }
    return value;
}

std::vector<EmbeddingValue> parse_embedding(std::span<const std::string_view> columns,
                                            const std::string& prefix, std::size_t first_column) {
    std::vector<EmbeddingValue> embedding;
    embedding.reserve(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i) {
        embedding.push_back(parse_component(columns[i], prefix, first_column + i));
    }
    return embedding;
}

std::ifstream open_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        throw InputDataError("Cannot open input file (expected a readable regular file): " +
                             path.string());
    }
    std::ifstream input(path);
    if (!input) {
        throw InputDataError("Cannot open input file: " + path.string());
    }
    return input;
}

void check_read(const std::ifstream& input, const std::filesystem::path& path) {
    if (input.bad() || (input.fail() && !input.eof())) {
        throw InputDataError("Cannot read input file: " + path.string());
    }
}

}  // namespace

std::unique_ptr<VectorStore> load_csv_dataset(const std::filesystem::path& path,
                                              DistanceKind metric) {
    auto input = open_file(path);
    std::unique_ptr<VectorStore> store;
    std::string row;
    std::size_t line = 0;
    while (std::getline(input, row)) {
        ++line;
        if (trim(row).empty()) {
            continue;
        }
        const auto prefix = location(path, line);
        const auto columns = fields(row);
        if (columns.size() < 2) {
            throw InputDataError(prefix + "expected an ID and at least one component.");
        }
        const auto id = parse_id(columns.front(), prefix);
        const auto embedding = parse_embedding(std::span(columns).subspan(1), prefix, 2);
        if (!store) {
            store = std::make_unique<VectorStore>(embedding.size(), metric);
        }
        switch (store->insert(id, embedding)) {
            case InsertStatus::Success:
                break;
            case InsertStatus::DuplicateId:
                throw InputDataError(prefix + "duplicate vector ID " + std::to_string(id) + ".");
            case InsertStatus::DimensionMismatch:
                throw InputDataError(prefix + "expected dimension " +
                                     std::to_string(store->dimension()) + ", found " +
                                     std::to_string(embedding.size()) + ".");
            case InsertStatus::InvalidEmbedding:
                throw InputDataError(prefix + "cosine metric does not accept all-zero embeddings.");
        }
    }
    check_read(input, path);
    if (!store) {
        throw InputDataError("Dataset is empty: " + path.string());
    }
    return store;
}

std::vector<EmbeddingValue> load_csv_query(const std::filesystem::path& path, std::size_t dimension,
                                           DistanceKind metric) {
    auto input = open_file(path);
    std::vector<EmbeddingValue> query;
    std::string row;
    std::size_t line = 0;
    while (std::getline(input, row)) {
        ++line;
        if (trim(row).empty()) {
            continue;
        }
        const auto prefix = location(path, line);
        if (!query.empty()) {
            throw InputDataError(prefix +
                                 "query file must contain exactly one embedding without an ID.");
        }
        const auto columns = fields(row);
        query = parse_embedding(columns, prefix, 1);
        if (query.size() != dimension) {
            throw InputDataError(prefix + "query dimension mismatch: expected " +
                                 std::to_string(dimension) + ", found " +
                                 std::to_string(query.size()) + ".");
        }
        bool nonzero = false;
        for (const auto value : query) {
            nonzero = nonzero || value != 0.0f;
        }
        if (metric == DistanceKind::Cosine && !nonzero) {
            throw InputDataError(prefix + "cosine metric does not accept an all-zero query.");
        }
    }
    check_read(input, path);
    if (query.empty()) {
        throw InputDataError("Query file is empty: " + path.string());
    }
    return query;
}

}  // namespace aetherindex::detail
