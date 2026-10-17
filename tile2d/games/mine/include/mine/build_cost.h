// Mine - what a building costs and who pays for it (docs/GAME_DESIGN.md section 1.37).
//
// The price is the `building:: materials` of the entry, and it is paid out of the **selected warehouse**:
// the player builds with what is in the box they picked. Both halves are here rather than in the world view
// because both are rules, and rules are what tests hold on to.
#pragma once

#include <mine/registry.h>
#include <mine/types/container.h>
#include <mine/types/content_stack.h>

#include <string>
#include <vector>

namespace mine {

/// Whether \p stock holds everything \p materials asks for. When it does not, \p missing says which item
/// and how many are short - the line the player is shown rather than a refusal with no reason.
[[nodiscard]] bool can_afford(const types::Container& stock, const ContentRegistry& registry,
                              const std::vector<ContentStack>& materials, std::string* missing = nullptr);

/// Takes \p materials out of \p stock. Only what can_afford() said yes to, so a caller that asked first
/// cannot leave a half-paid building behind.
void pay_for(types::Container& stock, const ContentRegistry& registry,
             const std::vector<ContentStack>& materials);

} // namespace mine
