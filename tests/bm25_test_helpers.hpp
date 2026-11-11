#pragma once

#include <cmath>
#include <stdexcept>
#include <vector>

#include "aether/lexical/bm25.hpp"
#include "test_helpers.hpp"

inline void requireBm25Near(double actual, double expected, const char* message,
                            double tolerance = 1e-6) {
    require(
        std::isfinite(actual) && std::isfinite(expected) && std::abs(actual - expected) < tolerance,
        message);
}

template <class Error, class Function>
void requireBm25Error(Function function, const char* message) {
    try {
        function();
    } catch (const Error&) {
        return;
    }
    throw std::runtime_error(message);
}

inline void addBm25Reference(aetherindex::lexical::Bm25Index& index) {
    index.add_document(101, "motor motor e47");
    index.add_document(102, "motor temperatura");
    index.add_document(103, "bomba");
}

inline void requireSameRanking(const std::vector<aetherindex::RankedResult>& actual,
                               const std::vector<aetherindex::RankedResult>& expected) {
    require(actual.size() == expected.size(), "Ranking size differs.");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(actual[i].document_id == expected[i].document_id, "Ranking ID differs.");
        requireBm25Near(actual[i].score, expected[i].score, "Ranking score differs.");
    }
}
