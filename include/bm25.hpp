#pragma once

// BM25 scoring helpers. Constants (k1, b) follow the MS MARCO baseline.
//
// BM25(q, d) = sum over t in q:
//     IDF(t) * (f(t,d) * (k1 + 1))
//             / (f(t,d) + k1 * (1 - b + b * |d| / avgdl))
//
// IDF(t) = ln((N - df(t) + 0.5) / (df(t) + 0.5) + 1)
//   - the +1 smoothing matches Lucene's BM25Similarity.

#include <cmath>

namespace idx::bm25 {

inline constexpr double kDefaultK1 = 1.2;
inline constexpr double kDefaultB = 0.75;

inline double idf(int total_docs, int df) {
    const double n = static_cast<double>(total_docs);
    const double d = static_cast<double>(df);
    return std::log((n - d + 0.5) / (d + 0.5) + 1.0);
}

inline double tf_norm(int freq, int doc_length, double avgdl,
                     double k1 = kDefaultK1, double b = kDefaultB) {
    const double f = static_cast<double>(freq);
    const double dl = static_cast<double>(doc_length);
    const double denom = f + k1 * (1.0 - b + b * (dl / avgdl));
    return (f * (k1 + 1.0)) / denom;
}

inline double score(int total_docs, int df, int freq, int doc_length, double avgdl,
                    double k1 = kDefaultK1, double b = kDefaultB) {
    return idf(total_docs, df) * tf_norm(freq, doc_length, avgdl, k1, b);
}

}  // namespace idx::bm25
