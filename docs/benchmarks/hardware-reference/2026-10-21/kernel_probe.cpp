// Diagnostic for hardware-reference evidence; not a latency benchmark.
#include <cmath>
#include <iostream>
#include "aether/distance/kernels.hpp"
int main() {
    alignas(64) float a[128]{};
    alignas(64) float b[128]{};
    a[0] = 1.0f; b[1] = 1.0f;
    const float cosine = aetherindex::detail::cosine_unit_kernel(a, b, 128);
    const float squared = aetherindex::detail::squared_euclidean_kernel(a, b, 128);
    std::cout << "backend=" << aetherindex::detail::distance_kernel_backend()
              << "\ncosine=" << cosine << "\nsquared_euclidean=" << squared
              << "\ncompiler=" << __VERSION__
#ifdef _LIBCPP_VERSION
              << "\nlibcpp_version=" << _LIBCPP_VERSION
#endif
#ifdef __FAST_MATH__
              << "\nfast_math=true"
#else
              << "\nfast_math=false"
#endif
              << "\n";
    return std::abs(cosine - 1.0f) < 1e-5f &&
           std::abs(squared - 2.0f) < 1e-5f ? 0 : 1;
}
