#pragma once

// Shared helpers for the market comparison harnesses: TEXMEX fvecs/ivecs readers,
// repetition statistics and a minimal argument parser.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../bench_common.hpp"

#include <sys/mman.h>

#include <new>

namespace aetherindex::market {

// Input buffers are mapped directly: on macOS, libmalloc keeps freed large blocks in the
// process footprint, which would charge the freed input copy to the system under test.
template <class T>
struct MappedAllocator {
    using value_type = T;
    MappedAllocator() = default;
    template <class U>
    MappedAllocator(const MappedAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        void* p = mmap(nullptr, n * sizeof(T), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t n) noexcept { munmap(p, n * sizeof(T)); }
    template <class U>
    bool operator==(const MappedAllocator<U>&) const noexcept { return true; }
};

struct Matrix {
    std::size_t rows = 0, dimension = 0;
    std::vector<float, MappedAllocator<float>> values;  // Row-major.
    const float* row(std::size_t i) const { return values.data() + i * dimension; }
};

// fvecs: per row an int32 dimension followed by that many float32 values.
inline Matrix read_fvecs(const std::string& path, std::size_t limit = 0) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("Cannot read " + path);
    const auto bytes = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::int32_t dimension = 0;
    in.read(reinterpret_cast<char*>(&dimension), 4);
    if (dimension <= 0 || bytes % (4 * (static_cast<std::size_t>(dimension) + 1)) != 0)
        throw std::runtime_error("Malformed fvecs file " + path);
    Matrix m;
    m.dimension = static_cast<std::size_t>(dimension);
    m.rows = bytes / (4 * (m.dimension + 1));
    if (limit && limit < m.rows) m.rows = limit;
    m.values.resize(m.rows * m.dimension);
    in.seekg(0);
    std::vector<char> record(4 * (m.dimension + 1));
    for (std::size_t i = 0; i < m.rows; ++i) {
        in.read(record.data(), static_cast<std::streamsize>(record.size()));
        std::int32_t d = 0;
        std::memcpy(&d, record.data(), 4);
        if (!in || d != dimension) throw std::runtime_error("Inconsistent fvecs row in " + path);
        std::memcpy(m.values.data() + i * m.dimension, record.data() + 4, 4 * m.dimension);
    }
    return m;
}

inline std::vector<std::vector<std::int32_t>> read_ivecs(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path);
    std::vector<std::vector<std::int32_t>> rows;
    std::int32_t dimension = 0;
    while (in.read(reinterpret_cast<char*>(&dimension), 4)) {
        if (dimension <= 0) throw std::runtime_error("Malformed ivecs file " + path);
        std::vector<std::int32_t> row(static_cast<std::size_t>(dimension));
        in.read(reinterpret_cast<char*>(row.data()), 4 * static_cast<std::streamsize>(dimension));
        if (!in) throw std::runtime_error("Truncated ivecs file " + path);
        rows.push_back(std::move(row));
    }
    return rows;
}

// Median and interquartile range (nearest rank) across repetitions.
struct Spread {
    double median = 0, q1 = 0, q3 = 0;
};
inline Spread spread(const std::vector<double>& values) {
    return {bench::percentile(values, 0.5), bench::percentile(values, 0.25),
            bench::percentile(values, 0.75)};
}

// --name value pairs; flags without a value are not supported (explicit values only).
class Args {
   public:
    Args(int argc, char** argv, int first) {
        for (int i = first; i < argc; i += 2) {
            const std::string name = argv[i];
            if (name.rfind("--", 0) != 0 || i + 1 >= argc)
                throw std::invalid_argument("Expected --name value, got " + name);
            values_[name.substr(2)] = argv[i + 1];
        }
    }
    std::string get(const std::string& name, const std::string& fallback = {}) const {
        const auto found = values_.find(name);
        if (found != values_.end()) {
            used_.push_back(name);
            return found->second;
        }
        if (fallback.empty()) throw std::invalid_argument("Missing --" + name);
        return fallback;
    }
    std::size_t number(const std::string& name, std::size_t fallback) const {
        return values_.contains(name) ? std::stoul(get(name)) : fallback;
    }
    double real(const std::string& name, double fallback) const {
        return values_.contains(name) ? std::stod(get(name)) : fallback;
    }
    bool has(const std::string& name) const { return values_.contains(name); }
    std::vector<std::size_t> list(const std::string& name, const std::string& fallback) const {
        std::vector<std::size_t> out;
        std::stringstream stream(get(name, fallback));
        for (std::string item; std::getline(stream, item, ',');) out.push_back(std::stoul(item));
        return out;
    }
    // Rejects misspelled options: every provided option must have been read.
    void require_all_used() const {
        for (const auto& [name, value] : values_)
            if (std::find(used_.begin(), used_.end(), name) == used_.end())
                throw std::invalid_argument("Unknown or unused option --" + name);
    }

   private:
    std::map<std::string, std::string> values_;
    mutable std::vector<std::string> used_;
};

inline std::string json_escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

}  // namespace aetherindex::market
