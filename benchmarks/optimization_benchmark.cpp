#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/application/query_engine.hpp"
#include "aether/distance/kernels.hpp"

namespace {
using namespace aetherindex;
using Clock = std::chrono::steady_clock;
using Dataset = std::vector<std::vector<float>>;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
double ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::ofstream report(const std::filesystem::path& path) {
    std::ofstream file(path);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.imbue(std::locale::classic());
    file << std::setprecision(17);
    return file;
}

double percentile(std::vector<double> times, double fraction) {
    std::sort(times.begin(), times.end());
    return times[static_cast<std::size_t>(std::ceil(fraction * times.size())) - 1];
}
double mean(const std::vector<double>& times) {
    double total = 0;
    for (double time : times) total += time;
    return total / times.size();
}
std::uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
#elif defined(__linux__)
    std::ifstream file("/proc/self/statm");
    std::uint64_t virtual_pages = 0, resident_pages = 0;
    if (file >> virtual_pages >> resident_pages)
        return resident_pages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}
VectorId id_for(std::size_t row) { return static_cast<VectorId>(row * 7 + 10); }
Dataset uniform_data(std::size_t count, std::size_t dimension, std::uint32_t seed) {
    std::mt19937 generator(seed);
    Dataset data(count, std::vector<float>(dimension));
    for (auto& vector : data)
        for (float& value : vector) {
            value =
                static_cast<float>(static_cast<int>(generator() % 2000001) - 1000000) / 1000000.0f;
        }
    return data;
}
void populate(VectorStore& store, const Dataset& data) {
    store.reserve(data.size());
    for (std::size_t row = 0; row < data.size(); ++row) {
        check(store.insert(id_for(row), data[row]) == InsertStatus::Success,
              "Benchmark fixture insertion failed.");
    }
}

std::vector<SearchResult> full_sort(const VectorStore& store, std::span<const float> raw,
                                    std::size_t k) {
    const PreparedQuery query(raw, store.dimension(), store.distance_kind());
    const auto distance = detail::distance_kernel(store.distance_kind());
    std::vector<std::pair<float, VectorId>> scored;
    scored.reserve(store.size());
    for (std::size_t row = 0; row < store.size(); ++row) {
        scored.emplace_back(
            distance(query.data(), store.data_by_position(row), store.padded_dimension()),
            store.ids()[row]);
    }
    std::sort(scored.begin(), scored.end());
    std::vector<SearchResult> result;
    result.reserve(std::min(k, scored.size()));
    for (std::size_t i = 0; i < std::min(k, scored.size()); ++i)
        result.push_back({scored[i].second, scored[i].first});
    return result;
}

void topk(const std::filesystem::path& directory) {
    constexpr std::size_t count = 100000, dimension = 128, k = 10;
    VectorStore store(dimension);
    populate(store, uniform_data(count, dimension, 42));
    const auto queries = uniform_data(10, dimension, 424242);
    QueryEngine engine;
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        checksum += full_sort(store, queries[i], k)[0].id;
        checksum += engine.search(store, queries[i], k)[0].id;
    }
    auto rows = report(directory / "topk_samples.csv");
    rows << "cycle,query_id,full_sort_ms,max_heap_ms\n";
    std::vector<double> sort_times, heap_times;
    for (std::size_t cycle = 0; cycle < 5; ++cycle)
        for (std::size_t q = 0; q < queries.size(); ++q) {
            std::vector<SearchResult> sorted, heap;
            double sort_time = 0, heap_time = 0;
            const auto run_sort = [&] {
                const auto start = Clock::now();
                sorted = full_sort(store, queries[q], k);
                sort_time = ms(Clock::now() - start);
            };
            const auto run_heap = [&] {
                const auto start = Clock::now();
                heap = engine.search(store, queries[q], k);
                heap_time = ms(Clock::now() - start);
            };
            if ((cycle + q) % 2 == 0) {
                run_sort();
                run_heap();
            } else {
                run_heap();
                run_sort();
            }
            check(sorted.size() == heap.size(), "TopK size mismatch.");
            for (std::size_t rank = 0; rank < k; ++rank) {
                check(sorted[rank].id == heap[rank].id &&
                          sorted[rank].distance == heap[rank].distance,
                      "TopK optimization changed exact results.");
                checksum += heap[rank].id;
            }
            rows << cycle << ',' << q << ',' << sort_time << ',' << heap_time << '\n';
            sort_times.push_back(sort_time);
            heap_times.push_back(heap_time);
        }
    const double reduction = 1 - mean(heap_times) / mean(sort_times);
    auto summary = report(directory / "topk.json");
    summary << "{\n\"vectors\":100000,\"dimension\":128,\"k\":10,\"samples_per_algorithm\":50,"
            << "\n\"kernel\":\"" << detail::distance_kernel_backend() << "\","
            << "\n\"full_sort_mean_ms\":" << mean(sort_times)
            << ",\"heap_mean_ms\":" << mean(heap_times)
            << ",\n\"full_sort_median_ms\":" << percentile(sort_times, .5)
            << ",\"heap_median_ms\":" << percentile(heap_times, .5)
            << ",\n\"relative_time_reduction\":" << reduction
            << ",\"ca_1_3_pass\":" << (reduction > .60 ? "true" : "false")
            << ",\"checksum\":" << checksum << "\n}\n";
    std::cout << "TopK reduction=" << reduction << " sort_ms=" << mean(sort_times)
              << " heap_ms=" << mean(heap_times) << '\n';
}

void memory(const std::filesystem::path& directory) {
    VectorStore store(8);
    store.reserve(100000);
    std::mt19937 generator(42);
    std::array<float, 8> values;
    for (std::size_t row = 0; row < 100000; ++row) {
        for (float& value : values)
            value =
                static_cast<float>(static_cast<int>(generator() % 2000001) - 1000000) / 1000000.0f;
        check(store.insert(id_for(row), values) == InsertStatus::Success,
              "Memory fixture insertion failed.");
    }
    const auto before = resident_bytes();
    const auto start = Clock::now();
    HnswIndex index(store);
    for (VectorId id : store.ids()) index.insert(id);
    const double build_ms = ms(Clock::now() - start);
    const auto after = resident_bytes();
    check(index.validate_graph().valid(), "Memory benchmark graph is invalid.");
    const auto usage = index.memory_usage();
    auto summary = report(directory / "memory.json");
    summary << "{\n\"vectors\":100000,\"dimension\":8,\"m\":16,\"m0\":32,\"seed\":42,"
            << "\n\"layer0_allocated_bytes\":" << usage.layer0_bytes
            << ",\"upper_allocated_bytes\":" << usage.upper_bytes
            << ",\"upper_occupied_slots\":" << usage.upper_slots
            << ",\n\"metadata_allocated_bytes\":" << usage.metadata_bytes
            << ",\"object_bytes\":" << usage.object_bytes
            << ",\"graph_allocated_bytes\":" << usage.total_bytes()
            << ",\"global_buffer_blocks\":" << usage.allocation_blocks
            << ",\n\"rss_before_graph_bytes\":" << before << ",\"rss_after_graph_bytes\":" << after
            << ",\"rss_delta_bytes\":"
            << static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before)
            << ",\n\"rss_scope\":\"process delta; includes allocator and temporary-workspace "
               "retention, not equivalent to owned graph bytes\","
            << "\n\"build_ms\":" << build_ms << ",\"ca_2_2_owned_capacity_pass\":"
            << (usage.total_bytes() <= 14500000 ? "true" : "false") << "\n}\n";
    std::cout << "Graph bytes=" << usage.total_bytes() << " blocks=" << usage.allocation_blocks
              << " RSS delta="
              << static_cast<std::int64_t>(after) - static_cast<std::int64_t>(before) << '\n';
}

// Explicit Box-Muller generator avoids implementation-specific normal_distribution.
// This double arithmetic generates fixtures only; production distances are FP32.
class Gaussian {
   public:
    explicit Gaussian(std::uint64_t seed) : generator_(seed) {}
    float next() {
        if (has_spare_) {
            has_spare_ = false;
            return spare_;
        }
        const double u = (static_cast<double>(generator_() >> 11) + .5) / 9007199254740992.0;
        const double v = (static_cast<double>(generator_() >> 11) + .5) / 9007199254740992.0;
        const double radius = std::sqrt(-2 * std::log(u));
        const double angle = 6.2831853071795864769 * v;
        spare_ = static_cast<float>(radius * std::sin(angle));
        has_spare_ = true;
        return static_cast<float>(radius * std::cos(angle));
    }

   private:
    std::mt19937_64 generator_;
    float spare_ = 0;
    bool has_spare_ = false;
};

double recall(const std::vector<SearchResult>& exact,
              const std::vector<SearchResult>& approximate) {
    std::size_t matches = 0;
    for (const auto& result : approximate) {
        if (std::find_if(exact.begin(), exact.end(),
                         [&](const auto& item) { return item.id == result.id; }) != exact.end())
            ++matches;
    }
    return static_cast<double>(matches) / exact.size();
}
void write_ids(std::ostream& output, const std::vector<SearchResult>& results) {
    for (std::size_t rank = 0; rank < results.size(); ++rank) {
        if (rank) output << ' ';
        output << results[rank].id;
    }
}
void save_fixture(const std::filesystem::path& path, const Dataset& data) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    for (const auto& vector : data)
        file.write(reinterpret_cast<const char*>(vector.data()), vector.size() * sizeof(float));
}

void clustered(const std::filesystem::path& directory) {
    constexpr std::size_t dimension = 128, clusters = 100;
    Gaussian data_random(42), query_random(424242);
    Dataset centers(clusters, std::vector<float>(dimension));
    for (auto& center : centers)
        for (float& value : center) value = data_random.next();
    Dataset data(10000, std::vector<float>(dimension)),
        queries(1000, std::vector<float>(dimension));
    for (std::size_t row = 0; row < data.size(); ++row)
        for (std::size_t d = 0; d < dimension; ++d) {
            data[row][d] = centers[row / 100][d] + .015f * data_random.next();
        }
    for (std::size_t q = 0; q < queries.size(); ++q)
        for (std::size_t d = 0; d < dimension; ++d) {
            queries[q][d] = centers[q / 10][d] + .015f * query_random.next();
        }
    save_fixture(directory / "cluster_vectors.f32", data);
    save_fixture(directory / "cluster_queries.f32", queries);
    VectorStore store(dimension);
    populate(store, data);
    HnswConfig config;
    config.ef_search = 30;
    config.neighbor_selection = NeighborSelection::Simple;
    HnswIndex simple(store, config);
    config.neighbor_selection = NeighborSelection::Heuristic;
    HnswIndex heuristic(store, config);
    auto start = Clock::now();
    for (VectorId id : store.ids()) simple.insert(id);
    const double simple_build = ms(Clock::now() - start);
    start = Clock::now();
    for (VectorId id : store.ids()) heuristic.insert(id);
    const double heuristic_build = ms(Clock::now() - start);
    check(simple.validate_graph().valid() && heuristic.validate_graph().valid(),
          "Clustered graph is invalid.");
    QueryEngine engine;
    auto rows = report(directory / "clustered_queries.csv");
    rows << "query_id,cluster,simple_recall,heuristic_recall,exact_ids,simple_ids,heuristic_ids\n";
    double simple_sum = 0, heuristic_sum = 0;
    for (std::size_t q = 0; q < queries.size(); ++q) {
        const auto exact = engine.search(store, queries[q], 10);
        const auto first = engine.search(simple, queries[q], 10);
        const auto second = engine.search(heuristic, queries[q], 10);
        const double a = recall(exact, first), b = recall(exact, second);
        simple_sum += a;
        heuristic_sum += b;
        rows << q << ',' << q / 10 << ',' << a << ',' << b << ',';
        write_ids(rows, exact);
        rows << ',';
        write_ids(rows, first);
        rows << ',';
        write_ids(rows, second);
        rows << '\n';
    }
    const double gain = (heuristic_sum - simple_sum) / queries.size();
    auto summary = report(directory / "clustered.json");
    summary << "{\n\"vectors\":10000,\"dimension\":128,\"queries\":1000,\"ef_search\":30,\"k\":10,"
            << "\n\"simple_mean_recall\":" << simple_sum / queries.size()
            << ",\"heuristic_mean_recall\":" << heuristic_sum / queries.size()
            << ",\"absolute_recall_gain\":" << gain << ",\"required_absolute_gain\":0.05,"
            << "\n\"simple_build_ms\":" << simple_build
            << ",\"heuristic_build_ms\":" << heuristic_build
            << ",\"ca_2_4_pass\":" << (gain >= .05 ? "true" : "false") << "\n}\n";
    std::cout << "Clustered recall simple=" << simple_sum / queries.size()
              << " heuristic=" << heuristic_sum / queries.size() << " absolute_gain=" << gain
              << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3)
            throw std::invalid_argument(
                "Usage: aether_optimization_benchmark topk|memory|clustered OUTPUT_DIR");
        const std::filesystem::path directory = argv[2];
        std::filesystem::create_directories(directory);
        const std::string mode = argv[1];
        if (mode == "topk")
            topk(directory);
        else if (mode == "memory")
            memory(directory);
        else if (mode == "clustered")
            clustered(directory);
        else
            throw std::invalid_argument("Unknown benchmark mode.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
