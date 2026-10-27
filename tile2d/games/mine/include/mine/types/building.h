// Mine - a plot that is a **building**: what it costs, how long it takes, what it is for, and where it
// may stand (docs/GAME_DESIGN.md section 1.34).
//
// A building is a **sub-object** of a plot, the way a container is (types/container.h): a content entry
// declares one under `building::` and the plot it describes carries it. The three things the designer
// asked for are here, and nothing else is:
//
//     building::
//         materials:["stone",10,"chunk",2]   # what it costs to build: item name, count, name, count
//         time:5                             # how long it takes to build, in seconds
//         category:"extraction"              # what it is for: storage, processing, extraction
//
// **Where it may stand is a query, not a flag**: can_place() is asked before anything is placed, so the
// answer can depend on what is already on the map (a drill has to sit on ore). The layer is the one that
// knows the map, so the query lives there (world.h) and this header only says what a building is.
#pragma once

#include <mine/types/content_stack.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine::types {

/// What a building is for. The list grows with the designer's buildings; a name the engine does not know
/// is refused while the content file is read, never guessed at.
enum class BuildingCategory : t2d::u8 { None, Storage, Processing, Extraction };

[[nodiscard]] const char* building_category_name(BuildingCategory category);
/// The category a name stands for, or nullopt when it is not one of them.
[[nodiscard]] std::optional<BuildingCategory> building_category_from_name(std::string_view name);

/// One building: its price, its build time, and what it is for.
struct Building {
    /// What it costs, as the data spells it: an item **name** and a count. Resolved against the registry
    /// when someone asks (a mod can add an item long after the content file was written).
    std::vector<ContentStack> materials;
    /// How long it takes to build, in seconds. Zero is a building that goes up at once.
    t2d::f32 seconds = 0.0f;
    BuildingCategory category = BuildingCategory::None;
    /// Whether a **new save** starts with this building unlocked (docs/GAME_DESIGN.md section 1.44). It is
    /// a **definition** attribute - what a save has unlocked is that save's own state
    /// (mine/building_unlocks.h), and this is only the value a new one starts from. Data that does not say
    /// is unlocked, so content written before the field existed stays visible.
    bool default_unlock = true;

    [[nodiscard]] bool empty() const {
        return materials.empty() && seconds <= 0.0f && category == BuildingCategory::None;
    }
};

} // namespace mine::types
