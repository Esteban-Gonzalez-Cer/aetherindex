#include <exception>
#include <iostream>
#include <limits>

#include "../bm25_test_helpers.hpp"

using namespace aetherindex::lexical;

int main() {
    try {
        requireBm25Near(compute_idf(3, 1), 0.9808292530, "T4: rare-term IDF failed.");
        requireBm25Near(compute_idf(3, 2), 0.4700036292, "T4: common-term IDF failed.");
        require(compute_idf(3, 1) > compute_idf(3, 2) && compute_idf(3, 3) > 0,
                "IDF must be positive and decrease with DF.");
        for (auto pair : {std::pair{0u, 0u}, {0u, 1u}, {3u, 0u}, {3u, 4u}}) {
            requireBm25Error<std::invalid_argument>(
                [&] { return compute_idf(pair.first, pair.second); }, "Invalid N/DF accepted.");
        }
        const auto maximum = std::numeric_limits<std::size_t>::max();
        const double ubiquitous = compute_idf(maximum, maximum);
        require(ubiquitous > 0 && std::isfinite(ubiquitous), "Large N lost positive IDF.");
        requireBm25Near(bm25_term_score(1, 3, 2, compute_idf(3, 1)), 0.8142733421,
                        "T5: e47 term score failed.");
        requireBm25Near(bm25_term_score(2, 3, 2, compute_idf(3, 2)), 0.5665797174,
                        "T5: repeated motor TF failed.");
        requireBm25Near(bm25_term_score(1, 2, 2, compute_idf(3, 2)), 0.4700036292,
                        "T5: unit normalization failed.");
        require(bm25_term_score(0, 0, 2, 1) == 0, "TF=0 must contribute zero.");
        requireBm25Near(bm25_term_score(2, 100, 2, 1, {1.2, 0}), 4.4 / 3.2,
                        "b=0 must ignore document length.");
        requireBm25Near(bm25_term_score(2, 4, 2, 1, {1.2, 1}), 4.4 / 4.4,
                        "b=1 must fully normalize length.");

        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (double invalid : {-1.0, 0.0, nan, inf, -inf}) {
            requireBm25Error<std::invalid_argument>([&] { Bm25Index bad({invalid, 0.75}); },
                                                    "Invalid k1 accepted.");
        }
        for (double invalid : {-0.1, 1.5, nan, inf, -inf}) {
            requireBm25Error<std::invalid_argument>([&] { Bm25Index bad({1.2, invalid}); },
                                                    "Invalid b accepted.");
        }
        for (double invalid : {0.0, -1.0, nan, inf}) {
            requireBm25Error<std::invalid_argument>(
                [&] { return bm25_term_score(1, 3, invalid, 1); }, "Invalid avgdl accepted.");
            requireBm25Error<std::invalid_argument>(
                [&] { return bm25_term_score(1, 3, 2, invalid); }, "Invalid IDF accepted.");
        }
        requireBm25Error<std::invalid_argument>([] { return bm25_term_score(3, 2, 2, 1); },
                                                "TF greater than document length accepted.");
        const double huge = std::numeric_limits<double>::max();
        const auto max_tf = std::numeric_limits<std::uint32_t>::max();
        requireBm25Near(bm25_term_score(max_tf, max_tf, max_tf, 1, {huge, 0.75}), max_tf,
                        "Finite huge k1 produced an avoidable overflow.");
        requireBm25Near(bm25_term_score(2, 3, 2, 1, {std::numeric_limits<double>::denorm_min(), 0}),
                        1, "Tiny positive k1 must be accepted.");
        requireBm25Error<std::overflow_error>(
            [&] { return bm25_term_score(2, 2, 2, huge, {huge, 0}); },
            "Unrepresentable term score was not rejected.");
        requireBm25Error<std::overflow_error>(
            [] { return bm25_term_score(1, 2, std::numeric_limits<double>::denorm_min(), 1); },
            "Length normalization overflow was not rejected.");

        Bm25Index index;
        addBm25Reference(index);
        index.build();
        const auto e47 = index.search("e47", 10);
        require(e47.size() == 1 && e47[0].document_id == 101, "T5: unexpected e47 candidates.");
        requireBm25Near(e47[0].score, 0.8142733421, "T5: public search score failed.");
        const auto motor = index.search("motor", 10);
        require(motor.size() == 2, "T5: nonmatching document present.");
        requireBm25Near(motor[0].score, 0.5665797174, "T5: public repeated-TF score failed.");
        requireBm25Near(motor[1].score, 0.4700036292, "T5: public single-TF score failed.");
        std::cout
            << "T4/T5/T10: independent numerical references, configuration and limits passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
