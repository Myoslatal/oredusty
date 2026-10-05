#include <mine/layer_rules.h>

#include <format>

namespace mine {
namespace {

/// The value of a field the engine reads as a whole number, or nothing when it is not one. A field that
/// is there but is not a number is refused rather than read as zero (layer_rules.h).
[[nodiscard]] std::optional<t2d::i64> read_int(const t2d::EcfgValue& value) {
    if (value.type() != t2d::EcfgType::Int) return std::nullopt;
    return value.as_int();
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
        // A floor the map cannot hold is refused here rather than when the layer is built: the map's own
        // size is what the same entry states, so the two can be checked against each other while the file
        // is read.
        if (rule.tile_layers > 0 && rule.floor.layer >= rule.tile_layers) {
            return fail(std::format("layer '{}': the floor creator works on tile layer {}, and this layer has {}",
                                    name, rule.floor.layer, rule.tile_layers));
        }
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
        // The seed is deliberately not used: a story is the same mine for every world, which is what
        // "pre-set" means. Endless mode is the one that derives a layer from (seed, index).
        (void)seed;
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
        if (rule->floor.rules.empty() || cells == 0) return spec;
        std::vector<const FloorRule*> painted(cells, nullptr);
        for (const FloorRule& floor_rule : rule->floor.rules) {
            switch (floor_rule.kind) {
                case FloorRule::Kind::FullFlash:
                    for (const FloorRule*& cell : painted) cell = &floor_rule;
                    break;
            }
        }
        for (u32 y = 0; y < spec.shape.height; ++y) {
            for (u32 x = 0; x < spec.shape.width; ++x) {
                const FloorRule* cell = painted[static_cast<usize>(y) * spec.shape.width + x];
                if (cell == nullptr) continue;
                PlacedContent where;
                where.kind = ContentKind::Floor;
                where.name = cell->floor;
                where.layer = rule->floor.layer;
                where.anchor = GridPos{static_cast<i32>(x), static_cast<i32>(y)};
                spec.content.push_back(std::move(where));
            }
        }
        return spec;
    };
}

} // namespace mine
