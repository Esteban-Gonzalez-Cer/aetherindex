#include <exception>
#include <iostream>

#include "../bm25_test_helpers.hpp"

using namespace aetherindex::lexical;

int main() {
    try {
        Bm25Index index;
        require(index.document_count() == 0 && index.average_document_length() == 0.0,
                "New statistics must be zero.");
        addBm25Reference(index);
        require(index.document_count() == 3 && !index.is_built(), "Collecting counts invalid.");
        index.build();
        const auto& inverted = index.inverted_index();
        require(index.document_count() == 3 && inverted.total_token_count() == 6,
                "T3: N or total tokens failed.");
        require(inverted.document_length(101) == 3 && inverted.document_length(102) == 2 &&
                    inverted.document_length(103) == 1 && !inverted.document_length(99),
                "T3: document lengths failed.");
        require(inverted.document_frequency("motor") == 2 &&
                    inverted.document_frequency("e47") == 1 &&
                    inverted.document_frequency("temperatura") == 1 &&
                    inverted.document_frequency("bomba") == 1,
                "T3: DF confused with TF.");
        requireBm25Near(index.average_document_length(), 2.0, "T3: avgdl failed.");

        Bm25Index empty;
        empty.build();
        require(empty.document_count() == 0 && empty.average_document_length() == 0 &&
                    empty.inverted_index().vocabulary_size() == 0 &&
                    empty.search("motor", 10).empty(),
                "Empty corpus divided by zero or produced candidates.");
        Bm25Index zero;
        zero.add_document(0, "");
        zero.add_document(1, " -- !!! ");
        zero.build();
        require(zero.document_count() == 2 && zero.average_document_length() == 0 &&
                    zero.inverted_index().document_length(0) == 0 &&
                    zero.inverted_index().document_length(1) == 0 && zero.search("x", 10).empty(),
                "All-empty documents must still count in N with avgdl=0.");
        Bm25Index mixed;
        mixed.add_document(0, "");
        mixed.add_document(1, "word");
        mixed.add_document(2, "!!!");
        mixed.build();
        requireBm25Near(mixed.average_document_length(), 1.0 / 3.0,
                        "Empty documents must participate in the mean.");
        const auto hits = mixed.search("word", 10);
        require(hits.size() == 1 && hits[0].document_id == 1, "Empty documents filled ranking.");
        const double expected = std::log(1.0 + 2.5 / 1.5) * 2.2 / (1.0 + 1.2 * 2.5);
        requireBm25Near(hits[0].score, expected,
                        "Empty documents were excluded from IDF or avgdl.");
        std::cout << "T3/T10: N, DF, TF, lengths, avgdl and empty documents passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
