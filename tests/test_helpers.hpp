#pragma once

#include <cmath>
#include <stdexcept>

inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void requireNear(float actual, float expected, const char* message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= 0.00001f, message);
}

template <typename Function>
void requireInvalidArgument(Function function, const char* message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}

template <typename Function>
void requireOverflow(Function function, const char* message) {
    try {
        function();
    } catch (const std::overflow_error&) {
        return;
    }
    throw std::runtime_error(message);
}
