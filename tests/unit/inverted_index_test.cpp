#include "aether/lexical/inverted_index.hpp"

#include <exception>
#include <iostream>
#include <limits>
#include <type_traits>

#include "../bm25_test_helpers.hpp"

using namespace aetherindex;
using namespace aetherindex::lexical;

int main() {
    try {
        InvertedIndex index;
        requireBm25Error<std::logic_error>([&] { return index.find("motor"); },
                                           "Unbuilt postings exposed.");
        index.add_document(102, "motor temperatura");
        index.add_document(101, "motor motor e47");
        index.add_document(103, "bomba");
        requireBm25Error<std::invalid_argument>([&] { index.add_document(101, "replacement"); },
                                                "Duplicate ID accepted.");
        requireBm25Error<std::invalid_argument>([&] { index.add_document(999, "invalid\xFF"); },
                                                "Invalid UTF-8 document accepted.");
        require(index.document_count() == 3 && index.total_token_count() == 6 &&
                    !index.document_length(999),
                "Rejected insert altered contents.");
        index.build();
        const auto list = index.find("motor");
        static_assert(std::is_same_v<decltype(list)::element_type, const Posting>);
        require(list.size() == 2 && list[0].document_id == 101 && list[0].term_frequency == 2 &&
                    list[1].document_id == 102 && list[1].term_frequency == 1,
                "T2: TF, ordering or deduplication failed.");
        require(index.find("e47").size() == 1 && index.find("bomba")[0].document_id == 103,
                "Term association failed.");
        require(index.find("missing").empty() && index.document_frequency("missing") == 0,
                "Missing term must be empty.");
        require(index.find("MOTOR").empty(), "Raw term lookup must not tokenize implicitly.");
        require(index.vocabulary_size() == 4, "Unexpected vocabulary size.");
        require(index.find("motor").data() == list.data(), "Read view was copied or invalidated.");
        requireBm25Error<std::logic_error>([&] { index.build(); }, "Repeated build accepted.");
        requireBm25Error<std::logic_error>([&] { index.add_document(1000, "new"); },
                                           "Post-build mutation accepted.");
        require(index.find("motor").data() == list.data(), "Rejected operations invalidated view.");

        InvertedIndex ids;
        ids.add_document(std::numeric_limits<DocumentId>::max(), "shared shared");
        ids.add_document(0, "shared");
        ids.add_document(427, "shared");
        ids.build();
        const auto extremes = ids.find("shared");
        require(extremes.size() == 3 && extremes[0].document_id == 0 &&
                    extremes[1].document_id == 427 &&
                    extremes[2].document_id == std::numeric_limits<DocumentId>::max(),
                "IDs were confused with dense positions or restricted.");
        std::cout
            << "T2: postings, duplicate rejection, read-only views and full ID range passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
