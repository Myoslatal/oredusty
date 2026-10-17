#include <mine/types/tile_definition.h>

#include <mine/types/drill.h>
#include <mine/types/entity_tile.h>
#include <mine/types/floor.h>
#include <mine/types/scene_tile.h>
#include <mine/types/single_ore.h>
#include <mine/types/warehouse.h>

#include <format>

namespace mine::types {

bool kind_is_a_tile(ContentKind kind) {
    // What occupies cells. A floor is one (it is the thing the others are placed on), an ore is one (it
    // is dug out of the cell it sits in), a structure and a machine are; an item, a recipe, a layer and
    // a channel are not on the map at all.
    switch (kind) {
        case ContentKind::Floor:
        case ContentKind::Ore:
        case ContentKind::Structure:
        case ContentKind::Machine:
        case ContentKind::Drill:
        case ContentKind::Warehouse: return true;
        default: return false;
    }
}

bool TileDefinition::is_tile() const { return kind_is_a_tile(kind); }

std::optional<TileDefinition> read_tile_definition(ContentKind kind, std::string_view name,
                                                   const t2d::EcfgValue& entry, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<TileDefinition> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    TileDefinition definition;
    definition.kind = kind;
    definition.name = std::string(name);

    if (const t2d::EcfgValue* image = entry.find("image"); image != nullptr) {
        if (!image->is_string()) {
            return fail(std::format("{} '{}': image must be a quoted path", content_kind_name(kind), name));
        }
        definition.image = std::string(image->as_string());
    }
    if (const t2d::EcfgValue* reverse = entry.find("random_reverse"); reverse != nullptr) {
        // A field the engine reads and cannot read is refused: "random_reverse:1" would otherwise be a
        // piece of content that quietly never turns around.
        if (reverse->type() != t2d::EcfgType::Bool) {
            return fail(std::format("{} '{}': random_reverse must be true or false", content_kind_name(kind), name));
        }
        definition.random_reverse = reverse->as_bool();
    }

    // The two numbers a mine is made of, read for the kinds that have them (docs/GAME_DESIGN.md section
    // 1.26). A number the engine reads and cannot use is refused here, where the file is: a drill with no
    // power and an ore with no hardness are data errors, not things to guess a rate for.
    const auto positive = [&](std::string_view field, f32& out) -> std::optional<std::string> {
        const t2d::EcfgValue* value = entry.find(field);
        if (value == nullptr) return std::nullopt;
        if (value->type() != t2d::EcfgType::Int && value->type() != t2d::EcfgType::Float) {
            return std::format("{} '{}': {} must be a number", content_kind_name(kind), name, field);
        }
        out = static_cast<f32>(value->as_float());
        if (!(out > 0.0f)) {
            return std::format("{} '{}': {} must be greater than zero", content_kind_name(kind), name, field);
        }
        return std::nullopt;
    };
        // "building::" - what this plot costs to build, how long it takes and what it is for
        // (types/building.h). A **definition** attribute: shared by every plot of the entry, and a layer
        // placement has no way to write it (docs/GAME_DESIGN.md sections 1.34, 1.35).
        if (const t2d::EcfgValue* building = entry.find("building"); building != nullptr) {
            if (!building->is_table()) {
                return fail(std::format("building '{}': building must be a table (building:: materials:[...])",
                                        name));
            }
            for (const t2d::EcfgValue& field : building->children()) {
                if (field.key() == "materials") {
                    if (!field.is_array()) {
                        return fail(std::format("building '{}': materials must be an array of item names and "
                                                "counts",
                                                name));
                    }
                    if (field.size() % 2 != 0) {
                        return fail(std::format("building '{}': materials has {} entries, and they come in pairs "
                                                "of name and count",
                                                name, field.size()));
                    }
                    for (usize index = 0; index + 1 < field.size(); index += 2) {
                        const t2d::EcfgValue& item = field.children()[index];
                        const t2d::EcfgValue& count = field.children()[index + 1];
                        if (!item.is_string() || item.as_string().empty()) {
                            return fail(std::format("building '{}': materials[{}] is not the quoted name of an "
                                                    "item",
                                                    name, index));
                        }
                        if (count.type() != t2d::EcfgType::Int || count.as_int() <= 0) {
                            return fail(std::format("building '{}': materials[{}] for '{}' is not a count above "
                                                    "zero",
                                                    name, index + 1, item.as_string()));
                        }
                        definition.building.materials.push_back(
                            ContentStack{std::string(item.as_string()), count.as_int()});
                    }
                    continue;
                }
                if (field.key() == "time") {
                    if (field.type() != t2d::EcfgType::Int && field.type() != t2d::EcfgType::Float) {
                        return fail(std::format("building '{}': time must be a number of seconds", name));
                    }
                    if (field.as_float() < 0.0) {
                        return fail(std::format("building '{}': time is {}, and a build time is not negative", name,
                                                field.as_float()));
                    }
                    definition.building.seconds = static_cast<f32>(field.as_float());
                    continue;
                }
                if (field.key() == "category") {
                    if (!field.is_string()) {
                        return fail(std::format("building '{}': category must be a quoted name (storage, "
                                                "processing, extraction)",
                                                name));
                    }
                    const std::optional<BuildingCategory> category =
                        building_category_from_name(field.as_string());
                    if (!category.has_value()) {
                        return fail(std::format("building '{}': '{}' is not a building category (storage, "
                                                "processing, extraction)",
                                                name, field.as_string()));
                    }
                    definition.building.category = *category;
                    continue;
                }
                return fail(std::format("building '{}': the building table has no field '{}' (materials, time, "
                                        "category)",
                                        name, field.key()));
            }
        }
    if (kind == ContentKind::Drill) {
        if (std::optional<std::string> problem = positive("dig_power", definition.dig_power);
            problem.has_value()) {
            return fail(std::move(*problem));
        }
        // A drill has **two** pictures: the base it stands on and the top that turns on it, plus how long
        // one turn takes (docs/GAME_DESIGN.md section 1.38). Both are a drill's words, so a floor that
        // says them is a typo rather than a floor with a spinning top.
        if (const t2d::EcfgValue* top = entry.find("top_image"); top != nullptr) {
            if (!top->is_string() || top->as_string().empty()) {
                return fail(std::format("drill '{}': top_image must be the quoted path of a picture", name));
            }
            definition.top_image = std::string(top->as_string());
        }
        if (const t2d::EcfgValue* spin = entry.find("spin"); spin != nullptr) {
            if (spin->type() != t2d::EcfgType::Int && spin->type() != t2d::EcfgType::Float) {
                return fail(std::format("drill '{}': spin must be a number of seconds per turn", name));
            }
            if (!(spin->as_float() > 0.0)) {
                return fail(std::format("drill '{}': spin is {}, and a turn takes time", name, spin->as_float()));
            }
            definition.spin = static_cast<f32>(spin->as_float());
        }
        // **Where** it turns: a point of the top's picture, in that picture's own coordinates. Both halves
        // are required and both are inside the picture - a pivot outside it is a part that swings rather
        // than turns, which is not what this field means (docs/GAME_DESIGN.md section 1.38).
        if (const t2d::EcfgValue* origin = entry.find("spin_origin"); origin != nullptr) {
            if (!origin->is_table()) {
                return fail(std::format("drill '{}': spin_origin must be a table (spin_origin:: x:0.5 y:0.5)",
                                        name));
            }
            f32 x = 0.5f;
            f32 y = 0.5f;
            for (const t2d::EcfgValue& half : origin->children()) {
                if (half.key() != "x" && half.key() != "y") {
                    return fail(std::format("drill '{}': spin_origin has no field '{}' (x, y)", name,
                                            half.key()));
                }
                if (half.type() != t2d::EcfgType::Int && half.type() != t2d::EcfgType::Float) {
                    return fail(std::format("drill '{}': spin_origin {} must be a number", name, half.key()));
                }
                const f32 value = static_cast<f32>(half.as_float());
                if (!(value >= 0.0f) || !(value <= 1.0f)) {
                    return fail(std::format("drill '{}': spin_origin {} is {}, and the picture runs 0 to 1",
                                            name, half.key(), value));
                }
                (half.key() == "x" ? x : y) = value;
            }
            definition.spin_origin = t2d::Vec2{x, y};
        }
    }
    if (kind == ContentKind::Warehouse) {
        // A capacity is a count, so it has to be a whole number: "capacity:2.5" is a warehouse whose size
        // its author cannot have meant, and 0 is one that can hold nothing.
        if (const t2d::EcfgValue* capacity = entry.find("capacity"); capacity != nullptr) {
            if (capacity->type() != t2d::EcfgType::Int) {
                return fail(std::format("warehouse '{}': capacity must be a whole number", name));
            }
            const t2d::i64 value = capacity->as_int();
            if (value <= 0) {
                return fail(std::format("warehouse '{}': capacity must be greater than zero", name));
            }
            definition.capacity = static_cast<usize>(value);
        }
    }
    if (kind == ContentKind::Ore) {
        if (std::optional<std::string> problem = positive("hardness", definition.hardness);
            problem.has_value()) {
            return fail(std::move(*problem));
        }
        if (const t2d::EcfgValue* drop = entry.find("drop"); drop != nullptr) {
            if (!drop->is_string() || drop->as_string().empty()) {
                return fail(std::format("ore '{}': drop must be the quoted name of a drop", name));
            }
            definition.drop = std::string(drop->as_string());
        }
    }
    return definition;
}

std::vector<TileDefinition> tile_definitions(const t2d::EcfgDocument& document,
                                             std::vector<std::string>* errors) {
    std::vector<TileDefinition> definitions;
    for (const t2d::EcfgValue& table : document.root().children()) {
        const ContentKind kind = content_kind_from_name(table.key());
        if (kind == ContentKind::Count || !kind_is_a_tile(kind)) continue;
        if (!table.is_table()) continue;
        for (const t2d::EcfgValue& entry : table.children()) {
            std::string error;
            std::optional<TileDefinition> definition =
                read_tile_definition(kind, entry.key(), entry, &error);
            if (!definition.has_value()) {
                if (errors != nullptr) errors->push_back(std::move(error));
                continue;
            }
            definitions.push_back(std::move(*definition));
        }
    }
    return definitions;
}

std::unique_ptr<SceneTile> make_tile(const TileDefinition& definition, ContentId id, i32 layer, GridPos anchor,
                                      i32 width, i32 height) {
    // The plot keeps a handle on the one definition record, and reads every fixed attribute through it
    // (docs/GAME_DESIGN.md section 1.35). The definitions outlive the layers that are built from them.
    std::unique_ptr<SceneTile> tile;
    switch (definition.kind) {
        case ContentKind::Floor:
            tile = std::make_unique<Floor>(definition.kind, id, layer, anchor, width, height);
            break;
        case ContentKind::Ore:
            // One cell of ore is one plot: what it yields is its content, and a scatter places one per
            // cell, so "which cell is ore" and "which plot is that ore" are the same question.
            tile = std::make_unique<SingleOre>(definition.kind, id, layer, anchor, width, height);
            break;
        case ContentKind::Machine:
            // A machine is scenery that also runs, so it is an entity plot.
            tile = std::make_unique<EntityTile>(definition.kind, id, layer, anchor, width, height);
            break;
        case ContentKind::Drill: {
            // A drill is a machine that mines what is under it: an entity plot whose digging power comes
            // from its entry. What it mines is decided when the layer places it, because that is where
            // the ore under it is known (types/drill.h, world.h).
            auto drill = std::make_unique<Drill>(definition.kind, id, layer, anchor, width, height);
            drill->set_dig_power(definition.dig_power);
            tile = std::move(drill);
            break;
        }
        case ContentKind::Structure:
            tile = std::make_unique<SceneTile>(definition.kind, id, layer, anchor, width, height);
            break;
        case ContentKind::Warehouse: {
            // A warehouse is scenery that holds things: a scene plot (never ticked) whose container has
            // the capacity its entry declared (types/warehouse.h).
            auto warehouse = std::make_unique<Warehouse>(definition.kind, id, layer, anchor, width, height);
            warehouse->container()->set_capacity(definition.capacity);
            tile = std::move(warehouse);
            break;
        }
        default:
            // Not a plot: an item, a recipe, a layer or a channel describes something else.
            return nullptr;
    }
    tile->set_random_reverse(definition.random_reverse);
    tile->set_definition(&definition);
    return tile;
}

} // namespace mine::types
