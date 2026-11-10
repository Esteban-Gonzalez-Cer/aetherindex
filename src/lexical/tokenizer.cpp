#include "aether/lexical/tokenizer.hpp"

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace aetherindex::lexical {
namespace {

struct CodePoint {
    std::uint32_t value;
    std::size_t bytes;
};

// Decode complete RFC 3629 sequences, rejecting overlongs, surrogates and >U+10FFFF.
CodePoint decode(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {first, 1};
    const std::size_t length = first >= 0xC2 && first <= 0xDF   ? 2
                               : first >= 0xE0 && first <= 0xEF ? 3
                               : first >= 0xF0 && first <= 0xF4 ? 4
                                                                : 0;
    if (length == 0 || length > text.size() - offset) {
        throw std::invalid_argument("Invalid UTF-8 text.");
    }
    std::uint32_t value = first & (0x7F >> length);
    for (std::size_t i = 1; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(text[offset + i]);
        if ((continuation & 0xC0) != 0x80) {
            throw std::invalid_argument("Invalid UTF-8 continuation.");
        }
        value = (value << 6) | (continuation & 0x3F);
    }
    const std::uint32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        throw std::invalid_argument("Invalid UTF-8 code point.");
    }
    return {value, length};
}

bool ascii_word(std::uint32_t value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

// Explicit separators only; not a complete Unicode category database.
bool unicode_separator(std::uint32_t value) {
    return value == 0x85 || value == 0xA0 || value == 0x1680 || value == 0xFEFF ||
           (value >= 0x2000 && value <= 0x206F) || (value >= 0x3000 && value <= 0x303F) ||
           (value >= 0xFF01 && value <= 0xFF0F) || (value >= 0xFF1A && value <= 0xFF20) ||
           (value >= 0xFF3B && value <= 0xFF40) || (value >= 0xFF5B && value <= 0xFF65);
}

bool word(CodePoint point) {
    return ascii_word(point.value) || (point.value >= 0x80 && !unicode_separator(point.value));
}

std::vector<std::string> tokenize_ascii_tech(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto point = decode(text, i);
        if (word(point)) {
            if (point.value >= 'A' && point.value <= 'Z') {
                current.push_back(static_cast<char>(point.value + ('a' - 'A')));
            } else {
                current.append(text.substr(i, point.bytes));
            }
        } else if ((point.value == '-' || point.value == '_') && !current.empty() &&
                   i + 1 < text.size() && word(decode(text, i + 1))) {
            current.push_back(static_cast<char>(point.value));
        } else if (point.value == '+' && current == "c" && i + 1 < text.size() &&
                   text[i + 1] == '+') {
            current += "++";
            i += 2;
            continue;
        } else {
            flush();
        }
        i += point.bytes;
    }
    flush();
    return tokens;
}

// --- english-v1 ---------------------------------------------------------------------

enum class Kind { Letter, Digit };

Kind kind(std::uint32_t value) { return value >= '0' && value <= '9' ? Kind::Digit : Kind::Letter; }

bool apostrophe(std::uint32_t value) {
    return value == '\'' || value == 0x2018 || value == 0x2019 || value == 0xFF07;
}

// UAX #29 WB6/7 (MidLetter, MidNumLet) and WB11/12 (MidNum, MidNumLet), ASCII-centric.
bool joins_letters(std::uint32_t value) {
    return value == '.' || value == ':' || value == 0xB7 || apostrophe(value);
}
bool joins_digits(std::uint32_t value) {
    return value == '.' || value == ',' || value == ';' || apostrophe(value);
}

// Word characters for english-v1: ASCII letters/digits and non-ASCII code points outside
// the punctuation and symbol blocks (Latin-1 punctuation/symbols, x and division signs,
// general punctuation, super/subscript digits and signs, currency, number forms, arrows, mathematical operators, technical
// and geometric symbols, dingbats, supplemental punctuation, CJK punctuation, emoji).
// Lucene's StandardTokenizer also splits there, so 2±0.5 or 10×3 do not become one token.
bool english_word(std::uint32_t value) {
    if (value < 0x80) return ascii_word(value);
    if (value == 0xAA || value == 0xB5 || value == 0xBA) return true;  // ª µ º are letters.
    if ((value >= 0x80 && value <= 0xBF) || value == 0xD7 || value == 0xF7) return false;
    if (value >= 0x2000 && value <= 0x206F) return false;
    // Superscript/subscript digits and signs (the modifier letters ⁱ ⁿ ₐ... stay).
    if (value == 0x2070 || (value >= 0x2074 && value <= 0x207E) ||
        (value >= 0x2080 && value <= 0x208E))
        return false;
    if (value >= 0x20A0 && value <= 0x20FF) return false;
    if (value >= 0x2150 && value <= 0x2BFF) return false;
    if (value >= 0x2E00 && value <= 0x2E7F) return false;
    if (value >= 0x1F000 && value <= 0x1FAFF) return false;
    return !unicode_separator(value);
}

// Simple lowercase mapping for Latin-1, Latin Extended-A, Greek and Cyrillic capitals
// (plus the Angstrom and Ohm signs), approximating Lucene's LowerCaseFilter there.
std::uint32_t lowercase(std::uint32_t value) {
    if (value >= 'A' && value <= 'Z') return value + 32;
    if (value >= 0xC0 && value <= 0xDE && value != 0xD7) return value + 32;
    if (value >= 0x100 && value <= 0x17F && value != 0x130 && value != 0x131 && value != 0x138 &&
        value != 0x149 && value != 0x17F) {
        const bool odd_capitals = (value >= 0x139 && value <= 0x148) || value >= 0x179;
        return (value % 2 == (odd_capitals ? 1u : 0u)) ? value + 1 : value;
    }
    if (value >= 0x391 && value <= 0x3A9 && value != 0x3A2) return value + 32;
    if (value >= 0x410 && value <= 0x42F) return value + 32;
    if (value >= 0x400 && value <= 0x40F) return value + 80;
    if (value == 0x212B) return 0xE5;
    if (value == 0x2126) return 0x3C9;
    return value;
}

void append_utf8(std::string& out, std::uint32_t value) {
    if (value < 0x80) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (value >> 18)));
        out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

bool ends_with(std::string_view token, std::string_view suffix) {
    return token.size() >= suffix.size() && token.substr(token.size() - suffix.size()) == suffix;
}

// Lucene EnglishAnalyzer.ENGLISH_STOP_WORDS_SET.
bool stopword(std::string_view token) {
    static constexpr std::string_view kStopwords[] = {
        "a",    "an",    "and",   "are",  "as",    "at",   "be",   "but",   "by",
        "for",  "if",    "in",    "into", "is",    "it",   "no",   "not",   "of",
        "on",   "or",    "such",  "that", "the",   "their", "then", "there", "these",
        "they", "this",  "to",    "was",  "will",  "with"};
    for (const auto word : kStopwords)
        if (word == token) return true;
    return false;
}

std::vector<std::string> tokenize_english(std::string_view text, StemCache* cache) {
    std::vector<std::string> tokens;
    std::string current;
    Kind last = Kind::Letter;
    const auto flush = [&] {
        if (current.empty()) return;
        // EnglishPossessiveFilter: 's after an apostrophe (', U+2019, U+FF07).
        if (ends_with(current, "'s")) {
            current.resize(current.size() - 2);
        } else if (ends_with(current, "\xE2\x80\x99s") || ends_with(current, "\xEF\xBC\x87s")) {
            current.resize(current.size() - 4);
        }
        if (!current.empty() && !stopword(current)) {
            if (!cache) {
                tokens.push_back(porter_stem(current));
            } else {
                auto found = cache->find(current);
                if (found == cache->end()) found = cache->emplace(current, porter_stem(current)).first;
                tokens.push_back(found->second);
            }
        }
        current.clear();
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto point = decode(text, i);
        if (english_word(point.value)) {
            append_utf8(current, lowercase(point.value));
            last = kind(point.value);
        } else if (!current.empty() && i + point.bytes < text.size() &&
                   (point.value == '_' || joins_letters(point.value) ||
                    joins_digits(point.value))) {
            const auto next = decode(text, i + point.bytes);
            const bool joined =
                english_word(next.value) &&
                (point.value == '_' ||
                 (last == Kind::Letter && kind(next.value) == Kind::Letter &&
                  joins_letters(point.value)) ||
                 (last == Kind::Digit && kind(next.value) == Kind::Digit &&
                  joins_digits(point.value)));
            if (joined) {
                current.append(text.substr(i, point.bytes));
            } else {
                flush();
            }
        } else {
            flush();
        }
        i += point.bytes;
    }
    flush();
    return tokens;
}

// Porter stemmer over bytes b[0..k], following Martin Porter's reference C code.
class Porter {
   public:
    explicit Porter(std::string& word) : b_(word), k_(static_cast<int>(word.size()) - 1) {}

    void run() {
        if (k_ <= 1) return;
        step1ab();
        step1c();
        step2();
        step3();
        step4();
        step5();
        b_.resize(static_cast<std::size_t>(k_ + 1));
    }

   private:
    bool cons(int i) const {
        switch (b_[i]) {
            case 'a':
            case 'e':
            case 'i':
            case 'o':
            case 'u':
                return false;
            case 'y':
                return i == 0 ? true : !cons(i - 1);
            default:
                return true;
        }
    }
    // Number of VC sequences in b[0..j].
    int m() const {
        int n = 0, i = 0;
        for (;; ++i) {
            if (i > j_) return n;
            if (!cons(i)) break;
        }
        ++i;
        for (;;) {
            for (;; ++i) {
                if (i > j_) return n;
                if (cons(i)) break;
            }
            ++i;
            ++n;
            for (;; ++i) {
                if (i > j_) return n;
                if (!cons(i)) break;
            }
            ++i;
        }
    }
    bool vowel_in_stem() const {
        for (int i = 0; i <= j_; ++i)
            if (!cons(i)) return true;
        return false;
    }
    bool double_c(int j) const { return j >= 1 && b_[j] == b_[j - 1] && cons(j); }
    bool cvc(int i) const {
        if (i < 2 || !cons(i) || cons(i - 1) || !cons(i - 2)) return false;
        const char ch = b_[i];
        return ch != 'w' && ch != 'x' && ch != 'y';
    }
    bool ends(std::string_view s) {
        const int length = static_cast<int>(s.size());
        if (length > k_ + 1) return false;
        if (std::string_view(b_).substr(static_cast<std::size_t>(k_ + 1 - length),
                                        static_cast<std::size_t>(length)) != s)
            return false;
        j_ = k_ - length;
        return true;
    }
    void set_to(std::string_view s) {
        b_.replace(static_cast<std::size_t>(j_ + 1), std::string::npos, s);
        k_ = j_ + static_cast<int>(s.size());
    }
    void r(std::string_view s) {
        if (m() > 0) set_to(s);
    }
    void truncate() { b_.resize(static_cast<std::size_t>(k_ + 1)); }

    void step1ab() {
        truncate();
        if (b_[k_] == 's') {
            if (ends("sses")) {
                k_ -= 2;
            } else if (ends("ies")) {
                set_to("i");
            } else if (b_[k_ - 1] != 's') {
                --k_;
            }
        }
        truncate();
        if (ends("eed")) {
            if (m() > 0) --k_;
        } else if ((ends("ed") || ends("ing")) && vowel_in_stem()) {
            k_ = j_;
            truncate();
            if (ends("at")) {
                set_to("ate");
            } else if (ends("bl")) {
                set_to("ble");
            } else if (ends("iz")) {
                set_to("ize");
            } else if (double_c(k_)) {
                --k_;
                const char ch = b_[k_];
                if (ch == 'l' || ch == 's' || ch == 'z') ++k_;
            } else {
                j_ = k_;
                if (m() == 1 && cvc(k_)) set_to("e");
            }
        }
        truncate();
    }
    void step1c() {
        if (ends("y") && vowel_in_stem()) b_[k_] = 'i';
    }
    void step2() {
        if (k_ < 1) return;
        switch (b_[k_ - 1]) {
            case 'a':
                if (ends("ational")) r("ate");
                else if (ends("tional")) r("tion");
                break;
            case 'c':
                if (ends("enci")) r("ence");
                else if (ends("anci")) r("ance");
                break;
            case 'e':
                if (ends("izer")) r("ize");
                break;
            case 'l':
                if (ends("bli")) r("ble");
                else if (ends("alli")) r("al");
                else if (ends("entli")) r("ent");
                else if (ends("eli")) r("e");
                else if (ends("ousli")) r("ous");
                break;
            case 'o':
                if (ends("ization")) r("ize");
                else if (ends("ation")) r("ate");
                else if (ends("ator")) r("ate");
                break;
            case 's':
                if (ends("alism")) r("al");
                else if (ends("iveness")) r("ive");
                else if (ends("fulness")) r("ful");
                else if (ends("ousness")) r("ous");
                break;
            case 't':
                if (ends("aliti")) r("al");
                else if (ends("iviti")) r("ive");
                else if (ends("biliti")) r("ble");
                break;
            case 'g':
                if (ends("logi")) r("log");
                break;
            default:
                break;
        }
        truncate();
    }
    void step3() {
        switch (b_[k_]) {
            case 'e':
                if (ends("icate")) r("ic");
                else if (ends("ative")) r("");
                else if (ends("alize")) r("al");
                break;
            case 'i':
                if (ends("iciti")) r("ic");
                break;
            case 'l':
                if (ends("ical")) r("ic");
                else if (ends("ful")) r("");
                break;
            case 's':
                if (ends("ness")) r("");
                break;
            default:
                break;
        }
        truncate();
    }
    void step4() {
        if (k_ < 1) return;
        bool matched = false;
        switch (b_[k_ - 1]) {
            case 'a':
                matched = ends("al");
                break;
            case 'c':
                matched = ends("ance") || ends("ence");
                break;
            case 'e':
                matched = ends("er");
                break;
            case 'i':
                matched = ends("ic");
                break;
            case 'l':
                matched = ends("able") || ends("ible");
                break;
            case 'n':
                matched = ends("ant") || ends("ement") || ends("ment") || ends("ent");
                break;
            case 'o':
                matched = (ends("ion") && j_ >= 0 && (b_[j_] == 's' || b_[j_] == 't')) ||
                          ends("ou");
                break;
            case 's':
                matched = ends("ism");
                break;
            case 't':
                matched = ends("ate") || ends("iti");
                break;
            case 'u':
                matched = ends("ous");
                break;
            case 'v':
                matched = ends("ive");
                break;
            case 'z':
                matched = ends("ize");
                break;
            default:
                break;
        }
        if (matched && m() > 1) k_ = j_;
        truncate();
    }
    void step5() {
        j_ = k_;
        if (b_[k_] == 'e') {
            const int a = m();
            if (a > 1 || (a == 1 && !cvc(k_ - 1))) --k_;
        }
        if (b_[k_] == 'l' && double_c(k_) && m() > 1) --k_;
        truncate();
    }

    std::string& b_;
    int k_;
    int j_ = 0;
};

}  // namespace

const char* analyzer_name(Analyzer analyzer) noexcept {
    return analyzer == Analyzer::EnglishV1 ? "english-v1" : "ascii-tech-v1";
}

std::vector<std::string> Tokenizer::tokenize(std::string_view text) const {
    return analyzer_ == Analyzer::EnglishV1 ? tokenize_english(text, nullptr)
                                            : tokenize_ascii_tech(text);
}

std::vector<std::string> Tokenizer::tokenize(std::string_view text, StemCache& cache) const {
    return analyzer_ == Analyzer::EnglishV1 ? tokenize_english(text, &cache)
                                            : tokenize_ascii_tech(text);
}

std::string porter_stem(std::string word) {
    Porter(word).run();
    return word;
}

}  // namespace aetherindex::lexical
