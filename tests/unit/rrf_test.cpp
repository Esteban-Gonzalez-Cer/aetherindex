#include "aether/hybrid/rrf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "../test_helpers.hpp"

using namespace aetherindex;
using namespace aetherindex::hybrid;

namespace {
using Ranking = std::vector<DocumentId>;
using Results = std::vector<RankedResult>;
constexpr double tolerance = 1e-12;

// Independent oracle: enumerates the union, then finds each document's
// position by scanning the original lists. No production helper/hash scores.
Results reference(std::span<const DocumentId> dense, std::span<const DocumentId> lexical,
                  double constant, std::size_t top_k) {
    std::set<DocumentId> ids(dense.begin(), dense.end());
    ids.insert(lexical.begin(), lexical.end());
    Results result;
    for (DocumentId id : ids) {
        long double score = 0;
        for (auto list : {dense, lexical}) {
            const auto found = std::find(list.begin(), list.end(), id);
            if (found != list.end()) {
                score +=
                    1.0L / (static_cast<long double>(constant) +
                            static_cast<long double>(std::distance(list.begin(), found)) + 1.0L);
            }
        }
        result.push_back({id, static_cast<double>(score)});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.score != b.score ? a.score > b.score : a.document_id < b.document_id;
    });
    if (result.size() > top_k) result.resize(top_k);
    return result;
}

void check_order(std::span<const RankedResult> results) {
    std::set<DocumentId> unique;
    for (std::size_t i = 0; i < results.size(); ++i) {
        require(std::isfinite(results[i].score) && results[i].score > 0,
                "RRF score must be finite and positive.");
        require(unique.insert(results[i].document_id).second, "Duplicate fused DocumentId.");
        if (i > 0) {
            require(results[i - 1].score > results[i].score ||
                        (results[i - 1].score == results[i].score &&
                         results[i - 1].document_id < results[i].document_id),
                    "RRF is not ordered by score descending then ID ascending.");
        }
    }
}

void compare(std::span<const RankedResult> actual, std::span<const RankedResult> expected) {
    require(actual.size() == expected.size(), "Unexpected RRF result count.");
    check_order(actual);
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(actual[i].document_id == expected[i].document_id,
                "Unexpected fused document order.");
        require(std::isfinite(expected[i].score) &&
                    std::abs(actual[i].score - expected[i].score) <= tolerance,
                "RRF numerical error exceeds 1e-12.");
    }
}

std::string json_quote(std::string_view text) {
    std::string result = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\') result += '\\';
        if (ch == '\n')
            result += "\\n";
        else if (ch == '\r')
            result += "\\r";
        else if (ch == '\t')
            result += "\\t";
        else
            result += ch;
    }
    return result + '"';
}

struct Observation {
    Ranking dense;
    Ranking lexical;
    double constant;
    std::size_t top_k;
    Results expected;
    Results actual;
    bool invalid_expected;
    std::string actual_error;
};

struct Evidence {
    std::string id;
    std::vector<Observation> observations;
    std::size_t property_trials = 0;
    std::size_t repetitions = 0;

    void run(Ranking dense, Ranking lexical, double constant = 60, std::size_t top_k = 10,
             bool invalid = false, Results expected = {}) {
        if (!invalid && expected.empty()) expected = reference(dense, lexical, constant, top_k);
        observations.push_back({dense, lexical, constant, top_k, expected, {}, invalid, {}});
        auto& entry = observations.back();
        try {
            const RrfFusion fusion({constant});
            entry.actual = fusion.fuse(dense, lexical, top_k);
        } catch (const std::invalid_argument&) {
            entry.actual_error = "std::invalid_argument";
        }
        require(dense == entry.dense && lexical == entry.lexical, "RRF modified an input ranking.");
        if (invalid) {
            require(entry.actual_error == "std::invalid_argument",
                    "Invalid input/config accepted.");
        } else {
            require(entry.actual_error.empty(), "Valid RRF input rejected.");
            compare(entry.actual, entry.expected);
        }
    }

    void save(const std::filesystem::path& path, bool passed, const std::string& error) const {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.imbue(std::locale::classic());
        output << std::setprecision(17);
        const auto results = [&](const Results& list) {
            output << '[';
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (i) output << ',';
                output << "{\"document_id\":" << list[i].document_id
                       << ",\"score\":" << list[i].score << '}';
            }
            output << ']';
        };
        const auto ids = [&](const Ranking& list) {
            output << '[';
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (i) output << ',';
                output << list[i];
            }
            output << ']';
        };
        output << "{\n\"case_id\":" << json_quote(id)
               << ",\n\"passed\":" << (passed ? "true" : "false")
               << ",\n\"error\":" << json_quote(error) << ",\n\"absolute_tolerance\":1e-12"
               << ",\n\"property_trials\":" << property_trials
               << ",\n\"repetitions\":" << repetitions << ",\n\"checks\":[";
        for (std::size_t i = 0; i < observations.size(); ++i) {
            if (i) output << ',';
            const auto& entry = observations[i];
            output << "{\"dense\":";
            ids(entry.dense);
            output << ",\"lexical\":";
            ids(entry.lexical);
            output << ",\"k_rrf\":";
            if (std::isfinite(entry.constant))
                output << entry.constant;
            else
                output << json_quote(std::isnan(entry.constant) ? "NaN"
                                     : entry.constant > 0       ? "+Inf"
                                                                : "-Inf");
            output << ",\"top_k\":" << entry.top_k << ",\"expected_error\":"
                   << (entry.invalid_expected ? "\"std::invalid_argument\"" : "null")
                   << ",\"actual_error\":"
                   << (entry.actual_error.empty() ? "null" : json_quote(entry.actual_error))
                   << ",\"expected\":";
            results(entry.expected);
            output << ",\"actual\":";
            results(entry.actual);
            output << '}';
        }
        output << "]\n}\n";
        output.close();
    }
};

void run_properties(Evidence& report) {
    const auto maximum = std::numeric_limits<DocumentId>::max();
    report.run({maximum, 0, 427}, {0, maximum, 91});
    const Ranking same = {9, 1, 7, 2};
    const RrfFusion fusion;
    compare(fusion.fuse(same, same, 100), reference(same, same, 60, 100));
    const auto view = std::span<const DocumentId>(same);
    compare(fusion.fuse(view.first(3), view.subspan(1), 100),
            reference(view.first(3), view.subspan(1), 60, 100));
    require(same == Ranking{9, 1, 7, 2}, "Overlapping input spans were modified.");
    for (double constant :
         {0.25, std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(),
          std::numeric_limits<double>::max()}) {
        report.run({0}, {0}, constant);
        const auto result = RrfFusion({constant}).fuse(Ranking{0}, {}, 1);
        require(result[0].score == 1.0 / (constant + 1.0) && result[0].score > 0,
                "Extreme finite configuration lost its positive contribution.");
    }
    RrfConfig original{10};
    const RrfFusion copied_config(original);
    original.k_rrf = 100;
    require(copied_config.config().k_rrf == 10 && fusion.config().k_rrf == 60,
            "Configuration was not copied or default was changed.");
    for (auto pair :
         {std::pair{Ranking{1, 2, 1}, Ranking{3}}, std::pair{Ranking{1}, Ranking{2, 3, 2}}}) {
        requireInvalidArgument([&] { static_cast<void>(fusion.fuse(pair.first, pair.second, 1)); },
                               "Duplicate beyond top_k was hidden.");
    }
    compare(fusion.fuse(Ranking{101}, {}, 1), Results{{101, 1.0 / 61.0}});
    requireInvalidArgument([&] { static_cast<void>(fusion.fuse({}, {}, 0)); },
                           "Zero top_k with empty lists was accepted.");

    std::mt19937 random(42);
    Ranking pool(128);
    std::iota(pool.begin(), pool.end(), 10u);
    for (auto& id : pool) id *= 37;
    pool[0] = 0;
    pool[1] = maximum;
    constexpr std::array constants = {0.25, 1.0, 10.0, 60.0, 100.0};
    for (std::size_t trial = 0; trial < 200; ++trial) {
        std::shuffle(pool.begin(), pool.end(), random);
        const auto dense_count = random() % 65;
        Ranking dense(pool.begin(), pool.begin() + dense_count);
        std::shuffle(pool.begin(), pool.end(), random);
        const auto lexical_count = random() % 65;
        Ranking lexical(pool.begin(), pool.begin() + lexical_count);
        const Ranking dense_before = dense, lexical_before = lexical;
        const double constant = constants[trial % constants.size()];
        const RrfFusion subject({constant});
        const auto full = subject.fuse(dense, lexical, 1000);
        const auto expected = reference(dense, lexical, constant, 1000);
        require(full.size() == expected.size(), "Union cardinality differs from independent scan.");
        check_order(full);
        for (const auto& item : full) {
            const auto found = std::find_if(
                expected.begin(), expected.end(),
                [&](const auto& other) { return item.document_id == other.document_id; });
            require(found != expected.end() && std::abs(item.score - found->score) <= tolerance,
                    "Independent union/position oracle disagrees.");
        }
        const std::size_t k = random() % 100 + 1;
        const Results prefix(full.begin(), full.begin() + std::min(k, full.size()));
        compare(subject.fuse(dense, lexical, k), prefix);
        compare(subject.fuse(lexical, dense, 1000), full);
        require(dense == dense_before && lexical == lexical_before,
                "Input mutation in property trial.");
        ++report.property_trials;
    }
}

void run_case(Evidence& report) {
    const Ranking dense = {101, 102, 103}, lexical = {102, 104, 101};
    const auto& id = report.id;
    if (id == "RRF-01") {
        report.run(dense, lexical, 60, 10, false,
                   {{102, 1.0 / 62 + 1.0 / 61},
                    {101, 1.0 / 61 + 1.0 / 63},
                    {104, 1.0 / 62},
                    {103, 1.0 / 63}});
    } else if (id == "RRF-02")
        report.run(dense, dense);
    else if (id == "RRF-03")
        report.run({101, 102, 103}, {103, 102, 101});
    else if (id == "RRF-04")
        report.run({101, 102}, {103, 104});
    else if (id == "RRF-05")
        report.run(dense, {});
    else if (id == "RRF-06")
        report.run({}, lexical);
    else if (id == "RRF-07")
        report.run({}, {});
    else if (id == "RRF-08")
        report.run(dense, lexical, 60, 1);
    else if (id == "RRF-09") {
        for (std::size_t k : {2u, 3u, 4u}) report.run(dense, lexical, 60, k);
    } else if (id == "RRF-10") {
        report.run(dense, lexical, 60, 10);
        report.run(dense, lexical, 60, std::numeric_limits<std::size_t>::max());
    } else if (id == "RRF-11")
        report.run({101, 102, 101}, {103, 104}, 60, 10, true);
    else if (id == "RRF-12")
        report.run({103, 104}, {101, 102, 101}, 60, 10, true);
    else if (id == "RRF-13")
        report.run({101}, {101});
    else if (id == "RRF-14")
        report.run({200, 100}, {100, 200});
    else if (id == "RRF-15") {
        report.run({101}, {}, 1);
        report.run({101}, {}, 10);
    } else if (id == "RRF-16")
        report.run({101}, {}, 100);
    else if (id == "RRF-17") {
        report.run({}, {}, 0, 10, true);
        report.run({}, {}, -0.0, 10, true);
    } else if (id == "RRF-18")
        report.run({}, {}, -10, 10, true);
    else if (id == "RRF-19") {
        for (double value :
             {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
              -std::numeric_limits<double>::infinity()})
            report.run({}, {}, value, 10, true);
    } else if (id == "RRF-20")
        report.run(dense, lexical, 60, 0, true);
    else if (id == "RRF-21") {
        report.run(dense, lexical);
        const RrfFusion fusion;
        const auto first = fusion.fuse(dense, lexical, 10);
        const Ranking tied_dense = {200, 100}, tied_lexical = {100, 200};
        report.run(tied_dense, tied_lexical);
        const auto first_tie = fusion.fuse(tied_dense, tied_lexical, 10);
        for (std::size_t repeat = 0; repeat < 50; ++repeat) {
            const auto next = fusion.fuse(dense, lexical, 10);
            compare(next, first);
            const auto next_tie = fusion.fuse(tied_dense, tied_lexical, 10);
            compare(next_tie, first_tie);
            for (std::size_t i = 0; i < next.size(); ++i) {
                require(next[i].score == first[i].score, "Repeat changed exact double score.");
            }
            for (std::size_t i = 0; i < next_tie.size(); ++i) {
                require(next_tie[i].score == first_tie[i].score, "Repeat changed exact tie score.");
            }
            ++report.repetitions;
        }
    } else if (id == "RRF-22")
        report.run({427, 0, 200}, {200, 91, 427});
    else if (id == "properties")
        run_properties(report);
    else
        throw std::invalid_argument("Unknown RRF test case.");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: aether_rrf_tests RRF-01..RRF-22|properties [REPORT.json]\n";
        return 1;
    }
    Evidence report{argv[1], {}};
    bool passed = false;
    std::string error;
    try {
        run_case(report);
        passed = true;
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    if (argc == 3) {
        try {
            report.save(argv[2], passed, error);
        } catch (const std::exception& exception) {
            std::cerr << exception.what() << '\n';
            return 1;
        }
    }
    std::cout << report.id << (passed ? " PASS" : " FAIL")
              << "; checks=" << report.observations.size()
              << "; property_trials=" << report.property_trials
              << "; repetitions=" << report.repetitions << (error.empty() ? "" : "; " + error)
              << '\n';
    return passed ? 0 : 1;
}
