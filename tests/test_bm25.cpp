// test_bm25: lock the score formula against hand-computed reference values
// and exercise the numerical edge cases (df=0, df=N, freq=0, large numbers).

#include <cmath>

#include "bm25.hpp"
#include "test_helpers.hpp"

namespace {

void test_lucene_reference_value() {
    // Reference: Lucene 9.10 BM25Similarity, k1=1.2, b=0.75.
    //   N=10, df=2, freq=3, |d|=5, avgdl=10
    //   idf = ln((10 - 2 + 0.5) / (2 + 0.5) + 1) = ln(4.4) ≈ 1.481604541
    //   tf  = 3 * (1.2 + 1) / (3 + 1.2 * (1 - 0.75 + 0.75 * 5/10))
    //       = 6.6 / 3.75 = 1.76
    //   score ≈ 1.481604541 * 1.76 ≈ 2.607623
    const double expected = std::log(4.4) * (6.6 / 3.75);
    const double got = idx::bm25::score(10, 2, 3, 5, 10.0);
    IDX_CHECK_NEAR(got, expected, 1e-9);
}

void test_idf_smoothing_keeps_value_positive() {
    // df = N triggers smoothing: ln(0.5 / (N+0.5) + 1). +1 keeps the value
    // strictly positive instead of negative as in unsmoothed BM25.
    const double idf_full = idx::bm25::idf(/*N=*/100, /*df=*/100);
    IDX_CHECK(idf_full > 0.0);

    // Common terms produce small but non-negative IDF.
    const double idf_common = idx::bm25::idf(/*N=*/10000, /*df=*/9000);
    IDX_CHECK(idf_common > 0.0);
}

void test_idf_rare_term_is_largest() {
    const double rare = idx::bm25::idf(/*N=*/1'000'000, /*df=*/1);
    const double common = idx::bm25::idf(/*N=*/1'000'000, /*df=*/100'000);
    IDX_CHECK(rare > common);
}

void test_tf_zero_freq_yields_zero() {
    // Zero frequency must produce zero TF contribution regardless of |d|.
    IDX_CHECK_EQ(idx::bm25::tf_norm(/*freq=*/0, /*doc_length=*/100, /*avgdl=*/50.0), 0.0);
}

void test_tf_saturation_with_k1() {
    // As freq grows, tf_norm should approach k1 + 1 (the asymptote when
    // |d| == avgdl, since denom ~ freq + k1).
    const double k1 = 1.2;
    const double tf_high = idx::bm25::tf_norm(/*freq=*/10000, /*doc_length=*/50,
                                              /*avgdl=*/50.0, k1, 0.75);
    IDX_CHECK_NEAR(tf_high, k1 + 1.0, 1e-2);
}

void test_b_zero_disables_length_norm() {
    // With b=0, document length must not influence the score.
    const double s_short = idx::bm25::score(1000, 50, 5, /*|d|=*/10, /*avgdl=*/100.0,
                                            /*k1=*/1.2, /*b=*/0.0);
    const double s_long = idx::bm25::score(1000, 50, 5, /*|d|=*/1000, /*avgdl=*/100.0,
                                            /*k1=*/1.2, /*b=*/0.0);
    IDX_CHECK_NEAR(s_short, s_long, 1e-12);
}

void test_b_one_emphasises_length_penalty() {
    // With b=1, a long document should be penalised more than a short one.
    const double s_short = idx::bm25::score(1000, 50, 5, /*|d|=*/10, /*avgdl=*/100.0,
                                            /*k1=*/1.2, /*b=*/1.0);
    const double s_long = idx::bm25::score(1000, 50, 5, /*|d|=*/1000, /*avgdl=*/100.0,
                                            /*k1=*/1.2, /*b=*/1.0);
    IDX_CHECK(s_short > s_long);
}

}  // namespace

int main() {
    test_lucene_reference_value();
    test_idf_smoothing_keeps_value_positive();
    test_idf_rare_term_is_largest();
    test_tf_zero_freq_yields_zero();
    test_tf_saturation_with_k1();
    test_b_zero_disables_length_norm();
    test_b_one_emphasises_length_penalty();
    return idx::testing::report("test_bm25");
}
