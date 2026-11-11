#pragma once

// Shared helpers for the optimization benchmarks: seeded generators with explicit
// integer-to-real mappings (independent of std:: distribution implementations),
// nearest-rank percentiles and process memory probes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#include <sys/resource.h>

namespace aetherindex::bench {

using Clock = std::chrono::steady_clock;

inline double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Uniform (0, 1) from the top 53 bits; never 0, so log() is safe.
inline double uniform(std::mt19937_64& engine) {
    return (static_cast<double>(engine() >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

// Box-Muller with an explicit mapping, reproducible across standard libraries.
inline float gaussian(std::mt19937_64& engine) {
    const double u1 = uniform(engine), u2 = uniform(engine);
    return static_cast<float>(std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2));
}

struct MixtureSpec {
    std::size_t dimension = 128;
    std::size_t clusters = 100;
    double spread = 1.0;
    std::uint64_t seed = 42;
};

// Gaussian mixture rows; queries use the same centers with an independent seed.
class GaussianMixture {
   public:
    explicit GaussianMixture(const MixtureSpec& spec) : spec_(spec) {
        std::mt19937_64 engine(spec.seed ^ 0x9e3779b97f4a7c15ULL);
        centers_.assign(spec.clusters, std::vector<float>(spec.dimension));
        for (auto& center : centers_)
            for (auto& value : center) value = gaussian(engine);
    }
    std::vector<std::vector<float>> sample(std::size_t count, std::uint64_t seed) const {
        std::mt19937_64 engine(seed);
        std::vector<std::vector<float>> rows(count, std::vector<float>(spec_.dimension));
        for (auto& row : rows) {
            const auto& center = centers_[engine() % centers_.size()];
            for (std::size_t d = 0; d < spec_.dimension; ++d)
                row[d] = center[d] + static_cast<float>(spec_.spread) * gaussian(engine);
        }
        return rows;
    }

   private:
    MixtureSpec spec_;
    std::vector<std::vector<float>> centers_;
};

struct ZipfSpec {
    std::size_t vocabulary = 200000;
    double exponent = 1.07;
    std::size_t min_length = 20;
    std::size_t max_length = 200;
    std::uint64_t seed = 42;
};

// Zipf-distributed synthetic text over tokens "w<rank>" (performance only, not relevance).
class ZipfText {
   public:
    explicit ZipfText(const ZipfSpec& spec) : spec_(spec), cdf_(spec.vocabulary) {
        double total = 0;
        for (std::size_t r = 0; r < spec.vocabulary; ++r) {
            total += 1.0 / std::pow(static_cast<double>(r + 1), spec.exponent);
            cdf_[r] = total;
        }
        for (auto& value : cdf_) value /= total;
    }
    std::size_t rank(std::mt19937_64& engine) const {
        const auto found = std::lower_bound(cdf_.begin(), cdf_.end(), uniform(engine));
        return std::min<std::size_t>(found - cdf_.begin(), cdf_.size() - 1);
    }
    std::vector<std::string> documents(std::size_t count) const {
        std::mt19937_64 engine(spec_.seed);
        std::vector<std::string> docs(count);
        const std::size_t span = spec_.max_length - spec_.min_length + 1;
        for (auto& doc : docs) {
            const std::size_t length = spec_.min_length + engine() % span;
            for (std::size_t t = 0; t < length; ++t) {
                if (t) doc += ' ';
                doc += 'w';
                doc += std::to_string(rank(engine));
            }
        }
        return docs;
    }
    // 2-5 distinct terms: mostly mid-frequency ranks plus occasional head terms.
    std::vector<std::string> queries(std::size_t count, std::uint64_t seed) const {
        std::mt19937_64 engine(seed);
        std::vector<std::string> queries(count);
        for (auto& query : queries) {
            const std::size_t terms = 2 + engine() % 4;
            for (std::size_t t = 0; t < terms; ++t) {
                const std::size_t r = (engine() % 5 == 0) ? engine() % 100 : 100 + engine() % 20000;
                if (t) query += ' ';
                query += 'w';
                query += std::to_string(r);
            }
        }
        return queries;
    }

   private:
    ZipfSpec spec_;
    std::vector<double> cdf_;
};

inline double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(values.size())));
    return values[std::max<std::size_t>(rank, 1) - 1];
}

struct LatencyStats {
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, max = 0, qps = 0;
};

inline LatencyStats summarize(const std::vector<double>& latencies_ms) {
    LatencyStats stats;
    if (latencies_ms.empty()) return stats;
    double total = 0;
    for (double value : latencies_ms) total += value;
    stats.mean = total / static_cast<double>(latencies_ms.size());
    stats.p50 = percentile(latencies_ms, 0.50);
    stats.p95 = percentile(latencies_ms, 0.95);
    stats.p99 = percentile(latencies_ms, 0.99);
    stats.max = *std::max_element(latencies_ms.begin(), latencies_ms.end());
    stats.qps = total > 0 ? 1000.0 * static_cast<double>(latencies_ms.size()) / total : 0;
    return stats;
}

// Current memory footprint in MiB. On macOS this is phys_footprint, which excludes pages
// already freed but still mapped as reusable (resident_size keeps counting those until
// memory pressure, so it cannot show memory released by release_rows()). Elsewhere:
// peak RSS.
inline double resident_mib() {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
        KERN_SUCCESS)
        return static_cast<double>(info.phys_footprint) / (1024.0 * 1024.0);
#endif
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
}

inline double peak_resident_mib() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);  // bytes on macOS
#else
    return static_cast<double>(usage.ru_maxrss) / 1024.0;  // KiB on Linux
#endif
}

inline std::ofstream open_csv(const std::string& path, const std::string& header) {
    std::ofstream file(path);
    if (!file) throw std::runtime_error("Cannot write " + path);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.imbue(std::locale::classic());
    file << std::setprecision(9) << header << '\n';
    return file;
}

inline const char* build_type() {
#ifdef NDEBUG
    return "Release (NDEBUG)";
#else
    return "Debug (assertions enabled; not valid performance evidence)";
#endif
}

}  // namespace aetherindex::bench
