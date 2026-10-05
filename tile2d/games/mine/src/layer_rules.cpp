#include <mine/layer_rules.h>

#include <format>
#include <utility>

namespace mine {
namespace {

/// The value of a field the engine reads as a whole number, or nothing when it is not one. A field that
/// is there but is not a number is refused rather than read as zero (layer_rules.h).
[[nodiscard]] std::optional<t2d::i64> read_int(const t2d::EcfgValue& value) {
    if (value.type() != t2d::EcfgType::Int) return std::nullopt;
    return value.as_int();
}

/// Scatters one generator's content into \p spec: which cells are eligible, how many of them get one,
/// and which ones. \p painted is what the floor creator put on each cell (nullptr: no floor), \p taken is
/// what the description already holds (one byte per cell per tile layer), and \p rng is the layer's own
/// dice - so the same (seed, layer) scatters the same way twice, and a different seed moves the ore
/// without changing how much of it there is.
void scatter_into(LayerSpec& spec, const ScatterRule& scatter, const std::vector<const FloorRule*>& painted,
                  std::vector<u8>& taken, usize cells, i32 tile_layers, t2d::Rng& rng) {
    const auto problem = [&](std::string message) { spec.problems.push_back(std::move(message)); };
    if (scatter.layer >= tile_layers) {
        problem(std::format("layer {}: the scatter '{}' works on tile layer {}, and this layer has {}",
                            spec.index, scatter.name, scatter.layer, tile_layers));
        return;
    }

    const u32 width = spec.shape.width;
    std::vector<u32> eligible;
    for (u32 y = 0; y < spec.shape.height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const usize cell = static_cast<usize>(y) * width + x;
            if (taken[static_cast<usize>(scatter.layer) * cells + cell] != 0) continue;
            if (!scatter.floor.empty()) {
                const FloorRule* floor_rule = painted[cell];
                if (floor_rule == nullptr || floor_rule->floor != scatter.floor) continue;
            }
            eligible.push_back(static_cast<u32>(cell));
        }
    }

    // How many: every eligible cell is taken with probability density, so the count varies around
    // density x cells; min and max bound that, and a map that cannot hold that many wins over both.
    usize wanted = 0;
    for (usize index = 0; index < eligible.size(); ++index) {
        if (rng.chance(static_cast<f32>(scatter.density))) ++wanted;
    }
    if (wanted < scatter.min) wanted = scatter.min;
    if (scatter.max.has_value() && wanted > *scatter.max) wanted = *scatter.max;
    if (wanted > eligible.size()) wanted = eligible.size();
    if (wanted < scatter.min) {
        // The map could not hold what the rule asked for. The restriction wins - it is a rule and a
        // minimum is a wish - and what it cost is said out loud rather than swallowed (world.h).
        problem(std::format("layer {}: the scatter '{}' asked for at least {} and {} cell(s) were eligible",
                            spec.index, scatter.name, scatter.min, eligible.size()));
    }

    // Which ones: a partial shuffle of the eligible cells, so every set of n of them is equally likely
    // and no cell is picked twice.
    for (usize index = 0; index < wanted; ++index) {
        const usize other = index + rng.next_bounded(static_cast<u32>(eligible.size() - index));
        std::swap(eligible[index], eligible[other]);
    }
    for (usize index = 0; index < wanted; ++index) {
        const u32 cell = eligible[index];
        PlacedContent where;
        where.kind = scatter.kind;
        where.name = scatter.content;
        where.layer = scatter.layer;
        where.anchor = GridPos{static_cast<i32>(cell % width), static_cast<i32>(cell / width)};
        spec.content.push_back(std::move(where));
        taken[static_cast<usize>(scatter.layer) * cells + cell] = 1;
    }
}

} // namespace

void LayerRules::add(LayerRule rule) {
    // The same layer twice: the first declaration keeps the name, the way the registry keeps the first
    // owner of a content name (registry.h).
    if (find(rule.name) != nullptr) return;
    rules_.push_back(std::move(rule));
}

const LayerRule* LayerRules::at(usize index) const { return index < rules_.size() ? &rules_[index] : nullptr; }

const LayerRule* LayerRules::find(std::string_view name) const {
    for (const LayerRule& rule : rules_) {
        if (rule.name == name) return &rule;
    }
    return nullptr;
}

ConstSpan<LayerRule> LayerRules::rules() const {
    return ConstSpan<LayerRule>(rules_.data(), rules_.size());
}

std::optional<LayerRule> read_layer_rule(std::string_view name, const t2d::EcfgValue& entry, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<LayerRule> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    LayerRule rule;
    rule.name = std::string(name);
    if (!entry.is_table()) {
        return fail(std::format("layer '{}': a layer is a table of what it is made of", name));
    }

    if (const t2d::EcfgValue* size = entry.find("size"); size != nullptr) {
        if (!size->is_table()) {
            return fail(std::format("layer '{}': size must be a table (size:: width:64 height:48)", name));
        }
        for (const t2d::EcfgValue& field : size->children()) {
            if (field.key() == "width" || field.key() == "height") {
                const std::optional<t2d::i64> value = read_int(field);
                if (!value.has_value()) {
                    return fail(std::format("layer '{}': size.{} must be a whole number of cells", name,
                                            field.key()));
                }
                if (*value < 1 || *value > static_cast<t2d::i64>(ContentGrid::kMaxDimension)) {
                    return fail(std::format("layer '{}': size.{} is {}, and a map is 1..{} cells per side", name,
                                            field.key(), *value, ContentGrid::kMaxDimension));
                }
                if (field.key() == "width") rule.width = static_cast<u32>(*value);
                else rule.height = static_cast<u32>(*value);
                continue;
            }
            if (field.key() == "tile_layers") {
                const std::optional<t2d::i64> value = read_int(field);
                if (!value.has_value()) {
                    return fail(std::format("layer '{}': size.tile_layers must be a whole number", name));
                }
                if (*value < 1 || *value > static_cast<t2d::i64>(ContentGrid::kMaxLayers)) {
                    return fail(std::format("layer '{}': size.tile_layers is {}, and a map has 1..{} of them",
                                            name, *value, ContentGrid::kMaxLayers));
                }
                rule.tile_layers = static_cast<i32>(*value);
                continue;
            }
            return fail(std::format("layer '{}': size has no field '{}' (width, height, tile_layers)", name,
                                    field.key()));
        }
    }

    if (const t2d::EcfgValue* floor = entry.find("floor"); floor != nullptr) {
        if (!floor->is_table()) {
            return fail(std::format("layer '{}': floor must be a table (floor:: full_flash:\"dirt\")", name));
        }
        for (const t2d::EcfgValue& field : floor->children()) {
            if (field.key() == "layer") {
                const std::optional<t2d::i64> value = read_int(field);
                if (!value.has_value()) {
                    return fail(std::format("layer '{}': floor.layer must be a whole number", name));
                }
                if (*value < 0 || *value >= static_cast<t2d::i64>(ContentGrid::kMaxLayers)) {
                    return fail(std::format("layer '{}': floor.layer is {}, and a map has 0..{} tile layers", name,
                                            *value, ContentGrid::kMaxLayers - 1));
                }
                rule.floor.layer = static_cast<i32>(*value);
                continue;
            }
            if (field.key() == "full_flash") {
                if (!field.is_string()) {
                    return fail(std::format(
                        "layer '{}': full_flash must be the quoted name of a floor (full_flash:\"dirt\")", name));
                }
                if (field.as_string().empty()) {
                    return fail(std::format("layer '{}': full_flash names no floor", name));
                }
                FloorRule painted;
                painted.kind = FloorRule::Kind::FullFlash;
                painted.floor = std::string(field.as_string());
                rule.floor.rules.push_back(std::move(painted));
                continue;
            }
            return fail(std::format("layer '{}': the floor creator has no rule '{}' (full_flash, layer)", name,
                                    field.key()));
        }
    }

    if (const t2d::EcfgValue* scatter = entry.find("scatter"); scatter != nullptr) {
        if (!scatter->is_table()) {
            return fail(std::format("layer '{}': scatter must be a table of scatter generators (scatter:: "
                                    "copper:: ...)",
                                    name));
        }
        for (const t2d::EcfgValue& generator : scatter->children()) {
            std::string scatter_error;
            std::optional<ScatterRule> parsed = read_scatter_rule(name, generator, &scatter_error);
            if (!parsed.has_value()) return fail(std::move(scatter_error));
            rule.scatter.push_back(std::move(*parsed));
        }
    }

    // The checks that need more than one table are made here, once everything is read, so the order the
    // file writes the tables in does not matter: a tile layer this map does not have, and a floor
    // restriction under a layer that lays no floor at all (which could never match anything).
    if (rule.tile_layers > 0) {
        if (rule.floor.layer >= rule.tile_layers) {
            return fail(std::format("layer '{}': the floor creator works on tile layer {}, and this layer has {}",
                                    name, rule.floor.layer, rule.tile_layers));
        }
        for (const ScatterRule& scatter : rule.scatter) {
            if (scatter.layer >= rule.tile_layers) {
                return fail(std::format("layer '{}': the scatter '{}' works on tile layer {}, and this layer has {}",
                                        name, scatter.name, scatter.layer, rule.tile_layers));
            }
        }
    }
    if (rule.floor.rules.empty()) {
        for (const ScatterRule& scatter : rule.scatter) {
            if (scatter.floor.empty()) continue;
            return fail(std::format("layer '{}': the scatter '{}' only takes cells whose floor is '{}', and this "
                                    "layer has no floor creator",
                                    name, scatter.name, scatter.floor));
        }
    }

    return rule;
}

std::optional<ScatterRule> read_scatter_rule(std::string_view layer_name, const t2d::EcfgValue& entry,
                                             std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ScatterRule> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    ScatterRule rule;
    rule.name = std::string(entry.key());
    const auto about = [&](std::string_view what) {
        return std::format("layer '{}': the scatter '{}' {}", layer_name, rule.name, what);
    };
    if (!entry.is_table()) {
        return fail(about("is a table of what it places (kind, content, layer, density, min, max, floor)"));
    }

    bool has_kind = false;
    bool has_content = false;
    bool has_density = false;
    for (const t2d::EcfgValue& field : entry.children()) {
        if (field.key() == "kind") {
            if (!field.is_string()) return fail(about("takes a quoted content kind (kind:\"ore\")"));
            const ContentKind kind = content_kind_from_name(field.as_string());
            if (kind == ContentKind::Count) {
                return fail(about(std::format("places '{}', which is not a content kind", field.as_string())));
            }
            if (!types::kind_is_a_tile(kind)) {
                return fail(about(std::format("places '{}' ({}), and only floors, ores, structures and machines "
                                              "occupy cells",
                                              field.as_string(), content_kind_name(kind))));
            }
            rule.kind = kind;
            has_kind = true;
            continue;
        }
        if (field.key() == "content") {
            if (!field.is_string()) return fail(about("takes a quoted content name (content:\"copper\")"));
            if (field.as_string().empty()) return fail(about("names no content"));
            rule.content = std::string(field.as_string());
            has_content = true;
            continue;
        }
        if (field.key() == "layer") {
            const std::optional<t2d::i64> value = read_int(field);
            if (!value.has_value()) return fail(about("takes a whole number for layer"));
            if (*value < 0 || *value >= static_cast<t2d::i64>(ContentGrid::kMaxLayers)) {
                return fail(about(std::format("works on tile layer {}, and a map has 0..{} tile layers", *value,
                                              ContentGrid::kMaxLayers - 1)));
            }
            rule.layer = static_cast<i32>(*value);
            continue;
        }
        if (field.key() == "density") {
            if (field.type() != t2d::EcfgType::Int && field.type() != t2d::EcfgType::Float) {
                return fail(about("takes a number for density"));
            }
            const t2d::f64 density = field.as_float();
            if (density < 0.0 || density > 1.0) {
                return fail(about(std::format("has a density of {}, and a density is the fraction of the eligible "
                                              "cells that gets one (0..1)",
                                              density)));
            }
            rule.density = density;
            has_density = true;
            continue;
        }
        if (field.key() == "min" || field.key() == "max") {
            const std::optional<t2d::i64> value = read_int(field);
            if (!value.has_value()) return fail(about(std::format("takes a whole number for {}", field.key())));
            if (*value < 0) return fail(about(std::format("has a {} of {}, and a count is not negative", field.key(),
                                                          *value)));
            if (field.key() == "min") rule.min = static_cast<u32>(*value);
            else rule.max = static_cast<u32>(*value);
            continue;
        }
        if (field.key() == "floor") {
            if (!field.is_string()) return fail(about("takes a quoted floor name (floor:\"dirt\")"));
            if (field.as_string().empty()) return fail(about("names no floor"));
            rule.floor = std::string(field.as_string());
            continue;
        }
        return fail(about(std::format("has no attribute '{}' (kind, content, layer, density, min, max, floor)",
                                      field.key())));
    }

    if (!has_content) return fail(about("does not say what it places (content:\"copper\")"));
    if (!has_kind) return fail(about("does not say which kind of content it places (kind:\"ore\")"));
    if (!has_density) return fail(about("does not say how dense it is (density:0.01)"));
    if (rule.max.has_value() && *rule.max < rule.min) {
        return fail(about(std::format("wants at least {} and at most {}", rule.min, *rule.max)));
    }
    return rule;
}

std::vector<LayerRule> layer_rules(const t2d::EcfgDocument& document, std::vector<std::string>* errors) {
    std::vector<LayerRule> rules;
    for (const t2d::EcfgValue& table : document.root().children()) {
        if (table.key() != content_kind_name(ContentKind::Layer)) continue;
        if (!table.is_table()) continue;
        for (const t2d::EcfgValue& entry : table.children()) {
            std::string error;
            std::optional<LayerRule> rule = read_layer_rule(entry.key(), entry, &error);
            if (!rule.has_value()) {
                if (errors != nullptr) errors->push_back(std::move(error));
                continue;
            }
            rules.push_back(std::move(*rule));
        }
    }
    return rules;
}

LayerGenerator story_layer_generator(const LayerRules& rules, LayerShape fallback) {
    return [&rules, fallback](u64 seed, i32 index) {
        LayerSpec spec;
        spec.index = index;
        const LayerRule* rule = index >= 0 ? rules.at(static_cast<usize>(index)) : nullptr;
        if (rule == nullptr) {
            // A layer the story does not describe: an empty one, of the shape the world was given.
            spec.shape = fallback;
            return spec;
        }
        spec.shape.width = rule->width != 0 ? rule->width : fallback.width;
        spec.shape.height = rule->height != 0 ? rule->height : fallback.height;
        spec.shape.tile_layers = rule->tile_layers > 0 ? rule->tile_layers : fallback.tile_layers;

        // The floor creator paints: every rule in file order, and the last rule to paint a cell is what
        // that cell ends up being (layer_rules.h). A layer with no floor rule, or no cells, is a layer
        // with nothing in it - which is a layer.
        const usize cells = static_cast<usize>(spec.shape.width) * static_cast<usize>(spec.shape.height);
        if (cells == 0) return spec;
        std::vector<const FloorRule*> painted(cells, nullptr);
        for (const FloorRule& floor_rule : rule->floor.rules) {
            switch (floor_rule.kind) {
                case FloorRule::Kind::FullFlash:
                    for (const FloorRule*& cell : painted) cell = &floor_rule;
                    break;
            }
        }
        // One byte per cell per tile layer: what this description already holds. The floor goes down
        // first and the scatters come after it, so nothing lands on a cell something else is on.
        const i32 tile_layers = spec.shape.tile_layers > 0 ? spec.shape.tile_layers : 1;
        std::vector<u8> taken(static_cast<usize>(tile_layers) * cells, 0);
        if (rule->floor.layer >= tile_layers) {
            // A tile layer this map does not have. Said once rather than once per cell, and nothing is
            // placed: a plot on a layer that is not there is refused by the build with the same words.
            spec.problems.push_back(std::format("layer {}: the floor creator works on tile layer {}, and this "
                                                "layer has {}",
                                                index, rule->floor.layer, tile_layers));
        } else {
            for (u32 y = 0; y < spec.shape.height; ++y) {
                for (u32 x = 0; x < spec.shape.width; ++x) {
                    const usize cell = static_cast<usize>(y) * spec.shape.width + x;
                    const FloorRule* floor_rule = painted[cell];
                    if (floor_rule == nullptr) continue;
                    PlacedContent where;
                    where.kind = ContentKind::Floor;
                    where.name = floor_rule->floor;
                    where.layer = rule->floor.layer;
                    where.anchor = GridPos{static_cast<i32>(x), static_cast<i32>(y)};
                    spec.content.push_back(std::move(where));
                    taken[static_cast<usize>(rule->floor.layer) * cells + cell] = 1;
                }
            }
        }

        // The scatters, in file order, on the layer's own dice: the same (seed, layer) scatters the same
        // way twice, and two layers of one mine do not scatter alike.
        if (!rule->scatter.empty()) {
            t2d::Rng rng(layer_seed(seed, index));
            for (const ScatterRule& scatter : rule->scatter) {
                scatter_into(spec, scatter, painted, taken, cells, tile_layers, rng);
            }
        }
        return spec;
    };
}

} // namespace mine
