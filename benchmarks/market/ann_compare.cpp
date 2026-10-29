// Market comparison, ANN: AetherIndex against hnswlib and FAISS on TEXMEX-format data.
//
// Every system runs in this one harness: same data buffers, same compiler flags, same
// steady_clock timer, same thread counts. One process builds one system/configuration
// (memory is measured as the phys_footprint growth over a baseline taken before the base
// vectors are read, after the input copy is freed) and then sweeps its search parameter.
//
//   aether_ann_compare --system S --base B.fvecs --queries Q.fvecs --groundtruth G.ivecs
//       --metric l2|cosine --dataset NAME --out FILE.csv [--M 16] [--ef-construction 200]
//       [--params 10,20,...] [--build-threads 10] [--query-threads 10]
//       [--latency-queries 1000] [--repetitions 5] [--pq-m 32] [--nlist 1024]
//       [--refine 4] [--raw FILE.csv]
//
// Systems: aether-hnsw, aether-sq8 (FP32 re-rank), aether-sq8-lean (FP32 released, no
// re-rank), aether-pq (FP32 re-rank), hnswlib, faiss-hnsw, faiss-hnsw-sq8,
// faiss-ivfpq-refine, faiss-flat (exact; checks the ground truth). For cosine the input
// must already be unit-normalized (prepare_ann.py does it) so that inner product,
// cosine and angular rankings agree for every system.
#include <omp.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/IndexRefine.h>
#include <faiss/impl/HNSW.h>
#include <hnswlib/hnswlib.h>

#include "aether/ann/brute_force/brute_force_index.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/core/parallel.hpp"
#include "aether/quantization/pq.hpp"
#include "aether/quantization/sq8.hpp"
#include "market_common.hpp"

using namespace aetherindex;
using namespace aetherindex::market;
namespace bench = aetherindex::bench;

namespace {

using Ids = std::vector<std::int64_t>;

struct Options {
    std::string system, metric, dataset;
    std::size_t m = 16, ef_construction = 200, build_threads = 10, query_threads = 10;
    std::size_t pq_m = 0, nlist = 1024, refine = 4;
};

// One system under test. search() is called concurrently from several threads.
class Searcher {
   public:
    virtual ~Searcher() = default;
    virtual void build(const Matrix& base) = 0;
    virtual void after_input_released() {}
    virtual void set_param(std::size_t value) = 0;
    virtual void search(const float* query, std::size_t k, Ids& out) const = 0;
    virtual void search_batch(const Matrix& queries, std::size_t k, std::size_t threads,
                              std::vector<Ids>& out) const {
        parallel_for(queries.rows, threads,
                     [&](std::size_t i) { search(queries.row(i), k, out[i]); });
    }
    virtual void single_threaded() {}
    virtual double library_mib() const { return 0; }
    virtual const char* param_name() const { return "ef"; }
};

// --- AetherIndex --------------------------------------------------------------------

class AetherSearcher final : public Searcher {
   public:
    enum class Codes { None, Sq8, Sq8Lean, Pq };
    AetherSearcher(const Options& options, Codes codes) : options_(options), codes_kind_(codes) {}

    void build(const Matrix& base) override {
        metric_ = options_.metric == "cosine" ? DistanceKind::Cosine : DistanceKind::SquaredEuclidean;
        store_ = std::make_unique<VectorStore>(base.dimension, metric_);
        store_->reserve(base.rows);
        for (std::size_t i = 0; i < base.rows; ++i)
            if (store_->insert(static_cast<VectorId>(i), {base.row(i), base.dimension}) !=
                InsertStatus::Success)
                throw std::runtime_error("Insert failed at row " + std::to_string(i));
        HnswConfig config;
        config.m = options_.m;
        config.m_max_0 = 2 * options_.m;
        config.ef_construction = options_.ef_construction;
        index_ = std::make_unique<HnswIndex>(*store_, config);
        const std::vector<VectorId> ids(store_->ids().begin(), store_->ids().end());
        index_->insert_parallel(ids, options_.build_threads);
        if (codes_kind_ == Codes::Sq8 || codes_kind_ == Codes::Sq8Lean) {
            codes_ = std::make_unique<Sq8Vectors>(*store_);
        } else if (codes_kind_ == Codes::Pq) {
            PqConfig pq;
            pq.subspaces = options_.pq_m;
            pq.threads = options_.build_threads;
            codes_ = std::make_unique<PqVectors>(*store_, pq);
        }
        if (codes_kind_ == Codes::Sq8Lean) store_->release_rows();
    }
    void set_param(std::size_t value) override { ef_ = value; }
    void search(const float* query, std::size_t k, Ids& out) const override {
        const PreparedQuery prepared({query, store_->dimension()}, store_->dimension(), metric_);
        const auto results =
            codes_ ? index_->search_quantized(prepared, *codes_, k, ef_,
                                              codes_kind_ == Codes::Sq8Lean ? 0 : std::max(k, ef_))
                   : index_->search_prepared(prepared, k, ef_);
        out.resize(results.size());
        for (std::size_t i = 0; i < results.size(); ++i) out[i] = results[i].id;
    }
    double library_mib() const override {
        const auto graph = index_->memory_usage();
        double bytes = static_cast<double>(graph.total_bytes());
        if (store_->has_rows())
            bytes += static_cast<double>(store_->size() * store_->dimension() * sizeof(float));
        if (codes_) bytes += static_cast<double>(codes_->memory_bytes());
        return bytes / (1024.0 * 1024.0);
    }

   private:
    Options options_;
    Codes codes_kind_;
    DistanceKind metric_ = DistanceKind::SquaredEuclidean;
    std::unique_ptr<VectorStore> store_;
    std::unique_ptr<HnswIndex> index_;
    std::unique_ptr<QuantizedVectors> codes_;
    std::size_t ef_ = 10;
};

// --- hnswlib ------------------------------------------------------------------------

class HnswlibSearcher final : public Searcher {
   public:
    explicit HnswlibSearcher(const Options& options) : options_(options) {}
    void build(const Matrix& base) override {
        if (options_.metric == "cosine")
            space_ = std::make_unique<hnswlib::InnerProductSpace>(base.dimension);
        else
            space_ = std::make_unique<hnswlib::L2Space>(base.dimension);
        index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
            space_.get(), base.rows, options_.m, options_.ef_construction);
        // As in hnswlib's own Python binding: first point alone, then concurrent adds.
        index_->addPoint(base.row(0), 0);
        parallel_for(base.rows - 1, options_.build_threads, [&](std::size_t i) {
            index_->addPoint(base.row(i + 1), static_cast<hnswlib::labeltype>(i + 1));
        });
    }
    void set_param(std::size_t value) override { index_->setEf(value); }
    void search(const float* query, std::size_t k, Ids& out) const override {
        auto heap = index_->searchKnn(query, k);
        out.resize(heap.size());
        for (std::size_t i = heap.size(); i-- > 0; heap.pop())
            out[i] = static_cast<std::int64_t>(heap.top().second);
    }
    double library_mib() const override {
        // Level-0 block plus upper-level link lists, as allocated by hnswlib.
        double bytes = static_cast<double>(index_->max_elements_ * index_->size_data_per_element_);
        for (std::size_t i = 0; i < index_->cur_element_count; ++i)
            if (index_->element_levels_[i] > 0)
                bytes += static_cast<double>(index_->size_links_per_element_ *
                                             index_->element_levels_[i]);
        return bytes / (1024.0 * 1024.0);
    }

   private:
    Options options_;
    std::unique_ptr<hnswlib::SpaceInterface<float>> space_;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> index_;
};

// --- FAISS --------------------------------------------------------------------------

class FaissSearcher final : public Searcher {
   public:
    enum class Kind { Hnsw, HnswSq8, IvfPqRefine, Flat };
    FaissSearcher(const Options& options, Kind kind) : options_(options), kind_(kind) {}

    void build(const Matrix& base) override {
        const auto d = static_cast<faiss::idx_t>(base.dimension);
        const auto metric =
            options_.metric == "cosine" ? faiss::METRIC_INNER_PRODUCT : faiss::METRIC_L2;
        const auto n = static_cast<faiss::idx_t>(base.rows);
        omp_set_num_threads(static_cast<int>(options_.build_threads));
        switch (kind_) {
            case Kind::Hnsw: {
                auto index = std::make_unique<faiss::IndexHNSWFlat>(d, options_.m, metric);
                index->hnsw.efConstruction = static_cast<int>(options_.ef_construction);
                hnsw_ = &index->hnsw;
                index_ = std::move(index);
                break;
            }
            case Kind::HnswSq8: {
                auto index = std::make_unique<faiss::IndexHNSWSQ>(
                    d, faiss::ScalarQuantizer::QT_8bit, options_.m, metric);
                index->hnsw.efConstruction = static_cast<int>(options_.ef_construction);
                hnsw_ = &index->hnsw;
                index_ = std::move(index);
                break;
            }
            case Kind::IvfPqRefine: {
                quantizer_ = metric == faiss::METRIC_L2
                                 ? std::unique_ptr<faiss::Index>(new faiss::IndexFlatL2(d))
                                 : std::unique_ptr<faiss::Index>(new faiss::IndexFlatIP(d));
                ivf_ = std::make_unique<faiss::IndexIVFPQ>(quantizer_.get(), d, options_.nlist,
                                                           options_.pq_m, 8, metric);
                auto refine = std::make_unique<faiss::IndexRefineFlat>(ivf_.get());
                refine->k_factor = static_cast<float>(options_.refine);
                index_ = std::move(refine);
                break;
            }
            case Kind::Flat:
                index_ = metric == faiss::METRIC_L2
                             ? std::unique_ptr<faiss::Index>(new faiss::IndexFlatL2(d))
                             : std::unique_ptr<faiss::Index>(new faiss::IndexFlatIP(d));
                break;
        }
        if (!index_->is_trained) index_->train(n, base.values.data());
        index_->add(n, base.values.data());
    }
    const char* param_name() const override {
        return kind_ == Kind::IvfPqRefine ? "nprobe" : kind_ == Kind::Flat ? "exact" : "ef";
    }
    void set_param(std::size_t value) override {
        if (hnsw_) hnsw_->efSearch = static_cast<int>(value);
        if (ivf_) ivf_->nprobe = value;
    }
    void single_threaded() override { omp_set_num_threads(1); }
    // Structure sizes from FAISS's own containers (codes, links, inverted lists).
    double library_mib() const override {
        const auto codes = [](const faiss::Index* index) {
            const auto* flat = dynamic_cast<const faiss::IndexFlatCodes*>(index);
            return flat ? static_cast<double>(flat->codes.size()) : 0.0;
        };
        double bytes = 0;
        if (const auto* hnsw = dynamic_cast<const faiss::IndexHNSW*>(index_.get())) {
            bytes = codes(hnsw->storage) +
                    static_cast<double>(hnsw->hnsw.neighbors.size() *
                                        sizeof(faiss::HNSW::storage_idx_t)) +
                    static_cast<double>(hnsw->hnsw.levels.size() * sizeof(int) +
                                        hnsw->hnsw.offsets.size() * sizeof(std::size_t));
        } else if (ivf_) {
            for (std::size_t list = 0; list < ivf_->nlist; ++list)
                bytes += static_cast<double>(ivf_->invlists->list_size(list) *
                                             (ivf_->code_size + sizeof(faiss::idx_t)));
            bytes += static_cast<double>(ivf_->nlist * ivf_->d * sizeof(float) +
                                         ivf_->pq.centroids.size() * sizeof(float));
            bytes += codes(dynamic_cast<const faiss::IndexRefine*>(index_.get())->refine_index);
        } else {
            bytes = codes(index_.get());
        }
        return bytes / (1024.0 * 1024.0);
    }
    void search(const float* query, std::size_t k, Ids& out) const override {
        std::vector<float> distances(k);
        out.assign(k, -1);
        index_->search(1, query, static_cast<faiss::idx_t>(k), distances.data(), out.data());
        out.erase(std::remove(out.begin(), out.end(), -1), out.end());
    }
    // FAISS parallelizes a query batch itself (OpenMP over queries).
    void search_batch(const Matrix& queries, std::size_t k, std::size_t threads,
                      std::vector<Ids>& out) const override {
        omp_set_num_threads(static_cast<int>(threads));
        std::vector<float> distances(queries.rows * k);
        std::vector<faiss::idx_t> labels(queries.rows * k);
        index_->search(static_cast<faiss::idx_t>(queries.rows), queries.values.data(),
                       static_cast<faiss::idx_t>(k), distances.data(), labels.data());
        for (std::size_t q = 0; q < queries.rows; ++q) {
            out[q].assign(labels.begin() + static_cast<std::ptrdiff_t>(q * k),
                          labels.begin() + static_cast<std::ptrdiff_t>((q + 1) * k));
            out[q].erase(std::remove(out[q].begin(), out[q].end(), -1), out[q].end());
        }
        omp_set_num_threads(1);
    }

   private:
    Options options_;
    Kind kind_;
    std::unique_ptr<faiss::Index> quantizer_;
    std::unique_ptr<faiss::IndexIVFPQ> ivf_;
    std::unique_ptr<faiss::Index> index_;
    faiss::HNSW* hnsw_ = nullptr;
};

std::unique_ptr<Searcher> make(const Options& o) {
    using A = AetherSearcher;
    using F = FaissSearcher;
    if (o.system == "aether-hnsw") return std::make_unique<A>(o, A::Codes::None);
    if (o.system == "aether-sq8") return std::make_unique<A>(o, A::Codes::Sq8);
    if (o.system == "aether-sq8-lean") return std::make_unique<A>(o, A::Codes::Sq8Lean);
    if (o.system == "aether-pq") return std::make_unique<A>(o, A::Codes::Pq);
    if (o.system == "hnswlib") return std::make_unique<HnswlibSearcher>(o);
    if (o.system == "faiss-hnsw") return std::make_unique<F>(o, F::Kind::Hnsw);
    if (o.system == "faiss-hnsw-sq8") return std::make_unique<F>(o, F::Kind::HnswSq8);
    if (o.system == "faiss-ivfpq-refine") return std::make_unique<F>(o, F::Kind::IvfPqRefine);
    if (o.system == "faiss-flat") return std::make_unique<F>(o, F::Kind::Flat);
    throw std::invalid_argument("Unknown --system " + o.system);
}

double recall_at(const std::vector<Ids>& results, const std::vector<std::vector<std::int32_t>>& truth,
                 std::size_t k) {
    double hits = 0;
    for (std::size_t q = 0; q < results.size(); ++q) {
        const std::unordered_set<std::int64_t> expected(truth[q].begin(), truth[q].begin() + k);
        for (std::size_t i = 0; i < std::min(k, results[q].size()); ++i)
            hits += expected.contains(results[q][i]);
    }
    return hits / (static_cast<double>(results.size()) * static_cast<double>(k));
}

int run(int argc, char** argv) {
    const Args args(argc, argv, 1);
    Options o;
    o.system = args.get("system");
    o.metric = args.get("metric");
    o.dataset = args.get("dataset");
    o.m = args.number("M", 16);
    o.ef_construction = args.number("ef-construction", 200);
    o.build_threads = args.number("build-threads", 10);
    o.query_threads = args.number("query-threads", 10);
    o.nlist = args.number("nlist", 1024);
    o.refine = args.number("refine", 4);
    const std::string base_path = args.get("base"), query_path = args.get("queries"),
                      truth_path = args.get("groundtruth"), out_path = args.get("out");
    const std::string raw_path = args.has("raw") ? args.get("raw") : std::string();
    const std::size_t latency_queries = args.number("latency-queries", 1000);
    const std::size_t repetitions = args.number("repetitions", 5);
    constexpr std::size_t k = 10;
    if (o.metric != "l2" && o.metric != "cosine")
        throw std::invalid_argument("--metric must be l2 or cosine");
    if (std::string(AETHER_HNSWLIB_VARIANT) == "fastmath" && o.system != "hnswlib")
        throw std::invalid_argument("The fast-math binary only runs --system hnswlib");

    const Matrix queries = read_fvecs(query_path);
    const auto truth = read_ivecs(truth_path);
    if (truth.size() < queries.rows || truth.front().size() < k)
        throw std::runtime_error("Ground truth does not cover the queries");
    Matrix latency_set;
    latency_set.dimension = queries.dimension;
    latency_set.rows = std::min(latency_queries, queries.rows);
    latency_set.values.assign(queries.values.begin(),
                              queries.values.begin() +
                                  static_cast<std::ptrdiff_t>(latency_set.rows * queries.dimension));
    omp_set_num_threads(1);
    const double baseline_mib = bench::resident_mib();

    std::unique_ptr<Searcher> searcher;
    std::size_t base_rows = 0, dimension = 0;
    double build_seconds = 0;
    {
        Matrix base = read_fvecs(base_path);
        base_rows = base.rows;
        dimension = base.dimension;
        if (base.dimension != queries.dimension) throw std::runtime_error("Dimension mismatch");
        if (o.pq_m == 0) o.pq_m = args.number("pq-m", base.dimension / 4);
        searcher = make(o);
        const auto start = bench::Clock::now();
        searcher->build(base);
        build_seconds = bench::elapsed_ms(start) / 1000.0;
    }  // Input copy freed here; every system keeps only what it owns.
    searcher->after_input_released();
    searcher->single_threaded();
    const double memory_mib = bench::resident_mib() - baseline_mib;

    const bool exact = o.system == "faiss-flat";
    const auto params = exact ? std::vector<std::size_t>{0}
                              : args.list("params", "10,16,24,32,48,64,96,128,192,256,384,512,800");
    args.require_all_used();
    const bool new_file = !std::ifstream(out_path).good();
    std::ofstream out(out_path, std::ios::app);
    if (!out) throw std::runtime_error("Cannot write " + out_path);
    out.imbue(std::locale::classic());
    out << std::setprecision(9);
    if (new_file)
        out << "dataset,system,variant,base_rows,dimension,metric,M,ef_construction,param_name,"
               "param,build_threads,build_seconds,memory_mib,library_mib,recall10,"
               "latency_queries,lat_p50_ms,lat_p95_ms,lat_p99_ms,lat_mean_ms,qps_single,"
               "query_threads,qps_batch,qps_batch_q1,qps_batch_q3,repetitions\n";
    std::ofstream raw;
    if (!raw_path.empty()) {
        const bool raw_new = !std::ifstream(raw_path).good();
        raw.open(raw_path, std::ios::app);
        raw << std::setprecision(9);
        if (raw_new) raw << "dataset,system,M,ef_construction,param,query,median_latency_ms\n";
    }
    for (const std::size_t param : params) {
        searcher->set_param(param);
        // Recall: every query, batch path (results do not depend on the path).
        std::vector<Ids> results(queries.rows);
        searcher->search_batch(queries, k, o.query_threads, results);
        const double recall = recall_at(results, truth, k);
        // Single-thread latency: one warm-up pass, then `repetitions` timed passes.
        searcher->single_threaded();
        Ids scratch;
        std::vector<std::vector<double>> per_query(latency_set.rows);
        std::vector<double> p50, p95, p99, mean, qps1;
        for (std::size_t rep = 0; rep <= repetitions; ++rep) {
            std::vector<double> latencies(latency_set.rows);
            for (std::size_t q = 0; q < latency_set.rows; ++q) {
                const auto start = bench::Clock::now();
                searcher->search(latency_set.row(q), k, scratch);
                latencies[q] = bench::elapsed_ms(start);
            }
            if (rep == 0) continue;
            for (std::size_t q = 0; q < latency_set.rows; ++q)
                per_query[q].push_back(latencies[q]);
            const auto stats = bench::summarize(latencies);
            p50.push_back(stats.p50);
            p95.push_back(stats.p95);
            p99.push_back(stats.p99);
            mean.push_back(stats.mean);
            qps1.push_back(stats.qps);
        }
        // Batch throughput with query_threads, all queries, after one warm-up pass.
        std::vector<double> qps;
        for (std::size_t rep = 0; rep <= repetitions; ++rep) {
            const auto start = bench::Clock::now();
            searcher->search_batch(queries, k, o.query_threads, results);
            const double seconds = bench::elapsed_ms(start) / 1000.0;
            if (rep > 0) qps.push_back(static_cast<double>(queries.rows) / seconds);
        }
        searcher->single_threaded();
        const auto batch = spread(qps);
        out << o.dataset << ',' << o.system << ',' << AETHER_HNSWLIB_VARIANT << ',' << base_rows
            << ',' << dimension << ',' << o.metric << ',' << o.m << ',' << o.ef_construction
            << ',' << searcher->param_name() << ',' << param << ',' << o.build_threads << ','
            << build_seconds << ',' << memory_mib << ',' << searcher->library_mib() << ','
            << recall << ',' << latency_set.rows << ',' << spread(p50).median << ','
            << spread(p95).median << ',' << spread(p99).median << ',' << spread(mean).median
            << ',' << spread(qps1).median << ',' << o.query_threads << ',' << batch.median << ','
            << batch.q1 << ',' << batch.q3 << ',' << repetitions << '\n';
        out.flush();
        if (raw.is_open())
            for (std::size_t q = 0; q < per_query.size(); ++q)
                raw << o.dataset << ',' << o.system << ',' << o.m << ',' << o.ef_construction
                    << ',' << param << ',' << q << ',' << bench::percentile(per_query[q], 0.5)
                    << '\n';
        std::cout << o.system << ' ' << searcher->param_name() << '=' << param
                  << " recall@10=" << recall << " p50=" << spread(p50).median
                  << "ms qps(batch)=" << batch.median << '\n';
    }
    std::cout << o.system << " build " << build_seconds << " s, memory " << memory_mib
              << " MiB (" << bench::build_type() << ")\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
