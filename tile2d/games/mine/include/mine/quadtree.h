// Mine - the quadtree a layer indexes what is not on a cell with.
//
// A plot is found through the **chunk index** (world.h) because a plot *is* cells: a chunk answers "what
// covers this cell" in four bytes a cell. An **entity** (types/entity.h) is not on a cell - it has a
// position, and it moves - so a chunk cannot hold it, and walking every entity to answer "what is near
// this point" would cost what the layer holds rather than what the question covers. This is that
// index: a quadtree over the layer's rectangle, subdivided while a node holds more than a handful of
// things (docs/GAME_DESIGN.md section 1.24).
//
// It is a container of **points with a payload** and knows nothing about what it holds: the payload is
// whatever the caller wants back (world.h puts a types::Entity* in it), and the key is the caller's -
// the same key is what remove() takes, and the tree never invents one. Nothing here is content.
//
// Bounds: the root is the rectangle it is built with, and something outside it is **kept in the root**
// rather than refused - it is still found by a query that covers where it actually is, and a query that
// does not cover it is not supposed to find it. The root's own entries are therefore tested by every
// query (there are few of them) while the rest of the tree is pruned by rectangle. What the game does
// about something that walked off the map is the game's business, not the index's.
//
// A query visits the nodes its area touches and tests the entries in them, so its cost is the answer
// plus the depth of the tree rather than the size of the layer - and last_query_cost() reports what it
// actually walked, which is what makes "the index is why this is cheap" a number instead of a claim.
#pragma once

#include <t2d/core/math2d.h>
#include <t2d/core/types.h>

#include <array>
#include <vector>

namespace mine {

using t2d::f32;
using t2d::i32;
using t2d::u32;
using t2d::usize;

/// A quadtree of points, each carrying a payload the caller chooses. One layer holds one of these.
template <typename Payload>
class QuadTree {
public:
    /// One thing in the tree: the caller's key, where it is, and what the caller wants back.
    struct Entry {
        u32 key = 0;
        t2d::Vec2 position{};
        Payload payload{};
    };

    /// What a query walked: the nodes it visited and the entries it tested. A measure, not state the
    /// tree needs - a query does not change what the tree is, which is why the counters are mutable.
    struct Cost {
        usize nodes = 0;
        usize entries = 0;
    };

    /// A tree over the rectangle it is given, which subdivides a node holding more than the node
    /// capacity, down to the depth limit. The defaults are the ones a mine's layers are built with:
    /// **sixteen** things in a node before it is worth splitting (a node is a linear scan, so it has to
    /// stay small), and eight levels (a 100x100 layer down to cells smaller than a step).
    explicit QuadTree(t2d::Aabb2 bounds, usize node_capacity = 16, i32 max_depth = 8)
        : bounds_(bounds), capacity_(node_capacity < 1 ? 1 : node_capacity),
          max_depth_(max_depth < 0 ? 0 : max_depth) {
        nodes_.push_back(Node{bounds_, 0});
    }

    QuadTree(const QuadTree&) = delete;
    QuadTree& operator=(const QuadTree&) = delete;

    // --- filling it -------------------------------------------------------------------------------
    /// Adds an entry. Keys are the caller's business: the tree does not check that a key is new, and
    /// two entries with one key are two entries (remove() takes the first one it finds).
    void insert(u32 key, t2d::Vec2 position, Payload payload) {
        insert_into(0, Entry{key, position, std::move(payload)});
        ++size_;
    }

    /// Takes the entry with \p key at \p position out; returns whether it was there.
    bool remove(u32 key, t2d::Vec2 position) {
        if (!take(0, key, position, nullptr)) return false;
        --size_;
        return true;
    }

    /// Moves an entry: the same as remove() then insert(), with the payload kept. What an entity that
    /// moved is worth doing - the tree cannot be asked to follow a position it was not told about.
    bool move(u32 key, t2d::Vec2 from, t2d::Vec2 to) {
        Entry taken;
        if (!take(0, key, from, &taken)) return false;
        taken.position = to;
        insert_into(0, std::move(taken));
        return true;
    }

    void clear() {
        nodes_.clear();
        nodes_.push_back(Node{bounds_, 0});
        size_ = 0;
    }

    // --- asking it --------------------------------------------------------------------------------
    /// Visits every entry whose position is inside \p area. The order is the tree's, not the order
    /// things were inserted in: a caller that needs a stable order sorts what it collects (world.h
    /// orders a query by the entity's serial, which is spawn order).
    template <typename Visitor>
    void for_each_in(const t2d::Aabb2& area, Visitor&& visit) const {
        last_cost_ = Cost{};
        visit_area(0, area, visit);
    }

    /// Visits every entry within \p radius of \p center: the area query, then the exact distance. A
    /// negative radius finds what is exactly at \p center.
    template <typename Visitor>
    void for_each_within(t2d::Vec2 center, f32 radius, Visitor&& visit) const {
        const f32 reach = radius < 0.0f ? 0.0f : radius;
        const f32 limit = reach * reach;
        last_cost_ = Cost{};
        visit_area(0, t2d::Aabb2::from_center(center, t2d::Vec2{reach, reach}), [&](const Entry& entry) {
            if (t2d::length_squared(entry.position - center) <= limit) visit(entry);
        });
    }

    // --- what it holds ----------------------------------------------------------------------------
    [[nodiscard]] usize size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    /// How many nodes the tree has: one while nothing has been worth splitting.
    [[nodiscard]] usize node_count() const { return nodes_.size(); }
    [[nodiscard]] const t2d::Aabb2& bounds() const { return bounds_; }
    /// What the last query walked.
    [[nodiscard]] const Cost& last_query_cost() const { return last_cost_; }

private:
    struct Node {
        t2d::Aabb2 bounds{};
        i32 depth = 0;
        std::array<i32, 4> children{-1, -1, -1, -1};
        std::vector<Entry> entries;

        [[nodiscard]] bool leaf() const { return children[0] < 0; }
    };

    /// Which child of \p node holds \p position, or -1 when the node is a leaf or the position is
    /// outside it - a position off the map stays in the node it reached rather than being refused.
    [[nodiscard]] static i32 child_of(const Node& node, t2d::Vec2 position) {
        if (node.leaf() || !node.bounds.contains(position)) return -1;
        const t2d::Vec2 middle = node.bounds.center();
        const usize quadrant = (position.x >= middle.x ? 1u : 0u) + (position.y >= middle.y ? 2u : 0u);
        return node.children[quadrant];
    }

    /// True when the two rectangles touch at all - edges included, because a query on a node's edge has
    /// to find what sits on it. (Aabb2::overlaps() excludes touching edges on purpose: that is the
    /// question collision asks, not the question an index asks.)
    [[nodiscard]] static constexpr bool touches(const t2d::Aabb2& a, const t2d::Aabb2& b) {
        return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y;
    }

    void insert_into(i32 node_index, Entry entry) {
        Node& node = nodes_[node_index];
        const i32 child = child_of(node, entry.position);
        if (child < 0) {
            node.entries.push_back(std::move(entry));
            // A node that has grown past what it should hold is split, once, here: the entries that fit
            // a quadrant move into it, and the rest stay. At the deepest level a node keeps them all.
            if (node.leaf() && node.entries.size() > capacity_ && node.depth < max_depth_) {
                subdivide(node_index);
            }
            return;
        }
        insert_into(child, std::move(entry));
    }

    /// Splits a leaf into four and re-inserts what it held, so each entry ends up where it belongs -
    /// including into a child that is itself split, which is what keeps a node's load bounded.
    void subdivide(i32 node_index) {
        const t2d::Aabb2 bounds = nodes_[node_index].bounds;
        const i32 depth = nodes_[node_index].depth;
        const t2d::Vec2 middle = bounds.center();
        const std::array<t2d::Aabb2, 4> quadrants{{
            t2d::Aabb2{bounds.min, middle},
            t2d::Aabb2{t2d::Vec2{middle.x, bounds.min.y}, t2d::Vec2{bounds.max.x, middle.y}},
            t2d::Aabb2{t2d::Vec2{bounds.min.x, middle.y}, t2d::Vec2{middle.x, bounds.max.y}},
            t2d::Aabb2{middle, bounds.max},
        }};
        std::array<i32, 4> children{};
        for (usize quadrant = 0; quadrant < quadrants.size(); ++quadrant) {
            children[quadrant] = static_cast<i32>(nodes_.size());
            nodes_.push_back(Node{quadrants[quadrant], depth + 1});
        }
        std::vector<Entry> held = std::move(nodes_[node_index].entries);
        nodes_[node_index].entries.clear();
        nodes_[node_index].children = children;
        for (Entry& entry : held) insert_into(node_index, std::move(entry));
    }

    bool take(i32 node_index, u32 key, t2d::Vec2 position, Entry* out) {
        Node& node = nodes_[node_index];
        for (usize index = 0; index < node.entries.size(); ++index) {
            if (node.entries[index].key != key || !(node.entries[index].position == position)) continue;
            if (out != nullptr) *out = std::move(node.entries[index]);
            node.entries.erase(node.entries.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }
        const i32 child = child_of(node, position);
        if (child < 0) return false;
        return take(child, key, position, out);
    }

    template <typename Visitor>
    void visit_area(i32 node_index, const t2d::Aabb2& area, Visitor&& visit) const {
        const Node& node = nodes_[node_index];
        // The root is never pruned: what fell outside the map is kept in it (child_of() sends it
        // there), so a query that covers where it actually is still finds it. There are few of those,
        // so testing them on every query costs nothing.
        if (node_index != 0 && !touches(node.bounds, area)) return;
        ++last_cost_.nodes;
        for (const Entry& entry : node.entries) {
            ++last_cost_.entries;
            if (area.contains(entry.position)) visit(entry);
        }
        if (node.leaf()) return;
        for (const i32 child : node.children) visit_area(child, area, visit);
    }

    t2d::Aabb2 bounds_{};
    usize capacity_ = 16;
    i32 max_depth_ = 8;
    std::vector<Node> nodes_;
    usize size_ = 0;
    /// Written by every query, which is why it is mutable: asking the tree something does not change
    /// what it is.
    mutable Cost last_cost_{};
};

} // namespace mine
