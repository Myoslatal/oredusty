// Mine - the story's layers, described in data.
//
// The designer describes a mine's layers in the same .ecfg files every other piece of content comes
// from: one entry per layer under "layer::", and below it what the layer is made of.
//
//     layer::
//         entrance::                        # the layer's name; the registry knows it by this
//             size::                        # how big the layer is - every field is optional
//                 width:64                  # cells, 1..4096; absent: the world's shape
//                 height:48
//                 tile_layers:2             # 1..32; absent: the world's shape
//             floor::                       # the floor creator
//                 layer:0                   # which tile layer it lays the floor on (default 0)
//                 full_flash:"dirt"         # every cell of that tile layer becomes this floor
//
// **The order the data declares the layers in is the story's order**: layer 0 is the first entry, layer
// 1 the second, and so on. That is the same order the registry hands out Layer ids in (registry.h), so
// the nth story layer is also the content id n+1 - and a save, which stores ids plus the name table they
// were handed out with, keeps pointing at the same layer when the entries are reordered.
//
// The generator here is the story half of the interface world.h fixes: (seed, index) in, a layer
// description out. Story mode ignores the seed - the same mine for every world, which is what "pre-set"
// means - and reads the designer's data; endless mode derives the layer from the pair instead
// (docs/GAME_DESIGN.md section 4). Both produce the same LayerSpec, so nothing downstream knows which.
//
// A layer entry's own fields belong to the designer and are ignored here, the way every other content
// entry's fields are (types/tile_definition.h). The fields inside the tables the **engine** defines -
// size::, floor:: - are the engine's, so a key it does not know is refused with a message rather than
// ignored: "widht:40" is a layer that is not the size its author thinks it is.
#pragma once

#include <mine/registry.h>
#include <mine/world.h>

#include <t2d/core/ecfg.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mine {

/// One rule of a layer's floor creator: what it paints.
///
/// A rule paints cells, and **the last rule to paint a cell is what that cell ends up being**, so rules
/// compose the way a designer reads them: fill everything with rock, then stamp something into it. One
/// cell ends up with one floor plot, which is also what keeps two rules from overlapping each other into
/// a refusal.
struct FloorRule {
    /// What the rule does. The list grows with the designer's rules; a rule name the engine does not
    /// know is refused while the file is read, never ignored.
    enum class Kind : u8 { FullFlash };

    Kind kind = Kind::FullFlash;
    /// full_flash: the "floor::" entry every cell of the creator's tile layer becomes.
    std::string floor;
};

/// The floor creator of one layer: which tile layer it works on, and the rules it applies in file order.
struct FloorCreator {
    /// The tile layer the floor is laid on. 0 is the ground; a map with more tile layers can put its
    /// floor somewhere else. A tile layer this map does not have is refused when the layer is built
    /// (world.h says why), because only then is the map's size known.
    i32 layer = 0;
    /// The rules, in the order the file writes them. Empty when the layer has no floor:: table - which
    /// is a layer with no floor, not an error.
    std::vector<FloorRule> rules;
};

/// One layer the story describes.
struct LayerRule {
    /// The "layer::" entry's name: what the registry knows the layer by, and what a save stores.
    std::string name;
    /// The size of the layer's map. Zero means "the world's shape decides" - the session's or the
    /// command line's - which is the same fallback a generator that does not describe a shape gets.
    u32 width = 0;
    u32 height = 0;
    i32 tile_layers = 0;
    /// The floor creator. A layer whose data has no floor:: table has no floor - and that is a layer:
    /// an empty one is what a session enters before the rules exist (world.h).
    FloorCreator floor;
};

/// Every layer the story data describes, in the order it declares them.
class LayerRules {
public:
    /// Adds \p rule unless a layer of that name is already there: the first declaration keeps the name,
    /// the way the registry keeps the first owner of a content name. Keeping the two in step is what
    /// makes the nth story layer the content id n+1.
    void add(LayerRule rule);
    /// The layer at \p index in the story's order, or nullptr when the story is shorter than that: a
    /// mine can be walked past the end of what the designer wrote, and what is there is an empty layer.
    [[nodiscard]] const LayerRule* at(usize index) const;
    [[nodiscard]] const LayerRule* find(std::string_view name) const;
    [[nodiscard]] usize size() const { return rules_.size(); }
    [[nodiscard]] bool empty() const { return rules_.empty(); }
    /// Every layer, in the story's order.
    [[nodiscard]] ConstSpan<LayerRule> rules() const;

private:
    std::vector<LayerRule> rules_;
};

/// Reads one "layer::" entry. \p name is the entry's key. Returns nothing and fills \p error when a
/// field the engine reads is there but is not what it has to be - a size that is not a size is refused
/// rather than read as "no size", because the two are different layers.
[[nodiscard]] std::optional<LayerRule> read_layer_rule(std::string_view name, const t2d::EcfgValue& entry,
                                                       std::string* error = nullptr);

/// Every layer \p document describes, in file order. A "layer::" entry the engine cannot read is
/// reported in \p errors and skipped, so one bad layer does not hide the ones after it.
[[nodiscard]] std::vector<LayerRule> layer_rules(const t2d::EcfgDocument& document,
                                                 std::vector<std::string>* errors = nullptr);

/// The story mode generator: layer \p index is the \p index'th layer the data declares, and \p fallback
/// is the shape a layer that does not state one gets (the shape the world was given). A layer the story
/// does not describe is an empty layer of the fallback shape, so a caller can ask at() first and say so
/// rather than show a mine that is empty for a reason nobody can see.
///
/// The rules must outlive the generator and every world it is installed in, exactly like the registry a
/// debug generator reads (world.h).
[[nodiscard]] LayerGenerator story_layer_generator(const LayerRules& rules, LayerShape fallback);

} // namespace mine
