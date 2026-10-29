#pragma once

#include <span>
#include <string_view>

#include "aether/application/search_service.hpp"

namespace aetherindex::cli {

struct CliOptions {
    bool show_help = false;
    bool convert = false;  // "convert": embeddings to .avec instead of a search.
    SearchRequest search;
    ConvertRequest conversion;
};

// Arguments exclude argv[0]. Throws invalid_argument on malformed options.
CliOptions parse_arguments(std::span<const std::string_view> arguments);
std::string_view help_text();

}  // namespace aetherindex::cli
