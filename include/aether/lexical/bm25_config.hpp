#pragma once

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

struct Bm25Config {
    double k1 = 1.2;
    double b = 0.75;
    Bm25Strategy strategy = Bm25Strategy::Exhaustive;
};

// Throws invalid_argument unless k1 is finite/positive, b is finite in [0,1] and the
// strategy is known.
void validate_bm25_config(const Bm25Config& config);

}  // namespace aetherindex::lexical
