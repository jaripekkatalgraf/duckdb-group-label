#pragma once

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <limits>

namespace group_label {

// ─────────────────────────────────────────────────────────────────────────────
// Union-Find with path halving + union by rank
// ─────────────────────────────────────────────────────────────────────────────

class UnionFind {
public:
    void add(int64_t id) {
        if (idx_.count(id)) return;
        idx_[id] = (int32_t)nodes_.size();
        nodes_.push_back({(int32_t)nodes_.size(), 0});
    }
    void unite(int64_t a, int64_t b) {
        auto ia = idx_.find(a), ib = idx_.find(b);
        if (ia == idx_.end() || ib == idx_.end()) return;
        int32_t ra = find(ia->second), rb = find(ib->second);
        if (ra == rb) return;
        if (nodes_[ra].rank < nodes_[rb].rank) std::swap(ra, rb);
        nodes_[rb].parent = ra;
        if (nodes_[ra].rank == nodes_[rb].rank) nodes_[ra].rank++;
    }
    struct Result { int64_t node_id, component_id; int32_t component_size; };
    std::vector<Result> results() {
        std::unordered_map<int32_t,int32_t> sizes;
        for (auto &p : idx_) sizes[find(p.second)]++;
        std::vector<int64_t> rev(nodes_.size());
        for (auto &p : idx_) rev[p.second] = p.first;
        std::vector<Result> out; out.reserve(idx_.size());
        for (auto &p : idx_) { int32_t r = find(p.second); out.push_back({p.first, rev[r], sizes[r]}); }
        return out;
    }
private:
    struct Node { int32_t parent, rank; };
    std::vector<Node> nodes_;
    std::unordered_map<int64_t,int32_t> idx_;
    int32_t find(int32_t i) {
        while (nodes_[i].parent != i) {
            nodes_[i].parent = nodes_[nodes_[i].parent].parent;
            i = nodes_[i].parent;
        }
        return i;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Metric
// ─────────────────────────────────────────────────────────────────────────────

enum class Metric { L1, L2, Linf };
inline Metric parse_metric(const std::string &s) {
    if (s=="L2"||s=="l2") return Metric::L2;
    if (s=="Linf"||s=="linf"||s=="Lmax"||s=="lmax") return Metric::Linf;
    return Metric::L1;
}

// ─────────────────────────────────────────────────────────────────────────────
// PixelGroup — pixels stored as (y,x) sorted by (y,x) with precomputed
// bounding box. This layout enables binary search over y for the proximity
// check, reducing inner loop from O(na*nb) to O(na*|y_band|).
//
// grid_id: groups with different non-zero grid_ids are never connected.
// Enables mass labeling: pass all groups from all grids in one call.
// ─────────────────────────────────────────────────────────────────────────────

struct PixelGroup {
    int64_t  id      = 0;
    int64_t  grid_id = 0;   // 0 = no constraint
    std::vector<int32_t> ys, xs;   // sorted by (y,x)
    int32_t  min_x = INT32_MAX, max_x = INT32_MIN;
    int32_t  min_y = INT32_MAX, max_y = INT32_MIN;

    // Build from pixel_id array (pixel_id = y * grid_w + x)
    void build(const int32_t *pids, int32_t n, int32_t grid_w) {
        struct YX { int32_t y, x; };
        std::vector<YX> tmp(n);
        for (int32_t i = 0; i < n; ++i) {
            tmp[i].y = pids[i] / grid_w;
            tmp[i].x = pids[i] % grid_w;
        }
        std::sort(tmp.begin(), tmp.end(), [](const YX &a, const YX &b){
            return a.y != b.y ? a.y < b.y : a.x < b.x;
        });
        ys.resize(n); xs.resize(n);
        for (int32_t i = 0; i < n; ++i) {
            ys[i] = tmp[i].y; xs[i] = tmp[i].x;
            if (xs[i] < min_x) min_x = xs[i]; if (xs[i] > max_x) max_x = xs[i];
            if (ys[i] < min_y) min_y = ys[i]; if (ys[i] > max_y) max_y = ys[i];
        }
    }
    int32_t size() const { return (int32_t)ys.size(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// groups_within_distance — two-phase vectorized proximity check
//
// Phase 1: O(1) bounding-box rejection
// Phase 2: binary-search y-band in larger group, scan only that band
// Early exit: return true on first connecting pair
// ─────────────────────────────────────────────────────────────────────────────

inline bool groups_within_distance(
    const PixelGroup &a, const PixelGroup &b,
    float max_dist, Metric metric)
{
    // Grid isolation: never connect groups from different non-zero grids
    if (a.grid_id != b.grid_id && a.grid_id != 0 && b.grid_id != 0) return false;
    if (a.size() == 0 || b.size() == 0) return false;

    int32_t D = (int32_t)std::ceil(max_dist);

    // Phase 1: bounding-box pre-filter — O(1)
    if (a.min_x - b.max_x > D || b.min_x - a.max_x > D) return false;
    if (a.min_y - b.max_y > D || b.min_y - a.max_y > D) return false;

    // Phase 2: iterate smaller group, binary-search y-band in larger group
    const PixelGroup &sm = (a.size() <= b.size()) ? a : b;
    const PixelGroup &lg = (a.size() <= b.size()) ? b : a;

    for (int32_t i = 0; i < sm.size(); ++i) {
        int32_t y1 = sm.ys[i], x1 = sm.xs[i];

        // Binary search: y-band [y1-D, y1+D] in sorted lg.ys
        int32_t lo = (int32_t)(std::lower_bound(lg.ys.begin(), lg.ys.end(), y1-D) - lg.ys.begin());
        int32_t hi = (int32_t)(std::upper_bound(lg.ys.begin(), lg.ys.end(), y1+D) - lg.ys.begin());

        for (int32_t j = lo; j < hi; ++j) {
            int32_t dx = std::abs(x1 - lg.xs[j]);
            if (dx > D) continue;

            float dist;
            int32_t dy = std::abs(y1 - lg.ys[j]);
            switch (metric) {
                case Metric::L1:   dist = (float)(dx + dy); break;
                case Metric::L2:   dist = std::sqrt((float)(dx*dx + dy*dy)); break;
                case Metric::Linf: dist = (float)std::max(dx, dy); break;
                default:           dist = (float)(dx + dy);
            }
            if (dist <= max_dist + 1e-6f) return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// label_groups — mass spatial labeling
// All groups passed at once. grid_id isolates grids from each other.
// ─────────────────────────────────────────────────────────────────────────────

inline std::vector<UnionFind::Result> label_groups(
    const std::vector<PixelGroup> &groups,
    float max_dist, Metric metric)
{
    UnionFind uf;
    for (const auto &g : groups) uf.add(g.id);

    int32_t n = (int32_t)groups.size();
    for (int32_t i = 0; i < n; ++i)
        for (int32_t j = i+1; j < n; ++j)
            if (groups_within_distance(groups[i], groups[j], max_dist, metric))
                uf.unite(groups[i].id, groups[j].id);

    return uf.results();
}

// ─────────────────────────────────────────────────────────────────────────────
// connected_components — pure Union-Find over explicit edge list
// ─────────────────────────────────────────────────────────────────────────────

inline std::vector<UnionFind::Result> connected_components(
    const std::vector<int64_t> &node_ids,
    const std::vector<int64_t> &edge_a,
    const std::vector<int64_t> &edge_b)
{
    UnionFind uf;
    for (auto id : node_ids) uf.add(id);
    for (size_t i = 0; i < edge_a.size(); ++i) uf.unite(edge_a[i], edge_b[i]);
    return uf.results();
}

} // namespace group_label
