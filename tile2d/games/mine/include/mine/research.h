// Mine - the technology tree: the nodes the content declares, their parents, and where they sit
// (docs/GAME_DESIGN.md section 1.50).
//
// A node is not content of its own: it is an entry that carries research:: (types/research.h), and the
// tree is built from the definitions the content load read. The tree is a **model**: parent checking,
// cycles, depth, the layout the screen draws (column = depth), which nodes are visible, and what has to
// be researched first. Nothing here knows about pixels, a wallet or a save.
#pragma once

#include <mine/research_state.h>
#include <mine/types/research.h>

#include <t2d/core/math2d.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

using t2d::i32;
using t2d::usize;

/// Layout units, not pixels: the screen multiplies them by its own node size. The tree grows **up**: a
/// node of depth d and same-level place r sits at (r * kResearchColumnStep, d * kResearchRowStep) from the
/// tree's origin - depth is the vertical step, the place the horizontal one - and the screen draws the
/// root at the graph bottom centre, a deeper level higher, centring each level's visible nodes around the
/// graph's middle. Lines run from a parent's top edge to a child's bottom.
inline constexpr i32 kResearchColumnStep = 4;
inline constexpr i32 kResearchRowStep = 3;

/// The layer a node asks for is the number the interface shows (layer 1 is the first one the player sees),
/// while MineLayer::index() is zero-based. The story's opening layer is therefore index 0 and satisfies
/// min_layer 1.
[[nodiscard]] constexpr bool layer_reaches(i32 min_layer, i32 layer_index) {
    return layer_index + 1 >= min_layer;
}

/// One research node as the content data declares it, with where the tree put it.
struct ResearchNode {
    std::string name;              ///< the content name the data refers to it by
    ContentKind kind = ContentKind::Structure;
    ContentId id = kNoContent;
    std::string tree;              ///< the tree ("intro") it is listed under
    std::vector<ContentStack> cost; ///< what researching it takes
    std::vector<usize> parents;    ///< indices into ResearchTree::node(), in the order the data wrote them
    bool is_root = false;
    i32 min_layer = 0;
    i32 depth = 0;                 ///< roots 0, a child one past its deepest parent (the vertical step)
    i32 column = 0;                ///< equal to depth; kept so a reader can see both numberings
    i32 row = 0;                   ///< place among the nodes of the same depth (the horizontal step)
};

/// One line of the tree: the parent that has to come first, and the child it opens.
struct ResearchEdge {
    usize parent = 0;
    usize child = 0;
};

class ResearchTree {
public:
    /// Reads every definition that declares research:: and checks the graph: a node name is unique, every
    /// parent exists, a root has no parent, anything that is not a root has at least one, and there is no
    /// cycle. What cannot be checked is reported in \p errors and the tree is still built from what is
    /// valid, so a screen can show the data errors instead of nothing.
    [[nodiscard]] static ResearchTree build(const ContentRegistry& registry,
                                            const ContentDefinitions& definitions,
                                            std::vector<std::string>* errors = nullptr);

    [[nodiscard]] usize node_count() const { return nodes_.size(); }
    [[nodiscard]] const ResearchNode& node(usize index) const { return nodes_[index]; }
    [[nodiscard]] usize edge_count() const { return edges_.size(); }
    [[nodiscard]] const ResearchEdge& edge(usize index) const { return edges_[index]; }
    /// The tree names in the order the nodes appeared: "intro" today, a category list tomorrow.
    [[nodiscard]] const std::vector<std::string>& trees() const { return trees_; }
    /// The node named \p name, or nothing.
    [[nodiscard]] std::optional<usize> find(std::string_view name) const;

    /// The node's position in layout units: horizontal from its place among the nodes of its depth,
    /// vertical from the depth. The screen centres a level's nodes around its graph, so this is the
    /// uncentred place the drawing pass builds on.
    [[nodiscard]] t2d::Vec2 position(usize index) const;
    /// A root, a node this mine has researched, or a node one of whose parents is researched. A node
    /// behind an unresearched parent is not shown at all.
    [[nodiscard]] bool visible(usize index, const ResearchState& state) const;
    /// Every parent researched. A root asks for nothing.
    [[nodiscard]] bool prereqs_met(usize index, const ResearchState& state) const;

private:
    std::vector<ResearchNode> nodes_;
    std::vector<ResearchEdge> edges_;
    std::vector<std::string> trees_;
};

} // namespace mine
