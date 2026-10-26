#pragma once

namespace aetherindex::lexical {

struct Bm25Config {
    double k1 = 1.2;
    double b = 0.75;
};

// Throws invalid_argument unless k1 is finite/positive and b is finite in [0,1].
void validate_bm25_config(const Bm25Config& config);

}  // namespace aetherindex::lexical
