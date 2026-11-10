// OKR 3 / KR3.4, KR3.5, KR3.6: distance kernels, SIMD against the scalar reference.
//
//   aether_distance_kernel_benchmark OUTPUT_DIR [--fvecs BASE.fvecs] [--rows 10000]
//       [--queries 8] [--repetitions 50] [--warmup 5] [--dims 100,128,384,768,1024]
//
// One "execution" is one kernel call (query row x data row). Each repetition times
// `rows` consecutive calls against one fixed in-memory matrix; the reported unit is the
// mean nanoseconds per call of that repetition. Scalar and SIMD kernels read exactly the
// same buffers, are compiled by the same compiler with the same flags into the same
// library and are both called through a function pointer. Their order alternates every
// repetition so that frequency or thermal drift does not favor one of them.
//
// KR3.4  Cosine (unit vectors, 1 - dot) and squared Euclidean, each timed over
//        rows x repetitions executions (default 10,000 x 50 = 500,000 per kernel).
// KR3.5  Numerical validation on the same data: SIMD vs scalar vs a long-double
//        reference, against the FP32 summation bound 2*n*eps*sum|term|.
// KR3.6  Mean time reduction of SIMD vs scalar (target >= 20 %).
//
// Default data: seeded Gaussian rows (seed 42) for each --dims value. --fvecs uses the
// first rows+queries vectors of a TEXMEX file (e.g. SIFT1M) at its own dimension; the
// last `queries` of them are the query rows. Rows are zero-padded to a multiple of 16 and
// kernels receive the padded length, exactly as on the search path.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "aether/core/aligned_allocator.hpp"
#include "aether/core/vector_math.hpp"
#include "aether/distance/kernels.hpp"

using namespace aetherindex;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::filesystem::path output;
    std::filesystem::path fvecs;
    std::size_t rows = 10000;
    std::size_t queries = 8;
    std::size_t repetitions = 50;
    std::size_t warmup = 5;
    std::vector<std::size_t> dims{100, 128, 384, 768, 1024};
};

// Row-major, each row padded with zeros to padded_dimension(dimension).
struct Matrix {
    std::size_t rows = 0, dimension = 0, stride = 0;
    AlignedVector<float> values;
    float* row(std::size_t i) { return values.data() + i * stride; }
    const float* row(std::size_t i) const { return values.data() + i * stride; }
};

struct Dataset {
    std::string name;
    Matrix data, queries;
};

struct Timing {
    std::vector<double> ns_per_call;  // One entry per measured repetition.
    double checksum = 0;
};

struct Validation {
    std::size_t pairs = 0;
    double max_abs_simd_scalar = 0, max_rel_simd_scalar = 0;
    double max_abs_simd_reference = 0, max_abs_scalar_reference = 0;
    double max_bound_ratio = 0;  // max |error| / tolerance over SIMD and scalar; <= 1 passes.
};

std::vector<std::size_t> parse_list(const std::string& text) {
    std::vector<std::size_t> values;
    std::stringstream input(text);
    std::string item;
    while (std::getline(input, item, ',')) values.push_back(std::stoul(item));
    if (values.empty()) throw std::invalid_argument("--dims needs at least one value");
    return values;
}

Options parse(int argc, char** argv) {
    if (argc < 2)
        throw std::invalid_argument(
            "Usage: aether_distance_kernel_benchmark OUTPUT_DIR [--fvecs BASE.fvecs] [--rows N] "
            "[--queries N] [--repetitions N] [--warmup N] [--dims D1,D2,...]");
    Options options;
    options.output = argv[1];
    for (int i = 2; i < argc; i += 2) {
        const std::string name = argv[i];
        if (i + 1 >= argc) throw std::invalid_argument(name + " needs a value");
        const std::string value = argv[i + 1];
        if (name == "--fvecs") options.fvecs = value;
        else if (name == "--rows") options.rows = std::stoul(value);
        else if (name == "--queries") options.queries = std::stoul(value);
        else if (name == "--repetitions") options.repetitions = std::stoul(value);
        else if (name == "--warmup") options.warmup = std::stoul(value);
        else if (name == "--dims") options.dims = parse_list(value);
        else throw std::invalid_argument("Unknown option " + name);
    }
    if (options.rows == 0 || options.queries == 0 || options.repetitions == 0)
        throw std::invalid_argument("--rows, --queries and --repetitions must be positive");
    return options;
}

Matrix allocate(std::size_t rows, std::size_t dimension) {
    Matrix m{rows, dimension, detail::padded_dimension(dimension), {}};
    m.values.assign(rows * m.stride, 0.0f);
    return m;
}

Dataset synthetic(std::size_t rows, std::size_t queries, std::size_t dimension) {
    std::mt19937_64 engine(42 + dimension);
    std::normal_distribution<float> normal(0.0f, 1.0f);
    Dataset set{"synthetic-gaussian", allocate(rows, dimension), allocate(queries, dimension)};
    for (Matrix* m : {&set.data, &set.queries})
        for (std::size_t r = 0; r < m->rows; ++r)
            for (std::size_t d = 0; d < dimension; ++d) m->row(r)[d] = normal(engine);
    return set;
}

// fvecs: per row an int32 dimension followed by that many float32 values.
Dataset from_fvecs(const std::filesystem::path& path, std::size_t rows, std::size_t queries) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path.string());
    std::int32_t dimension = 0;
    in.read(reinterpret_cast<char*>(&dimension), 4);
    if (!in || dimension <= 0) throw std::runtime_error("Malformed fvecs file " + path.string());
    in.seekg(0);
    const auto dim = static_cast<std::size_t>(dimension);
    Dataset set{path.filename().string(), allocate(rows, dim), allocate(queries, dim)};
    std::vector<char> record(4 * (dim + 1));
    for (std::size_t i = 0; i < rows + queries; ++i) {
        in.read(record.data(), static_cast<std::streamsize>(record.size()));
        std::int32_t d = 0;
        std::memcpy(&d, record.data(), 4);
        if (!in || d != dimension)
            throw std::runtime_error("fvecs file has fewer rows than requested: " + path.string());
        float* target = i < rows ? set.data.row(i) : set.queries.row(i - rows);
        std::memcpy(target, record.data() + 4, 4 * dim);
    }
    return set;
}

// The cosine kernels' contract is unit vectors (the store normalizes once on insert).
Matrix normalized(const Matrix& source) {
    Matrix m = source;
    for (std::size_t r = 0; r < m.rows; ++r) {
        float* row = m.row(r);
        long double norm = 0;
        for (std::size_t d = 0; d < m.dimension; ++d) norm += static_cast<long double>(row[d]) * row[d];
        if (norm == 0) throw std::runtime_error("Zero row cannot be used with cosine");
        const auto scale = static_cast<float>(1.0L / std::sqrt(norm));
        for (std::size_t d = 0; d < m.dimension; ++d) row[d] *= scale;
    }
    return m;
}

double time_repetition(detail::DistanceKernel kernel, const Matrix& data, const float* query,
                       double& checksum) {
    float sum = 0.0f;
    const auto start = Clock::now();
    for (std::size_t r = 0; r < data.rows; ++r) sum += kernel(query, data.row(r), data.stride);
    const auto elapsed = Clock::now() - start;
    checksum += sum;  // Keeps every call observable.
    return std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(data.rows);
}

void time_both(const Options& options, DistanceKind metric, const Matrix& data,
               const Matrix& queries, Timing& scalar, Timing& simd) {
    const auto scalar_kernel = detail::scalar_distance_kernel(metric);
    const auto simd_kernel = detail::distance_kernel(metric);
    double ignored = 0;
    for (std::size_t w = 0; w < options.warmup; ++w) {
        const float* query = queries.row(w % queries.rows);
        time_repetition(scalar_kernel, data, query, ignored);
        time_repetition(simd_kernel, data, query, ignored);
    }
    for (std::size_t rep = 0; rep < options.repetitions; ++rep) {
        const float* query = queries.row(rep % queries.rows);
        if (rep % 2 == 0) {
            scalar.ns_per_call.push_back(time_repetition(scalar_kernel, data, query, scalar.checksum));
            simd.ns_per_call.push_back(time_repetition(simd_kernel, data, query, simd.checksum));
        } else {
            simd.ns_per_call.push_back(time_repetition(simd_kernel, data, query, simd.checksum));
            scalar.ns_per_call.push_back(time_repetition(scalar_kernel, data, query, scalar.checksum));
        }
    }
}

Validation validate(DistanceKind metric, const Matrix& data, const Matrix& queries) {
    constexpr long double eps = std::numeric_limits<float>::epsilon();
    const auto scalar_kernel = detail::scalar_distance_kernel(metric);
    const auto simd_kernel = detail::distance_kernel(metric);
    const bool cosine = metric == DistanceKind::Cosine;
    Validation v;
    for (std::size_t q = 0; q < queries.rows; ++q) {
        const float* query = queries.row(q);
        for (std::size_t r = 0; r < data.rows; ++r) {
            const float* row = data.row(r);
            long double exact = 0, magnitude = 0;
            for (std::size_t d = 0; d < data.dimension; ++d) {
                const long double term =
                    cosine ? static_cast<long double>(query[d]) * row[d]
                           : (static_cast<long double>(query[d]) - row[d]) *
                                 (static_cast<long double>(query[d]) - row[d]);
                exact += term;
                magnitude += std::abs(term);
            }
            if (cosine) exact = 1.0L - std::clamp(exact, -1.0L, 1.0L);
            const long double tolerance = 2.0L * static_cast<long double>(data.stride) * eps *
                                              magnitude +
                                          (cosine ? eps : std::numeric_limits<float>::min());
            const long double simd = simd_kernel(query, row, data.stride);
            const long double scalar = scalar_kernel(query, row, data.stride);
            const long double diff = std::abs(simd - scalar);
            v.max_abs_simd_scalar = std::max<double>(v.max_abs_simd_scalar, diff);
            if (std::abs(scalar) > 0)
                v.max_rel_simd_scalar =
                    std::max<double>(v.max_rel_simd_scalar, diff / std::abs(scalar));
            v.max_abs_simd_reference =
                std::max<double>(v.max_abs_simd_reference, std::abs(simd - exact));
            v.max_abs_scalar_reference =
                std::max<double>(v.max_abs_scalar_reference, std::abs(scalar - exact));
            v.max_bound_ratio = std::max<double>(
                v.max_bound_ratio,
                std::max({std::abs(simd - exact), std::abs(scalar - exact), diff}) / tolerance);
            ++v.pairs;
        }
    }
    return v;
}

struct Stats {
    double mean = 0, median = 0, p95 = 0, min = 0, max = 0, stddev = 0;
};

Stats stats(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    Stats s;
    const double n = static_cast<double>(values.size());
    for (double v : values) s.mean += v / n;
    for (double v : values) s.stddev += (v - s.mean) * (v - s.mean) / n;
    s.stddev = std::sqrt(s.stddev);
    s.median = values.size() % 2 ? values[values.size() / 2]
                                 : (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2;
    s.p95 = values[std::max<std::size_t>(
                       static_cast<std::size_t>(std::ceil(0.95 * n)), 1) - 1];
    s.min = values.front();
    s.max = values.back();
    return s;
}

std::ofstream open(const std::filesystem::path& path) {
    std::ofstream file(path);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.imbue(std::locale::classic());
    file << std::setprecision(9);
    return file;
}

const char* metric_name(DistanceKind metric) {
    return metric == DistanceKind::Cosine ? "cosine" : "squared_euclidean";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse(argc, argv);
        std::filesystem::create_directories(options.output);
        std::vector<Dataset> datasets;
        if (!options.fvecs.empty())
            datasets.push_back(from_fvecs(options.fvecs, options.rows, options.queries));
        else
            for (std::size_t dimension : options.dims)
                datasets.push_back(synthetic(options.rows, options.queries, dimension));

        auto timing_csv = open(options.output / "kr3_4_kernel_timing.csv");
        auto speedup_csv = open(options.output / "kr3_6_simd_speedup.csv");
        auto validation_csv = open(options.output / "kr3_5_numeric_validation.csv");
        auto raw_csv = open(options.output / "raw_repetitions.csv");
        timing_csv << "dataset,metric,dimension,variant,backend,rows,repetitions,executions,"
                      "mean_ns,median_ns,p95_ns,min_ns,max_ns,stddev_ns\n";
        speedup_csv << "dataset,metric,dimension,scalar_mean_ns,simd_mean_ns,"
                       "scalar_median_ns,simd_median_ns,mean_reduction_pct,speedup,"
                       "meets_kr3_6\n";
        validation_csv << "dataset,metric,dimension,pairs,max_abs_simd_vs_scalar,"
                          "max_rel_simd_vs_scalar,max_abs_simd_vs_longdouble,"
                          "max_abs_scalar_vs_longdouble,max_error_over_bound,pass\n";
        raw_csv << "dataset,metric,dimension,variant,repetition,ns_per_call\n";

        bool all_valid = true, all_faster = true;
        for (const Dataset& set : datasets) {
            for (DistanceKind metric : {DistanceKind::Cosine, DistanceKind::SquaredEuclidean}) {
                const bool cosine = metric == DistanceKind::Cosine;
                const Matrix data = cosine ? normalized(set.data) : set.data;
                const Matrix queries = cosine ? normalized(set.queries) : set.queries;
                const std::size_t dim = data.dimension;

                const Validation v = validate(metric, data, queries);
                const bool valid = v.max_bound_ratio <= 1.0;
                all_valid = all_valid && valid;
                validation_csv << set.name << ',' << metric_name(metric) << ',' << dim << ','
                               << v.pairs << ',' << v.max_abs_simd_scalar << ','
                               << v.max_rel_simd_scalar << ',' << v.max_abs_simd_reference << ','
                               << v.max_abs_scalar_reference << ',' << v.max_bound_ratio << ','
                               << (valid ? "true" : "false") << '\n';

                Timing scalar, simd;
                time_both(options, metric, data, queries, scalar, simd);
                const Stats s = stats(scalar.ns_per_call), x = stats(simd.ns_per_call);
                for (const auto& [variant, backend, st] :
                     {std::tuple{"scalar", "Scalar FP32 (sequential)", s},
                      std::tuple{"simd", detail::distance_kernel_backend(), x}})
                    timing_csv << set.name << ',' << metric_name(metric) << ',' << dim << ','
                               << variant << ',' << backend << ',' << data.rows << ','
                               << options.repetitions << ','
                               << data.rows * options.repetitions << ',' << st.mean << ','
                               << st.median << ',' << st.p95 << ',' << st.min << ',' << st.max
                               << ',' << st.stddev << '\n';
                const double reduction = 100.0 * (1.0 - x.mean / s.mean);
                const bool faster = reduction >= 20.0;
                all_faster = all_faster && faster;
                speedup_csv << set.name << ',' << metric_name(metric) << ',' << dim << ','
                            << s.mean << ',' << x.mean << ',' << s.median << ',' << x.median
                            << ',' << reduction << ',' << s.mean / x.mean << ','
                            << (faster ? "true" : "false") << '\n';
                for (std::size_t rep = 0; rep < options.repetitions; ++rep) {
                    raw_csv << set.name << ',' << metric_name(metric) << ',' << dim
                            << ",scalar," << rep << ',' << scalar.ns_per_call[rep] << '\n';
                    raw_csv << set.name << ',' << metric_name(metric) << ',' << dim << ",simd,"
                            << rep << ',' << simd.ns_per_call[rep] << '\n';
                }
                std::cout << set.name << ' ' << metric_name(metric) << " d=" << dim
                          << " scalar=" << s.mean << "ns simd=" << x.mean
                          << "ns reduction=" << reduction << "% max|simd-scalar|="
                          << v.max_abs_simd_scalar << (valid ? " valid" : " INVALID") << '\n';
            }
        }

        auto meta = open(options.output / "environment.json");
        meta << "{\"simd_backend\":\"" << detail::distance_kernel_backend()
             << "\",\"scalar_backend\":\"Scalar FP32, one sequential FMA accumulator, "
                "vectorization disabled\",\"dataset\":\""
             << (options.fvecs.empty() ? "synthetic Gaussian, seed 42+dimension"
                                       : options.fvecs.string())
             << "\",\"rows\":" << options.rows << ",\"query_rows\":" << options.queries
             << ",\"warmup_repetitions\":" << options.warmup
             << ",\"measured_repetitions\":" << options.repetitions
             << ",\"executions_per_kernel\":" << options.rows * options.repetitions
             << ",\"timing_unit\":\"mean ns per kernel call within one repetition of `rows` "
                "calls\",\"order\":\"scalar/simd alternate each repetition\",\"threads\":1"
             << ",\"build_type\":\""
#ifdef NDEBUG
             << "Release (NDEBUG)"
#else
             << "Debug (assertions enabled; not valid KR3 evidence)"
#endif
             << "\",\"compiler\":\"" << __VERSION__ << "\",\"cplusplus\":" << __cplusplus
             << ",\"all_numeric_validations_pass\":" << (all_valid ? "true" : "false")
             << ",\"all_reductions_at_least_20pct\":" << (all_faster ? "true" : "false")
             << "}\n";
        std::cout << "Wrote KR3.4/KR3.5/KR3.6 reports to " << options.output << '\n';
        return all_valid ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
