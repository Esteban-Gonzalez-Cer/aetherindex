// This driver also builds against the frozen legacy headers/libraries using
// -DAETHER_LEGACY_BASELINE. Keep its measured workload identical in both builds.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/query_engine.hpp"

namespace {
using namespace aetherindex;
using Clock = std::chrono::steady_clock;

void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}
double ms(Clock::duration time) { return std::chrono::duration<double, std::milli>(time).count(); }

std::vector<float> parse_values(std::string line, std::size_t start) {
    std::replace(line.begin(), line.end(), ',', ' ');
    std::istringstream input(line);
    input.imbue(std::locale::classic());
    if (start) {
        std::uint64_t id;
        input >> id;
    }
    std::vector<float> vector;
    float value;
    while (input >> value) {
        check(std::isfinite(value), "Nonfinite DBpedia component.");
        vector.push_back(value);
    }
    check(input.eof() && vector.size() == 1024, "DBpedia fixture must have dimension 1024.");
    return vector;
}

class PerfControl {
   public:
    PerfControl() {
        const char* control = std::getenv("AETHER_PERF_CTL_FIFO");
        const char* ack = std::getenv("AETHER_PERF_ACK_FIFO");
        if (!control && !ack) return;
        check(control && ack, "Both perf control/ack FIFO paths are required.");
        commands_.open(control);
        acknowledgments_.open(ack);
        check(commands_.is_open() && acknowledgments_.is_open(), "Cannot open perf control FIFOs.");
        enabled_ = true;
    }
    void command(const char* text) {
        if (!enabled_) return;
        commands_ << text << '\n' << std::flush;
        std::string ack;
        check(static_cast<bool>(std::getline(acknowledgments_, ack)) && ack == "ack",
              "Perf did not acknowledge the measurement boundary.");
    }

   private:
    std::ofstream commands_;
    std::ifstream acknowledgments_;
    bool enabled_ = false;
};

double overlap(const std::vector<SearchResult>& exact,
               const std::vector<SearchResult>& approximate) {
    std::size_t count = 0;
    for (const auto& result : approximate) {
        if (std::find_if(exact.begin(), exact.end(),
                         [&](const auto& x) { return x.id == result.id; }) != exact.end())
            ++count;
    }
    return count / 10.0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4)
            throw std::invalid_argument(
                "Usage: aether_dbpedia_benchmark DATA_DIR OUTPUT_DIR [heuristic|simple]");
        const std::filesystem::path data = argv[1], output = argv[2];
        std::filesystem::create_directories(output);
        const std::string selection = argc == 4 ? argv[3] : "heuristic";
        check(selection == "heuristic" || selection == "simple", "Invalid neighbor selection.");
#ifdef AETHER_LEGACY_BASELINE
        check(selection == "simple", "The legacy implementation supports only simple selection.");
#endif
        VectorStore store(1024);
#ifndef AETHER_LEGACY_BASELINE
        store.reserve(10000);
#endif
        std::ifstream vectors(data / "vectors.csv");
        vectors.imbue(std::locale::classic());
        check(vectors.is_open(), "Cannot open DBpedia vectors.csv.");
        std::string line;
        std::size_t row = 0;
        while (std::getline(vectors, line)) {
            const auto comma = line.find(',');
            check(comma != std::string::npos, "Missing DBpedia ID.");
            const auto id = static_cast<VectorId>(std::stoul(line.substr(0, comma)));
            check(id == 7 * row + 10, "DBpedia row-ID mapping changed.");
            const auto values = parse_values(line, 1);
            check(store.insert(id, values) == InsertStatus::Success, "DBpedia insertion failed.");
            ++row;
        }
        check(vectors.eof() && store.size() == 10000, "Expected exactly 10,000 DBpedia vectors.");
        std::vector<std::vector<float>> queries;
        for (std::size_t q = 0; q < 20; ++q) {
            std::ostringstream filename;
            filename << "query_" << std::setw(2) << std::setfill('0') << q << ".csv";
            std::ifstream file(data / "queries" / filename.str());
            check(static_cast<bool>(std::getline(file, line)), "Missing DBpedia query.");
            queries.push_back(parse_values(line, 0));
            check(!std::getline(file, line), "Extra DBpedia query row.");
        }
        HnswConfig config;
#ifndef AETHER_LEGACY_BASELINE
        config.neighbor_selection =
            selection == "simple" ? NeighborSelection::Simple : NeighborSelection::Heuristic;
#endif
        const auto build_start = Clock::now();
        HnswIndex index(store, config);
        for (VectorId id : store.ids()) index.insert(id);
        const double build_ms = ms(Clock::now() - build_start);
        check(index.validate_graph().valid(), "Invalid DBpedia graph.");
        QueryEngine engine;
        std::vector<std::vector<SearchResult>> exact;
        for (const auto& query : queries) exact.push_back(engine.search(store, query, 10));
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < 100; ++i)
            checksum += engine.search(index, queries[i % 20], 10).front().id;

        PerfControl perf;
        std::vector<double> times(1000);
        // Allocate reporting data before the counter boundary. Result-vector
        // allocations performed by QueryEngine remain part of the workload.
        std::vector<std::vector<SearchResult>> result_rows(1000);
        perf.command("enable");
        for (std::size_t i = 0; i < 1000; ++i) {
            const auto start = Clock::now();
            result_rows[i] = engine.search(index, queries[i % 20], 10);
            times[i] = ms(Clock::now() - start);
        }
        perf.command("disable");
        double recall_sum = 0;
        auto rows = std::ofstream(output / "dbpedia_samples.csv");
        rows.exceptions(std::ios::failbit | std::ios::badbit);
        rows.imbue(std::locale::classic());
        rows << std::setprecision(17) << "sample,query_id,query_ms,recall_at_10\n";
        for (std::size_t i = 0; i < times.size(); ++i) {
            const auto& result = result_rows[i];
            check(result.size() == 10, "DBpedia search returned fewer than ten results.");
            const double recall = overlap(exact[i % 20], result);
            recall_sum += recall;
            for (const auto& neighbor : result) {
                check(std::isfinite(neighbor.distance), "Invalid DBpedia result distance.");
                checksum += neighbor.id;
            }
            rows << i << ',' << i % 20 << ',' << times[i] << ',' << recall << '\n';
        }
        double total = 0;
        for (double time : times) total += time;
        std::sort(times.begin(), times.end());
        const double p99 = times[989];
        auto summary = std::ofstream(output / "dbpedia.json");
        summary.exceptions(std::ios::failbit | std::ios::badbit);
        summary.imbue(std::locale::classic());
        summary << std::setprecision(17)
                << "{\n\"vectors\":10000,\"dimension\":1024,\"distinct_queries\":20,"
                << "\"measured_searches\":1000,\"warmup_searches\":100,\"threads\":1,\"ef_search\":"
                   "50,\"k\":10,"
                << "\n\"selection\":\"" << selection << "\",\"build_ms\":" << build_ms
                << ",\"mean_query_ms\":" << total / times.size()
                << ",\"p50_query_ms\":" << times[499] << ",\"p95_query_ms\":" << times[949]
                << ",\"p99_query_ms\":" << p99 << ",\"max_query_ms\":" << times.back()
                << ",\n\"mean_recall_at_10\":" << recall_sum / times.size()
                << ",\"ca_2_5_pass\":" << (p99 < 1.5 ? "true" : "false")
                << ",\"checksum\":" << checksum
                << ",\n\"timing_scope\":\"QueryEngine including query preparation; graph "
                   "construction and CSV I/O excluded\","
                << "\n\"sample_limit\":\"20 distinct held-out queries repeated 50 times; local "
                   "warm-cache observation\"\n}\n";
        std::cout << "DBpedia " << selection << ": p99=" << p99
                  << " ms; mean_recall=" << recall_sum / times.size() << "; build=" << build_ms
                  << " ms\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
