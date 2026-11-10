#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aetherindex::lexical {

// Text analysis applied identically to documents and queries.
//   AsciiTechV1  ascii-tech-v1: ASCII lowercase, internal -/_, C++ (including C++17),
//                repeated tokens retained. The default.
//   EnglishV1    english-v1: modeled on Lucene's EnglishAnalyzer for Latin text. Words
//                split at hyphens and at punctuation/symbol code points (e.g. ± × ≥);
//                '_' joins; '.', ':', '·' and apostrophes join two letters; '.', ',', ';'
//                and apostrophes join two digits. Then the English possessive 's is
//                removed, Latin/Greek/Cyrillic letters are lowercased, Lucene's 33
//                English stopwords are dropped and the classic Porter stemmer is applied.
//   EnglishPlainV1  english-plain-v1: english-v1 without stopword removal and without
//                stemming (tokenization, possessive removal and lowercasing only). The
//                counterpart of Lucene/Anserini with -stemmer none -keepStopwords.
// None applies NFC or full Unicode case folding; ascii-tech-v1 lowercases ASCII only.
enum class Analyzer { AsciiTechV1, EnglishV1, EnglishPlainV1 };

[[nodiscard]] const char* analyzer_name(Analyzer analyzer) noexcept;

// Invalid UTF-8 throws invalid_argument. Returned strings own their contents.
// Memoized Porter stems (word -> stem). Owned by one thread; an indexer reuses one for all
// its documents, so each distinct word is stemmed once.
using StemCache = std::unordered_map<std::string, std::string>;

class Tokenizer {
   public:
    explicit Tokenizer(Analyzer analyzer = Analyzer::AsciiTechV1) : analyzer_(analyzer) {}
    [[nodiscard]] std::vector<std::string> tokenize(std::string_view text) const;
    // Same tokens; english-v1 stems through `cache`.
    [[nodiscard]] std::vector<std::string> tokenize(std::string_view text, StemCache& cache) const;

   private:
    Analyzer analyzer_;
};

// Classic Porter (1980) stemmer as published by Martin Porter (with his 'bli'/'logi'
// departures, as in Lucene's PorterStemmer). Words of two letters or fewer are unchanged.
[[nodiscard]] std::string porter_stem(std::string word);

}  // namespace aetherindex::lexical
