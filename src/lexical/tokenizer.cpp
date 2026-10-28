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

}  // namespace

std::vector<std::string> Tokenizer::tokenize(std::string_view text) const {
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

}  // namespace aetherindex::lexical
