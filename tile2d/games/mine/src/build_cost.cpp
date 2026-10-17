#include <mine/build_cost.h>

#include <format>

namespace mine {

namespace {

/// How much of \p name the stock holds. It is an **item** and nothing else: the drop lying on the ground
/// is made out of the item its ore drops (types/drill.cpp), so a stone carried into a warehouse is the
/// stone item, and a price counts items (docs/GAME_DESIGN.md sections 1.23, 1.25).
[[nodiscard]] usize held(const types::Container& stock, const ContentRegistry& registry,
                         std::string_view name) {
    const ContentId id = registry.find(ContentKind::Item, name);
    return id == kNoContent ? 0 : stock.count_of(ContentKind::Item, id);
}

} // namespace

bool can_afford(const types::Container& stock, const ContentRegistry& registry,
                const std::vector<ContentStack>& materials, std::string* missing) {
    for (const ContentStack& stack : materials) {
        // A material is named, not numbered, in the data: the id is looked up where the question is asked, so
        // a mod that adds an item long after the content file was written is still counted (docs/MODS.md).
        const usize have = held(stock, registry, stack.name);
        if (have >= static_cast<usize>(stack.count)) continue;
        if (missing != nullptr) {
            *missing = std::format("{}: {} of {} (needs {})", stack.name, have, stack.count, stack.count);
        }
        return false;
    }
    return true;
}

void pay_for(types::Container& stock, const ContentRegistry& registry,
             const std::vector<ContentStack>& materials) {
    for (const ContentStack& stack : materials) {
        const ContentId id = registry.find(ContentKind::Item, stack.name);
        if (id == kNoContent) continue;   // can_afford() would have said no; paying nothing is the safe half
        (void)stock.take(ContentKind::Item, id, static_cast<usize>(stack.count));
    }
}

} // namespace mine
