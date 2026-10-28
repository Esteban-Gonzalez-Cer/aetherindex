#pragma once

namespace aetherindex::hybrid {

struct RrfConfig {
    double k_rrf = 60.0;
};

// Throws invalid_argument unless k_rrf is finite and strictly positive.
void validate_rrf_config(const RrfConfig& config);

}  // namespace aetherindex::hybrid
