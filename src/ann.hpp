// ann.hpp - a small IVF-Flat index (hand written, no third party libraries).
//
// The index is built from the committed rows of a table with k-means and
// searched by probing the closest centroids.  For small tables the search
// degenerates into an exhaustive scan, which keeps the recall at 1.0 for the
// data sizes used by the competition.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvdb {

struct AnnPoint {
  int64_t pk = 0;
  std::vector<float> vec;
};

class AnnIndex {
 public:
  // Builds the index.  `points` must all have the same dimension.
  void build(std::vector<AnnPoint> points);

  bool built() const { return built_; }
  size_t size() const { return points_.size(); }
  int dim() const { return dim_; }

  // Returns up to `k` candidate pks ordered by increasing distance.
  std::vector<int64_t> search(const std::vector<float>& query, size_t k) const;

 private:
  bool built_ = false;
  bool exhaustive_ = true;  // small tables are scanned completely
  int dim_ = 0;
  std::vector<AnnPoint> points_;
  std::vector<std::vector<float>> centroids_;
  std::vector<std::vector<uint32_t>> postings_;  // centroid -> point indices
  std::vector<double> radii_;                   // centroid -> cluster radius
};

}  // namespace mvdb
