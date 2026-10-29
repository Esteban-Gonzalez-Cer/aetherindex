#pragma once

#include "aether/lexical/tokenizer.hpp"

namespace aetherindex::lexical {

// Scoring strategy. All strategies return the same results with bitwise-identical
// scores; they differ in memory and speed. Exhaustive keeps the original raw postings
// (and InvertedIndex::find); the others build compressed 128-document blocks with
// per-block score upper bounds and release the raw postings after build().
//   TermAtATime  dense accumulator over every posting of the query terms.
//   Wand         document-at-a-time, skipping documents whose term bounds cannot
//                reach the current Top-K threshold.
//   BlockMaxWand Wand plus per-block bounds that skip whole blocks.
enum class Bm25Strategy { Exhaustive, TermAtATime, Wand, BlockMaxWand };

// How a term repeated in the analyzed query counts.
//   Unique     each distinct term contributes once (the default).
//   Frequency  a term's contribution is multiplied by its occurrences in the query, as in
//              Lucene/Anserini bag-of-words queries (each term boosted by its repetitions).
enum class QueryTermWeighting { Unique, Frequency };

struct Bm25Config {
    double k1 = 1.2;
    double b = 0.75;
    Bm25Strategy strategy = Bm25Strategy::Exhaustive;
    // Text analysis for documents and queries (see tokenizer.hpp).
    Analyzer analyzer = Analyzer::AsciiTechV1;
    QueryTermWeighting query_weighting = QueryTermWeighting::Unique;
};

// Throws invalid_argument unless k1 is finite/positive, b is finite in [0,1] and the
// strategy, analyzer and query weighting are known.
void validate_bm25_config(const Bm25Config& config);

}  // namespace aetherindex::lexical
