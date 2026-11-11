#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aetherindex {

// One metadata condition. Equality: the document's field equals one of `values`.
// Date range: the field is a YYYY-MM-DD date within [from_day, to_day] (days since
// 1970-01-01, either bound optional). A document without the field never matches.
struct FilterClause {
    std::string field;
    std::vector<std::string> values;
    bool date_range = false;
    std::optional<std::int64_t> from_day{};
    std::optional<std::int64_t> to_day{};
};

// "field=value" or "field=v1|v2" (any of). Throws invalid_argument on bad syntax.
FilterClause parse_equality_filter(std::string_view text);
// "field=FROM..TO" with YYYY-MM-DD dates; either side may be empty, not both.
FilterClause parse_date_filter(std::string_view text);
// Proleptic Gregorian YYYY-MM-DD to days since 1970-01-01; nullopt if invalid.
std::optional<std::int64_t> parse_date(std::string_view text);

using Metadata = std::vector<std::pair<std::string, std::string>>;

// All clauses must hold (logical AND).
bool matches_filters(const Metadata& metadata, const std::vector<FilterClause>& clauses);

}  // namespace aetherindex
