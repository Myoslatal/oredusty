#include <mine/research.h>

#include <mine/types/tile_definition.h>

#include <algorithm>
#include <array>
#include <format>
#include <unordered_map>
#include <utility>

namespace mine {

namespace {

/// The kinds whose entries can be research nodes, in a fixed order: what the tree walks, and therefore
/// the order nodes appear in before the layout sorts them by depth. The same order as the save table.
constexpr std::array<ContentKind, 6> kResearchKinds{ContentKind::Structure, ContentKind::Machine,
                                                    ContentKind::Drill,     ContentKind::Warehouse,
                                                    ContentKind::Floor,     ContentKind::Ore};

} // namespace

ResearchTree ResearchTree::build(const ContentRegistry& registry, const ContentDefinitions& definitions,
                                 std::vector<std::string>* errors) {
    ResearchTree tree;
    const auto report = [errors](std::string message) {
        if (errors != nullptr) errors->push_back(std::move(message));
    };

    // The nodes, in the data's order. The parent names are kept aside: a parent may be declared after
    // its child, so names are resolved once every node is known.
    std::vector<std::vector<std::string>> declared_parents;
    std::unordered_map<std::string, usize> by_name;
    for (const ContentKind kind : kResearchKinds) {
        for (const ContentEntry& entry : registry.entries(kind)) {
            const types::TileDefinition* definition = definitions.find_tile(kind, entry.name);
            if (definition == nullptr || !definition->research.has_value()) continue;
            const types::Research& research = *definition->research;
            if (by_name.contains(entry.name)) {
                report(std::format("research node '{}' is declared more than once", entry.name));
                continue;
            }
            ResearchNode node;
            node.name = entry.name;
            node.kind = kind;
            node.id = entry.id;
            node.tree = research.tree;
            node.cost = research.cost;
            node.is_root = research.is_root;
            node.min_layer = research.min_layer;
            const usize index = tree.nodes_.size();
            tree.nodes_.push_back(std::move(node));
            declared_parents.push_back(research.parents);
            by_name.emplace(entry.name, index);
        }
    }

    // Every parent has to exist; one entry naming the same parent twice is one edge, not two.
    for (usize index = 0; index < tree.nodes_.size(); ++index) {
        ResearchNode& node = tree.nodes_[index];
        for (const std::string& parent : declared_parents[index]) {
            const auto found = by_name.find(parent);
            if (found == by_name.end()) {
                report(std::format("research node '{}' names parent '{}', which is not a research node",
                                   node.name, parent));
                continue;
            }
            if (std::find(node.parents.begin(), node.parents.end(), found->second) == node.parents.end()) {
                node.parents.push_back(found->second);
            }
        }
    }

    // A root is the tree's way in and has nothing before it; everything else is reached through at
    // least one parent.
    for (usize index = 0; index < tree.nodes_.size(); ++index) {
        const ResearchNode& node = tree.nodes_[index];
        if (node.is_root && !declared_parents[index].empty()) {
            report(std::format("research node '{}' is a root and also names parents", node.name));
        }
        if (!node.is_root && declared_parents[index].empty()) {
            report(std::format("research node '{}' is not a root and names no parent", node.name));
        }
    }

    // The edges are the parents' order: what the screen draws, and what depth walks.
    std::vector<std::vector<usize>> children(tree.nodes_.size());
    std::vector<usize> indegree(tree.nodes_.size(), 0);
    for (usize child = 0; child < tree.nodes_.size(); ++child) {
        for (const usize parent : tree.nodes_[child].parents) {
            tree.edges_.push_back(ResearchEdge{parent, child});
            children[parent].push_back(child);
            ++indegree[child];
        }
    }

    // Depth is the longest path from a root (a child is one past its deepest parent), and the order is
    // also what finds a cycle: a node that never reaches indegree zero cannot be reached from a root.
    std::vector<usize> queue;
    for (usize index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) queue.push_back(index);
    }
    for (usize head = 0; head < queue.size(); ++head) {
        const usize current = queue[head];
        const i32 next_depth = tree.nodes_[current].depth + 1;
        for (const usize child : children[current]) {
            tree.nodes_[child].depth = std::max(tree.nodes_[child].depth, next_depth);
            if (--indegree[child] == 0) queue.push_back(child);
        }
    }
    if (queue.size() < tree.nodes_.size()) {
        for (usize index = 0; index < tree.nodes_.size(); ++index) {
            if (indegree[index] == 0) continue;
            report(std::format("research tree has a cycle (through '{}')", tree.nodes_[index].name));
            break;
        }
    }

    // Column = depth; within a column the nodes keep the order the data declared them in. The screen
    // multiplies the result by its node size, so the layout is stable between builds of the same data.
    std::vector<i32> next_row;
    for (usize index = 0; index < tree.nodes_.size(); ++index) {
        ResearchNode& node = tree.nodes_[index];
        node.column = node.depth;
        const usize column = static_cast<usize>(std::max(node.column, 0));
        if (column >= next_row.size()) next_row.resize(column + 1, 0);
        node.row = next_row[column]++;
    }

    // The category list is the nodes' order, each tree name once.
    for (const ResearchNode& node : tree.nodes_) {
        if (std::find(tree.trees_.begin(), tree.trees_.end(), node.tree) == tree.trees_.end()) {
            tree.trees_.push_back(node.tree);
        }
    }
    return tree;
}

std::optional<usize> ResearchTree::find(std::string_view name) const {
    for (usize index = 0; index < nodes_.size(); ++index) {
        if (nodes_[index].name == name) return index;
    }
    return std::nullopt;
}

t2d::Vec2 ResearchTree::position(usize index) const {
    const ResearchNode& node = nodes_[index];
    // The tree grows up: depth is the vertical step (drawn higher on screen), the place among its
    // level the horizontal one.
    return t2d::Vec2{static_cast<t2d::f32>(node.row * kResearchColumnStep),
                     static_cast<t2d::f32>(node.depth * kResearchRowStep)};
}

bool ResearchTree::visible(usize index, const ResearchState& state) const {
    if (index >= nodes_.size()) return false;
    const ResearchNode& node = nodes_[index];
    if (node.is_root || state.researched(node.kind, node.id)) return true;
    for (const usize parent : node.parents) {
        if (state.researched(nodes_[parent].kind, nodes_[parent].id)) return true;
    }
    return false;
}

bool ResearchTree::prereqs_met(usize index, const ResearchState& state) const {
    if (index >= nodes_.size()) return false;
    for (const usize parent : nodes_[index].parents) {
        if (!state.researched(nodes_[parent].kind, nodes_[parent].id)) return false;
    }
    return true;
}

} // namespace mine
