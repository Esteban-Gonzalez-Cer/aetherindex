#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace aetherindex::lexical {

// ascii-tech-v1: ASCII lowercase, internal -/_, C++ (including C++17), repeated
// tokens retained. Valid UTF-8 is preserved, without Unicode case folding/NFC.
// Invalid UTF-8 throws invalid_argument. Returned strings own their contents.
class Tokenizer {
   public:
    [[nodiscard]] std::vector<std::string> tokenize(std::string_view text) const;
};

}  // namespace aetherindex::lexical
