#include "aether/application/filter.hpp"

#include <algorithm>
#include <stdexcept>

namespace aetherindex {
namespace {

std::pair<std::string, std::string> split_field(std::string_view text, const char* option) {
    const auto equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0)
        throw std::invalid_argument(std::string("Invalid ") + option + ": expected field=value.");
    return {std::string(text.substr(0, equals)), std::string(text.substr(equals + 1))};
}

int digits(std::string_view text, std::size_t start, std::size_t count) {
    int value = 0;
    for (std::size_t i = start; i < start + count; ++i) {
        if (text[i] < '0' || text[i] > '9') return -1;
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

}  // namespace

std::optional<std::int64_t> parse_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return std::nullopt;
    const int year = digits(text, 0, 4), month = digits(text, 5, 2), day = digits(text, 8, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1) return std::nullopt;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    constexpr int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (day > lengths[month - 1] + (month == 2 && leap)) return std::nullopt;
    // Days from civil (H. Hinnant), exact for the proleptic Gregorian calendar.
    const std::int64_t y = year - (month <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

FilterClause parse_equality_filter(std::string_view text) {
    auto [field, value] = split_field(text, "--filter");
    FilterClause clause;
    clause.field = std::move(field);
    std::size_t start = 0;
    for (;;) {
        const auto bar = value.find('|', start);
        auto alternative =
            value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (alternative.empty())
            throw std::invalid_argument("Invalid --filter: empty value in '" + std::string(text) +
                                        "'.");
        clause.values.push_back(std::move(alternative));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return clause;
}

FilterClause parse_date_filter(std::string_view text) {
    auto [field, range] = split_field(text, "--filter-date");
    const auto dots = range.find("..");
    if (dots == std::string::npos)
        throw std::invalid_argument("Invalid --filter-date: expected field=FROM..TO.");
    FilterClause clause;
    clause.field = std::move(field);
    clause.date_range = true;
    const auto from = range.substr(0, dots), to = range.substr(dots + 2);
    if (from.empty() && to.empty())
        throw std::invalid_argument("Invalid --filter-date: give FROM, TO or both.");
    const auto bound = [&](const std::string& value) -> std::optional<std::int64_t> {
        if (value.empty()) return std::nullopt;
        const auto day = parse_date(value);
        if (!day)
            throw std::invalid_argument("Invalid --filter-date: '" + value +
                                        "' is not a valid YYYY-MM-DD date.");
        return day;
    };
    clause.from_day = bound(from);
    clause.to_day = bound(to);
    if (clause.from_day && clause.to_day && *clause.from_day > *clause.to_day)
        throw std::invalid_argument("Invalid --filter-date: FROM is after TO.");
    return clause;
}

bool matches_filters(const Metadata& metadata, const std::vector<FilterClause>& clauses) {
    for (const auto& clause : clauses) {
        const auto found = std::find_if(metadata.begin(), metadata.end(), [&](const auto& entry) {
            return entry.first == clause.field;
        });
        if (found == metadata.end()) return false;
        if (clause.date_range) {
            const auto day = parse_date(found->second);
            if (!day || (clause.from_day && *day < *clause.from_day) ||
                (clause.to_day && *day > *clause.to_day))
                return false;
        } else if (std::find(clause.values.begin(), clause.values.end(), found->second) ==
                   clause.values.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace aetherindex
