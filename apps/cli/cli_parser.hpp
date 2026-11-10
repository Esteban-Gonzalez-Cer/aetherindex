#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <string_view>

#include "aether/application/search_service.hpp"

namespace aetherindex::cli {

enum class Command {
    Search,
    Convert,
    IndexBuild,
    IndexInfo,
    IndexVerify,
    IndexAdd,
    IndexDelete,
    IndexCompact,
    IndexCheckpoint
};

struct CliOptions {
    bool show_help = false;
    bool convert = false;  // "convert": embeddings to .avec instead of a search.
    Command command = Command::Search;
    SearchRequest search;
    ConvertRequest conversion;
    // search --index DIR, index info|verify --index DIR, index build --out DIR.
    std::filesystem::path index_path;
    bool verify_checksums = true;  // --no-verify skips payload checksums when opening.
    // search --index: query-time options given explicitly, checked once the index's mode
    // is known (for example, --alpha requires a hybrid index).
    std::vector<std::string> index_query_options;
    // index add: records from search.dataset_path (and search.vectors_path); --replace
    // updates existing IDs. index delete: the --id values.
    bool replace = false;
    std::vector<std::string> delete_ids;
    bool sync = true;  // --no-sync: log records are not flushed to stable storage.
};

// Arguments exclude argv[0]. Throws invalid_argument on malformed options.
CliOptions parse_arguments(std::span<const std::string_view> arguments);
std::string_view help_text();

}  // namespace aetherindex::cli
