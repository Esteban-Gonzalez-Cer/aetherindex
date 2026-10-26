#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>

#include "../bm25_test_helpers.hpp"

// Isolated executable: no fault hooks in the production library. Lexical
// containers use ordinary (not over-aligned) allocations.
namespace {
std::ptrdiff_t allocations_left = -1;
}

void* operator new(std::size_t bytes) {
    if (allocations_left == 0) throw std::bad_alloc();
    if (allocations_left > 0) --allocations_left;
    if (void* pointer = std::malloc(bytes == 0 ? 1 : bytes)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete[](pointer); }
#endif

namespace {
using namespace aetherindex::lexical;

template <class Function>
bool inject(std::ptrdiff_t point, Function action) {
    allocations_left = point;
    bool failed = false;
    try {
        action();
    } catch (const std::bad_alloc&) {
        failed = true;
    } catch (...) {
        allocations_left = -1;
        throw;
    }
    allocations_left = -1;
    return failed;
}

void check_insert() {
    std::size_t failures = 0;
    bool success = false;
    for (std::ptrdiff_t point = 0; point < 256; ++point) {
        Bm25Index index;
        addBm25Reference(index);
        const bool failed = inject(point, [&] {
            index.add_document(999,
                               "additional additional term with independent owned frequencies");
        });
        if (failed) {
            ++failures;
            require(index.document_count() == 3 && !index.inverted_index().document_length(999) &&
                        index.inverted_index().total_token_count() == 6,
                    "Failed insert changed logical contents.");
            index.add_document(999, "additional");
        } else {
            success = true;
        }
        index.build();
        const auto found = index.search("additional", 10);
        require(found.size() == 1 && found[0].document_id == 999,
                "Reuse after failed insert failed.");
        if (!failed) break;
    }
    require(failures > 0 && success, "Did not exercise every insertion allocation failure.");
    std::cout << "insert allocation failure points=" << failures << '\n';
}

void check_build() {
    std::size_t failures = 0;
    bool success = false;
    for (std::ptrdiff_t point = 0; point < 256; ++point) {
        Bm25Index index;
        addBm25Reference(index);
        const bool failed = inject(point, [&] { index.build(); });
        if (failed) {
            ++failures;
            require(!index.is_built() && index.document_count() == 3 &&
                        index.inverted_index().total_token_count() == 6,
                    "Failed build published READY or changed statistics.");
            requireBm25Error<std::logic_error>([&] { return index.search("motor", 10); },
                                               "Partial index became queryable.");
            index.add_document(999, "");
            index.build();
        } else {
            success = true;
        }
        const auto found = index.search("motor", 10);
        require(found.size() == 2 && found[0].document_id == 101 && found[1].document_id == 102,
                "Build retry lost pending frequencies.");
        if (!failed) break;
    }
    require(failures > 0 && success, "Did not exercise every build allocation failure.");
    std::cout << "build allocation failure points=" << failures << '\n';
}

void check_search() {
    Bm25Index index;
    addBm25Reference(index);
    index.build();
    const auto expected = index.search("motor e47", 10);
    const auto* view = index.inverted_index().find("motor").data();
    std::size_t failures = 0;
    bool success = false;
    for (std::ptrdiff_t point = 0; point < 256; ++point) {
        const bool failed =
            inject(point, [&] { static_cast<void>(index.search("motor e47", 10)); });
        if (failed)
            ++failures;
        else
            success = true;
        require(index.is_built() && index.inverted_index().find("motor").data() == view,
                "Failed search mutated index or views.");
        requireSameRanking(index.search("motor e47", 10), expected);
        if (!failed) break;
    }
    require(failures > 0 && success, "Did not exercise search allocation failure.");
    std::cout << "search allocation failure points=" << failures << '\n';
}
}  // namespace

int main() {
    try {
        check_insert();
        check_build();
        check_search();
    } catch (const std::exception& error) {
        allocations_left = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
