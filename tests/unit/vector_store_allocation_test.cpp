#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>

#include "../test_helpers.hpp"
#include "aether/storage/vector_store.hpp"

// Allocation interception is confined to this executable. Production targets
// have no allocation hooks, custom allocators, or fault-injection switches.
namespace {
std::ptrdiff_t allocations_before_failure = -1;
}

void* operator new(std::size_t bytes) {
    if (allocations_before_failure == 0) {
        throw std::bad_alloc();
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete[](void* memory) noexcept { ::operator delete(memory); }

#if defined(__cpp_sized_deallocation)
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }

void operator delete[](void* memory, std::size_t) noexcept { ::operator delete[](memory); }
#endif

namespace {
using namespace aetherindex;

std::array<float, 3> valuesFor(VectorId id) {
    const float value = static_cast<float>(id);
    return {value + 1.0f, value + 2.0f, value + 3.0f};
}

void checkExistingEntries(const VectorStore& store, VectorId count) {
    for (VectorId id = 0; id < count; ++id) {
        const auto recovered = store.find(id);
        const auto expected = valuesFor(id);
        require(recovered.has_value() && store.ids()[id] == id,
                "Allocation failure corrupted the ID map or insertion order.");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            require((*recovered)[i] == expected[i], "Allocation failure changed existing values.");
        }
    }
}

void checkFailurePoints(VectorId initial_count) {
    bool reached_success = false;
    std::size_t failure_points = 0;
    for (std::ptrdiff_t fail_at = 0; fail_at < 64; ++fail_at) {
        VectorStore store(3);
        for (VectorId id = 0; id < initial_count; ++id) {
            const auto values = valuesFor(id);
            require(store.insert(id, values) == InsertStatus::Success, "Setup insertion failed.");
        }
        const auto expected_new = valuesFor(0);
        const std::span<const float> input =
            initial_count == 0 ? std::span<const float>(expected_new) : *store.find(0);
        bool failed = false;
        InsertStatus status = InsertStatus::InvalidEmbedding;
        allocations_before_failure = fail_at;
        try {
            status = store.insert(1000, input);
        } catch (const std::bad_alloc&) {
            failed = true;
        } catch (...) {
            allocations_before_failure = -1;
            throw;
        }
        allocations_before_failure = -1;
        // input may now be invalid, including after rollback. Reacquire views.
        checkExistingEntries(store, initial_count);
        if (failed) {
            ++failure_points;
            require(store.size() == initial_count && store.ids().size() == initial_count,
                    "Failed insertion must restore both counts.");
            require(!store.contains(1000) && !store.find(1000),
                    "Failed insertion must not register the new ID.");
            require(store.insert(1000, expected_new) == InsertStatus::Success,
                    "The store must remain usable after an allocation failure.");
        } else {
            require(status == InsertStatus::Success, "Insertion was unexpectedly rejected.");
            reached_success = true;
        }
        require(store.size() == initial_count + 1 && store.ids().back() == 1000,
                "Successful retry must commit exactly one entry.");
        const auto inserted = *store.find(1000);
        for (std::size_t i = 0; i < expected_new.size(); ++i) {
            require(inserted[i] == expected_new[i], "Successful retry stored incorrect values.");
        }
        checkExistingEntries(store, initial_count);
        if (reached_success) {
            break;
        }
    }
    require(reached_success && failure_points >= 2,
            "Every allocation failure point must be exercised before a successful insertion.");
    std::cout << "Rollback verified at " << failure_points << " allocation points with "
              << initial_count << " existing embeddings.\n";
}
}  // namespace

int main() {
    try {
        for (aetherindex::VectorId count : {0u, 1u, 2u, 13u, 29u}) {
            checkFailurePoints(count);
        }
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
