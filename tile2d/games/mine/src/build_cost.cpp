#include <mine/build_cost.h>

#include <format>

namespace mine {

namespace {

/// What a price is paid in: the **item** of that name and any **drop** of that name.
///
/// A drop is what an ore throws out and an item is what a machine makes, a recipe moves and a demand asks
/// for (docs/GAME_DESIGN.md sections 1.23, 1.25) - and a stone lying on the ground and a stone in a box are
/// the same stone. A player who dragged a drill's output into the warehouse **has** stone, so a price that
/// only counted items would tell them they have none: which is exactly the report "the warehouse clearly has
/// stone in it and it still says stone 0/10". Both kinds are counted, and both are spent.
constexpr ContentKind kPays[] = {ContentKind::Item, ContentKind::Drop};

/// How much of \p name the stock holds, over both kinds that count as paying for it.
[[nodiscard]] usize held(const types::Container& stock, const ContentRegistry& registry,
                         std::string_view name) {
    usize have = 0;
    for (const ContentKind kind : kPays) {
        const ContentId id = registry.find(kind, name);
        if (id != kNoContent) have += stock.count_of(kind, id);
    }
    return have;
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
        usize left = static_cast<usize>(stack.count);
        for (const ContentKind kind : kPays) {
            const ContentId id = registry.find(kind, stack.name);
            if (id == kNoContent || left == 0) continue;
            // Items first, then drops: what a machine made goes into the machine before what was picked up
            // off the floor. can_afford() said the two together are enough, so this cannot come up short.
            left -= stock.take(kind, id, left);
        }
    }
}

} // namespace mine
