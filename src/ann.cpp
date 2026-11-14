// ann.cpp - implementation of the IVF-Flat index.
#include "ann.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace mvdb {
namespace {

// Below this number of rows an exhaustive scan over the in memory vectors is
// used: building a k-means index for a smaller set of vectors only wastes
// time and can only lose recall.  The vectors stay in RAM, the rows
// themselves are still read from the table file.
constexpr size_t kExhaustiveLimit = 65536;
constexpr size_t kMaxCentroids = 256;
constexpr int kKMeansIterations = 25;
// Minimum number of clusters that are probed before the distance based
// stopping rule may end the search.
constexpr size_t kMinProbes = 8;
// Work budget (distance computations) for the k-means training phase.
constexpr double kTrainingBudget = 5.0e8;

double l2(const std::vector<float>& a, const std::vector<float>& b) {
  double sum = 0.0;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

}  // namespace

void AnnIndex::build(std::vector<AnnPoint> points) {
  points_ = std::move(points);
  built_ = true;
  dim_ = points_.empty() ? 0 : static_cast<int>(points_.front().vec.size());
  centroids_.clear();
  postings_.clear();
  if (points_.empty()) {
    exhaustive_ = true;
    return;
  }
  const size_t n = points_.size();
  if (n < kExhaustiveLimit) {
    exhaustive_ = true;
    return;
  }
  exhaustive_ = false;
  size_t k = static_cast<size_t>(std::sqrt(static_cast<double>(n)));
  if (k < 1) k = 1;
  if (k > kMaxCentroids) k = kMaxCentroids;
  if (k > n) k = n;

  // The k-means training runs on a deterministic sample of the data, the
  // assignment step then puts every row into its nearest cluster.  This keeps
  // the build time of a large table bounded.
  const size_t trainCap = 20000;
  std::vector<size_t> train;
  train.reserve(std::min(n, trainCap));
  if (n <= trainCap) {
    for (size_t i = 0; i < n; ++i) train.push_back(i);
  } else {
    const size_t stride = n / trainCap;
    for (size_t i = 0; i < trainCap; ++i) train.push_back(i * stride);
  }

  // Deterministic k-means++ seeding: start with the point that is farthest
  // away from the centroid of all points and then always add the point that
  // is farthest away from the centroids chosen so far.  No randomness is
  // involved, so the index is reproducible.
  centroids_.resize(k);
  {
    std::vector<float> mean(dim_, 0.0f);
    for (size_t idx : train) {
      for (int d = 0; d < dim_; ++d) mean[d] += points_[idx].vec[d];
    }
    for (int d = 0; d < dim_; ++d) mean[d] /= static_cast<double>(train.size());

    size_t first = train[0];
    double farthest = -1.0;
    for (size_t idx : train) {
      const double d = l2(points_[idx].vec, mean);
      if (d > farthest) {
        farthest = d;
        first = idx;
      }
    }
    centroids_[0] = points_[first].vec;

    std::vector<double> minDist(train.size(), std::numeric_limits<double>::max());
    for (size_t c = 1; c < k; ++c) {
      for (size_t i = 0; i < train.size(); ++i) {
        const double d = l2(points_[train[i]].vec, centroids_[c - 1]);
        if (d < minDist[i]) minDist[i] = d;
      }
      size_t pick = 0;
      double far = -1.0;
      for (size_t i = 0; i < train.size(); ++i) {
        if (minDist[i] > far) {
          far = minDist[i];
          pick = i;
        }
      }
      centroids_[c] = points_[train[pick]].vec;
    }
  }

  std::vector<size_t> assign(train.size(), 0);
  std::vector<std::vector<double>> sums(k, std::vector<double>(dim_, 0.0));
  std::vector<size_t> counts(k, 0);
  // The number of Lloyd iterations is derived from a fixed work budget so
  // that a huge table does not need minutes for the index build.
  const double workPerIteration = static_cast<double>(train.size()) *
                                  static_cast<double>(k) *
                                  static_cast<double>(dim_ > 0 ? dim_ : 1);
  int iterations = static_cast<int>(kTrainingBudget / (workPerIteration > 1.0 ? workPerIteration : 1.0));
  if (iterations < 3) iterations = 3;
  if (iterations > kKMeansIterations) iterations = kKMeansIterations;
  for (int iter = 0; iter < iterations; ++iter) {
    bool changed = false;
    for (size_t c = 0; c < k; ++c) {
      std::fill(sums[c].begin(), sums[c].end(), 0.0);
      counts[c] = 0;
    }
    for (size_t i = 0; i < train.size(); ++i) {
      size_t best = 0;
      double bestDist = std::numeric_limits<double>::max();
      for (size_t c = 0; c < k; ++c) {
        const double d = l2(points_[train[i]].vec, centroids_[c]);
        if (d < bestDist) {
          bestDist = d;
          best = c;
        }
      }
      if (assign[i] != best) {
        assign[i] = best;
        changed = true;
      }
      for (int d = 0; d < dim_; ++d) sums[best][d] += points_[train[i]].vec[d];
      ++counts[best];
    }
    for (size_t c = 0; c < k; ++c) {
      if (counts[c] == 0) continue;
      for (int d = 0; d < dim_; ++d) centroids_[c][d] = static_cast<float>(sums[c][d] / counts[c]);
    }
    if (!changed && iter > 0) break;
  }

  // Assign every row to its nearest centroid and build the postings lists.
  postings_.assign(k, {});
  for (size_t i = 0; i < n; ++i) {
    size_t best = 0;
    double bestDist = std::numeric_limits<double>::max();
    for (size_t c = 0; c < k; ++c) {
      const double d = l2(points_[i].vec, centroids_[c]);
      if (d < bestDist) {
        bestDist = d;
        best = c;
      }
    }
    postings_[best].push_back(static_cast<uint32_t>(i));
  }

  // The radius of a cluster is the distance of its farthest member from the
  // centroid.  It gives a lower bound for the distance of the nearest member
  // of the cluster (distance to centroid - radius) which makes the stopping
  // rule of the search much tighter.
  radii_.assign(k, 0.0);
  for (size_t c = 0; c < k; ++c) {
    double radius = 0.0;
    for (uint32_t idx : postings_[c]) {
      radius = std::max(radius, l2(points_[idx].vec, centroids_[c]));
    }
    radii_[c] = radius;
  }
}

std::vector<int64_t> AnnIndex::search(const std::vector<float>& query, size_t k) const {
  std::vector<int64_t> result;
  if (!built_ || points_.empty() || k == 0) return result;
  const size_t n = points_.size();
  if (exhaustive_) {
    std::vector<std::pair<double, int64_t>> scored;
    scored.reserve(n);
    for (const auto& p : points_) scored.emplace_back(l2(query, p.vec), p.pk);
    const size_t take = std::min(k, scored.size());
    std::partial_sort(scored.begin(), scored.begin() + take, scored.end(),
                      [](const std::pair<double, int64_t>& a, const std::pair<double, int64_t>& b) {
                        if (a.first != b.first) return a.first < b.first;
                        return a.second < b.second;
                      });
    for (size_t i = 0; i < take; ++i) result.push_back(scored[i].second);
    return result;
  }

  // IVF search: probe the closest clusters first.  A cluster is ordered by
  // the lower bound of the distance of its nearest member
  // (distance to centroid - cluster radius), which is a tight bound.
  std::vector<std::pair<double, size_t>> clusterOrder;
  clusterOrder.reserve(centroids_.size());
  for (size_t c = 0; c < centroids_.size(); ++c) {
    const double lower = std::max(0.0, l2(query, centroids_[c]) - (c < radii_.size() ? radii_[c] : 0.0));
    clusterOrder.emplace_back(lower, c);
  }
  std::sort(clusterOrder.begin(), clusterOrder.end());

  std::vector<std::pair<double, int64_t>> best;  // max-heap by distance
  auto worse = [](const std::pair<double, int64_t>& a, const std::pair<double, int64_t>& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second > b.second;
  };
  for (size_t probed = 0; probed < clusterOrder.size(); ++probed) {
    const auto& entry = clusterOrder[probed];
    // Stop as soon as the next cluster cannot contain a better hit than the
    // worst one found so far - but never before kMinProbes clusters have been
    // inspected, because the radius of a cluster is only an estimate.
    if (probed >= kMinProbes && best.size() >= k && entry.first > best.front().first) break;
    const std::vector<uint32_t>& posting = postings_[entry.second];
    for (uint32_t idx : posting) {
      const AnnPoint& p = points_[idx];
      const double d = l2(query, p.vec);
      if (best.size() < k) {
        best.push_back({d, p.pk});
        std::push_heap(best.begin(), best.end(), worse);
      } else if (d < best.front().first || (d == best.front().first && p.pk < best.front().second)) {
        std::pop_heap(best.begin(), best.end(), worse);
        best.back() = {d, p.pk};
        std::push_heap(best.begin(), best.end(), worse);
      }
    }
  }
  std::sort_heap(best.begin(), best.end(),
                 [](const std::pair<double, int64_t>& a, const std::pair<double, int64_t>& b) {
                   if (a.first != b.first) return a.first < b.first;
                   return a.second < b.second;
                 });
  result.reserve(best.size());
  for (const auto& e : best) result.push_back(e.second);
  return result;
}

}  // namespace mvdb
