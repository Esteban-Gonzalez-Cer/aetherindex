// Lucene BM25 runs and latency for the market comparison, with the same protocol as
// aether_ir_compare: a quality run of DEPTH results per query, single-thread latency for
// the first LATENCY_QUERIES queries at LATENCY_K after warm-up, then batch throughput
// with THREADS over every query. Queries are built like Anserini's
// BagOfWordsQueryGenerator (analyzed terms as SHOULD clauses, boosted by repetition), so
// the runs reproduce Anserini's published BEIR BM25 numbers. Timing covers query
// analysis and IndexSearcher.search only; BEIR ids are read afterwards for the run file.
//
//   java -cp anserini-fatjar.jar:CLASSES LuceneSearch INDEX TOPICS.tsv PREFIX K1 B
//        STEMMER(porter|none) KEEP_STOPWORDS(true|false) DEPTH LATENCY_K LATENCY_QUERIES
//        REPETITIONS THREADS WARMUPS
import io.anserini.analysis.DefaultEnglishAnalyzer;
import java.io.BufferedWriter;
import java.io.IOException;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import org.apache.lucene.analysis.Analyzer;
import org.apache.lucene.analysis.CharArraySet;
import org.apache.lucene.analysis.TokenStream;
import org.apache.lucene.analysis.tokenattributes.CharTermAttribute;
import org.apache.lucene.index.DirectoryReader;
import org.apache.lucene.index.IndexReader;
import org.apache.lucene.index.StoredFields;
import org.apache.lucene.index.Term;
import org.apache.lucene.search.BooleanClause;
import org.apache.lucene.search.BooleanQuery;
import org.apache.lucene.search.BoostQuery;
import org.apache.lucene.search.IndexSearcher;
import org.apache.lucene.search.Query;
import org.apache.lucene.search.ScoreDoc;
import org.apache.lucene.search.TermQuery;
import org.apache.lucene.search.TopDocs;
import org.apache.lucene.search.similarities.BM25Similarity;
import org.apache.lucene.store.FSDirectory;

public class LuceneSearch {
  static Analyzer analyzer;
  static IndexSearcher searcher;

  static Query query(String text) throws IOException {
    Map<String, Integer> counts = new LinkedHashMap<>();
    try (TokenStream stream = analyzer.tokenStream("contents", text)) {
      CharTermAttribute term = stream.addAttribute(CharTermAttribute.class);
      stream.reset();
      while (stream.incrementToken()) counts.merge(term.toString(), 1, Integer::sum);
      stream.end();
    }
    BooleanQuery.Builder builder = new BooleanQuery.Builder();
    for (Map.Entry<String, Integer> e : counts.entrySet())
      builder.add(new BoostQuery(new TermQuery(new Term("contents", e.getKey())), e.getValue()),
                  BooleanClause.Occur.SHOULD);
    return builder.build();
  }

  static TopDocs search(String text, int k) throws IOException {
    return searcher.search(query(text), k);
  }

  static double percentile(double[] values, double p) {
    double[] sorted = values.clone();
    Arrays.sort(sorted);
    int rank = (int) Math.ceil(p * sorted.length);
    return sorted[Math.max(rank, 1) - 1];
  }

  static double median(List<Double> values) {
    double[] array = values.stream().mapToDouble(Double::doubleValue).toArray();
    return percentile(array, 0.5);
  }

  public static void main(String[] args) throws Exception {
    String index = args[0], topics = args[1], prefix = args[2];
    float k1 = Float.parseFloat(args[3]), b = Float.parseFloat(args[4]);
    String stemmer = args[5];
    boolean keepStopwords = Boolean.parseBoolean(args[6]);
    int depth = Integer.parseInt(args[7]), latencyK = Integer.parseInt(args[8]);
    int latencyQueries = Integer.parseInt(args[9]), repetitions = Integer.parseInt(args[10]);
    int threads = Integer.parseInt(args[11]), warmups = Integer.parseInt(args[12]);

    analyzer = stemmer.equals("none")
        ? (keepStopwords ? DefaultEnglishAnalyzer.newNonStemmingInstance(CharArraySet.EMPTY_SET)
                         : DefaultEnglishAnalyzer.newNonStemmingInstance())
        : (keepStopwords ? DefaultEnglishAnalyzer.newStemmingInstance(stemmer, CharArraySet.EMPTY_SET)
                         : DefaultEnglishAnalyzer.newStemmingInstance(stemmer));
    IndexReader reader = DirectoryReader.open(FSDirectory.open(Paths.get(index)));
    searcher = new IndexSearcher(reader);
    searcher.setSimilarity(new BM25Similarity(k1, b));
    List<String> ids = new ArrayList<>(), texts = new ArrayList<>();
    for (String line : Files.readAllLines(Paths.get(topics), StandardCharsets.UTF_8)) {
      String[] fields = line.split("\t", 2);
      ids.add(fields[0]);
      texts.add(fields.length > 1 ? fields[1] : "");
    }
    int n = ids.size();
    ExecutorService pool = Executors.newFixedThreadPool(threads);

    // Quality run.
    List<Future<TopDocs>> futures = new ArrayList<>();
    for (String text : texts) futures.add(pool.submit(() -> search(text, depth)));
    StoredFields stored = reader.storedFields();
    try (BufferedWriter run = Files.newBufferedWriter(Paths.get(prefix + ".run"))) {
      for (int q = 0; q < n; ++q) {
        ScoreDoc[] hits = futures.get(q).get().scoreDocs;
        for (int r = 0; r < hits.length; ++r)
          run.write(ids.get(q) + " Q0 " + stored.document(hits[r].doc).get("id") + " " + (r + 1)
                    + " " + hits[r].score + " lucene\n");
      }
    }

    int rows = Math.min(latencyQueries, n);
    for (int w = 0; w < warmups; ++w)
      for (int q = 0; q < rows; ++q) search(texts.get(q), latencyK);
    List<Double> p50 = new ArrayList<>(), p95 = new ArrayList<>(), p99 = new ArrayList<>(),
                 mean = new ArrayList<>(), qps1 = new ArrayList<>(), qps = new ArrayList<>();
    double[][] perQuery = new double[rows][repetitions];
    for (int rep = 0; rep < repetitions; ++rep) {
      double[] latencies = new double[rows];
      double total = 0;
      for (int q = 0; q < rows; ++q) {
        long start = System.nanoTime();
        search(texts.get(q), latencyK);
        latencies[q] = (System.nanoTime() - start) / 1e6;
        perQuery[q][rep] = latencies[q];
        total += latencies[q];
      }
      p50.add(percentile(latencies, 0.50));
      p95.add(percentile(latencies, 0.95));
      p99.add(percentile(latencies, 0.99));
      mean.add(total / rows);
      qps1.add(1000.0 * rows / total);
    }
    for (int rep = 0; rep <= repetitions; ++rep) {
      long start = System.nanoTime();
      List<Future<TopDocs>> batch = new ArrayList<>();
      for (String text : texts) batch.add(pool.submit(() -> search(text, latencyK)));
      for (Future<TopDocs> f : batch) f.get();
      double seconds = (System.nanoTime() - start) / 1e9;
      if (rep > 0) qps.add(n / seconds);
    }
    pool.shutdown();
    try (PrintWriter raw = new PrintWriter(prefix + "_latency.csv", StandardCharsets.UTF_8)) {
      raw.println("query_index,query_id,median_latency_ms");
      for (int q = 0; q < rows; ++q)
        raw.printf(Locale.ROOT, "%d,%s,%.9g%n", q, ids.get(q), percentile(perQuery[q], 0.5));
    }
    Runtime runtime = Runtime.getRuntime();
    System.gc();
    double heapMib = (runtime.totalMemory() - runtime.freeMemory()) / (1024.0 * 1024.0);
    double[] sortedQps = qps.stream().mapToDouble(Double::doubleValue).toArray();
    try (PrintWriter summary = new PrintWriter(prefix + ".json", StandardCharsets.UTF_8)) {
      summary.printf(Locale.ROOT,
          "{%n  \"system\": \"lucene-bm25\",%n  \"lucene_version\": \"%s\",%n  \"java\": \"%s\",%n"
          + "  \"k1\": %s,%n  \"b\": %s,%n  \"stemmer\": \"%s\",%n  \"keep_stopwords\": %s,%n"
          + "  \"documents\": %d,%n  \"queries\": %d,%n  \"depth\": %d,%n  \"latency_k\": %d,%n"
          + "  \"latency_queries\": %d,%n  \"repetitions\": %d,%n  \"warmup_passes\": %d,%n"
          + "  \"lat_p50_ms\": %.9g,%n  \"lat_p95_ms\": %.9g,%n  \"lat_p99_ms\": %.9g,%n"
          + "  \"lat_mean_ms\": %.9g,%n  \"qps_single\": %.9g,%n  \"threads\": %d,%n"
          + "  \"qps_batch\": %.9g,%n  \"qps_batch_q1\": %.9g,%n  \"qps_batch_q3\": %.9g,%n"
          + "  \"jvm_heap_used_mib_after_gc\": %.6g%n}%n",
          org.apache.lucene.util.Version.LATEST, System.getProperty("java.version"), k1, b,
          stemmer, keepStopwords, reader.numDocs(), n, depth, latencyK, rows, repetitions,
          warmups, median(p50), median(p95), median(p99), median(mean), median(qps1), threads,
          percentile(sortedQps, 0.5), percentile(sortedQps, 0.25), percentile(sortedQps, 0.75),
          heapMib);
    }
    System.out.printf(Locale.ROOT, "lucene %s: p50 %.4f ms, batch QPS %.1f%n", index, median(p50),
                      percentile(sortedQps, 0.5));
    reader.close();
  }
}
