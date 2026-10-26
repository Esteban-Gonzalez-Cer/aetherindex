#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>

#include "../test_helpers.hpp"
#include "aether/ann/hnsw/hnsw_index.hpp"
#include "aether/storage/vector_store.hpp"

// Allocation interception is confined to this executable. Production targets
// have no fault-injection switches. Aligned allocations are intercepted too.
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {
std::ptrdiff_t allocations_before_failure = -1;
bool trace_graph = false;
struct Allocation {
    void* pointer = nullptr;
    std::size_t bytes = 0;
};
std::array<Allocation, 256> graph_allocations{};
void trace(void* pointer, std::size_t bytes) {
    if (!trace_graph) return;
    for (auto& allocation : graph_allocations) {
        if (!allocation.pointer) {
            allocation = {pointer, bytes};
            return;
        }
    }
    std::abort();
}
void untrace(void* pointer) noexcept {
    if (!pointer) return;
    for (auto& allocation : graph_allocations) {
        if (allocation.pointer == pointer) {
            allocation = {};
            return;
        }
    }
}
}  // namespace

void* operator new(std::size_t bytes) {
    if (allocations_before_failure == 0) {
        throw std::bad_alloc();
    }
    if (allocations_before_failure > 0) {
        --allocations_before_failure;
    }
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) {
        trace(memory, bytes);
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    if (allocations_before_failure == 0) throw std::bad_alloc();
    if (allocations_before_failure > 0) --allocations_before_failure;
    const auto align = static_cast<std::size_t>(alignment);
    if (bytes == 0) bytes = align;
#if defined(_MSC_VER)
    void* memory = _aligned_malloc(bytes, align);
#else
    void* memory = std::aligned_alloc(align, ((bytes + align - 1) / align) * align);
#endif
    if (!memory) throw std::bad_alloc();
    trace(memory, bytes);
    return memory;
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return ::operator new(bytes, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept {
    untrace(memory);
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}
void operator delete[](void* memory, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete(void* memory) noexcept {
    untrace(memory);
    std::free(memory);
}

void operator delete[](void* memory) noexcept { ::operator delete(memory); }

#if defined(__cpp_sized_deallocation)
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }

void operator delete[](void* memory, std::size_t) noexcept { ::operator delete[](memory); }
void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}
void operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}
#endif

namespace {
using namespace aetherindex;

std::array<float, 3> valuesFor(VectorId id, DistanceKind metric = DistanceKind::SquaredEuclidean) {
    const float value = static_cast<float>(id);
    if (metric == DistanceKind::Cosine) return {0, value + 1.0f, 0};
    return {value + 1.0f, value + 2.0f, value + 3.0f};
}

void checkExistingEntries(const VectorStore& store, VectorId count) {
    for (VectorId id = 0; id < count; ++id) {
        const auto recovered = store.find(id);
        auto expected = valuesFor(id, store.distance_kind());
        if (store.distance_kind() == DistanceKind::Cosine) expected = {0, 1, 0};
        require(recovered.has_value() && store.ids()[id] == id,
                "Allocation failure corrupted the ID map or insertion order.");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            require((*recovered)[i] == expected[i], "Allocation failure changed existing values.");
        }
    }
}

void checkFailurePoints(VectorId initial_count, DistanceKind metric) {
    bool reached_success = false;
    std::size_t failure_points = 0;
    for (std::ptrdiff_t fail_at = 0; fail_at < 64; ++fail_at) {
        VectorStore store(3, metric);
        for (VectorId id = 0; id < initial_count; ++id) {
            const auto values = valuesFor(id, metric);
            require(store.insert(id, values) == InsertStatus::Success, "Setup insertion failed.");
        }
        const auto expected_new = valuesFor(0, metric);
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
              << initial_count << " existing embeddings, metric=" << static_cast<int>(metric)
              << ".\n";
}

void checkPersistentGraphAllocations() {
    VectorStore store(3, DistanceKind::SquaredEuclidean);
    store.reserve(1000);
    for (VectorId id = 0; id < 1000; ++id) {
        const std::array<float, 3> values{static_cast<float>(id % 31), static_cast<float>(id / 31),
                                          1};
        require(store.insert(id * 7, values) == InsertStatus::Success,
                "Allocation graph fixture failed.");
    }
    trace_graph = true;
    {
        HnswIndex index(store);
        for (VectorId id : store.ids()) index.insert(id);
        trace_graph = false;
        std::size_t blocks = 0, bytes = 0;
        for (const auto& allocation : graph_allocations) {
            if (allocation.pointer) {
                ++blocks;
                bytes += allocation.bytes;
            }
        }
        const auto usage = index.memory_usage();
        require(blocks == 4 && blocks == usage.allocation_blocks,
                "Dynamic audit: graph must retain only four global allocations after 1000 nodes.");
        require(bytes + usage.object_bytes == usage.total_bytes(),
                "Dynamic audit must match actual reserved graph buffer bytes.");
        require(index.validate_graph().valid(), "Audited graph must be structurally valid.");
        std::cout << "CA-2.1 dynamic allocation audit: N=1000, persistent blocks=" << blocks
                  << ", bytes=" << bytes << '\n';
    }
    for (const auto& allocation : graph_allocations) {
        require(!allocation.pointer, "Destroying the graph must free every audited allocation.");
    }
}
}  // namespace

int main() {
    try {
        for (auto metric :
             {aetherindex::DistanceKind::Cosine, aetherindex::DistanceKind::SquaredEuclidean}) {
            for (aetherindex::VectorId count : {0u, 1u, 2u, 13u, 29u})
                checkFailurePoints(count, metric);
        }
        checkPersistentGraphAllocations();
    } catch (const std::exception& error) {
        allocations_before_failure = -1;
        trace_graph = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
