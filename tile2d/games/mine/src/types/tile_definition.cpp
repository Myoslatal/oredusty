#include <mine/types/tile_definition.h>

#include <mine/content_loader.h>
#include <mine/types/tile_runtime.h>

#include <cmath>
#include <format>

#include <mine/runtime/content/runtime_content.h>

#include <mine/runtime/object_data.h>

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
    case ContentKind::Warehouse:
        return true;
    default:
        return false;
    }
}

bool TileDefinition::is_tile() const {
    return kind_is_a_tile(kind);
}

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
    if (const t2d::EcfgValue* tile_type = entry.find("tile_type"); tile_type != nullptr) {
        if (!tile_type->is_string()) {
            return fail(std::format("{} '{}': tile_type must be a registered type name",
                                    content_kind_name(kind), name));
        }
        definition.tile_type = std::string(tile_type->as_string());
        if (definition.tile_type.empty()) {
            return fail(std::format("{} '{}': tile_type must not be empty", content_kind_name(kind), name));
        }
    }
    if (const t2d::EcfgValue* reverse = entry.find("random_reverse"); reverse != nullptr) {
        // A field the engine reads and cannot read is refused: "random_reverse:1" would otherwise be a
        // piece of content that quietly never turns around.
        if (reverse->type() != t2d::EcfgType::Bool) {
            return fail(
                std::format("{} '{}': random_reverse must be true or false", content_kind_name(kind), name));
        }
        definition.random_reverse = reverse->as_bool();
    }
    // "size::" - how many cells the plot covers, read for every kind that is a plot (docs/MODS.md). The
    // layer asks the same numbers when it places one (world.h); a footprint no map could hold is refused
    // here, where it is written, rather than clamped into a size the designer did not ask for.
    if (const t2d::EcfgValue* size = entry.find("size"); size != nullptr) {
        if (!size->is_table()) {
            return fail(std::format("{} '{}': size must be a table (size:: width:1 height:1)",
                                    content_kind_name(kind), name));
        }
        for (const t2d::EcfgValue& field : size->children()) {
            if (field.key() != "width" && field.key() != "height") {
                return fail(std::format("{} '{}': size has no field '{}' (width, height)",
                                        content_kind_name(kind), name, field.key()));
            }
            if (field.type() != t2d::EcfgType::Int) {
                return fail(std::format("{} '{}': size.{} must be a whole number of cells",
                                        content_kind_name(kind), name, field.key()));
            }
            const t2d::i64 value = field.as_int();
            if (value < 1 || value > static_cast<t2d::i64>(ContentGrid::kMaxDimension)) {
                return fail(std::format("{} '{}': size.{} is {}, and a plot is 1..{} cells per side",
                                        content_kind_name(kind), name, field.key(), value,
                                        ContentGrid::kMaxDimension));
            }
            (field.key() == "width" ? definition.width : definition.height) = static_cast<i32>(value);
        }
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
            return fail(
                std::format("building '{}': building must be a table (building:: materials:[...])", name));
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
                    return fail(std::format("building '{}': time is {}, and a build time is not negative",
                                            name, field.as_float()));
                }
                definition.building.seconds = static_cast<f32>(field.as_float());
                continue;
            }
            if (field.key() == "default_unlock") {
                // What a **new save** starts with (docs/GAME_DESIGN.md section 1.44). A field the engine
                // reads and cannot read is refused rather than guessed at, like every other one here.
                if (field.type() != t2d::EcfgType::Bool) {
                    return fail(std::format("building '{}': default_unlock must be true or false", name));
                }
                definition.building.default_unlock = field.as_bool();
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
                                    "category, default_unlock)",
                                    name, field.key()));
        }
    }
    // "research::" - the technology tree node this content is (types/research.h). A definition
    // attribute like building: shared by every plot of the entry, and a layer placement cannot write it
    // (docs/GAME_DESIGN.md section 1.50).
    if (const t2d::EcfgValue* research = entry.find("research"); research != nullptr) {
        if (!research->is_table()) {
            return fail(std::format(
                "research '{}': research must be a table (research:: cost:[...] tree:\"...\")", name));
        }
        Research parsed;
        for (const t2d::EcfgValue& field : research->children()) {
            if (field.key() == "cost") {
                if (!field.is_array()) {
                    return fail(
                        std::format("research '{}': cost must be an array of item names and counts", name));
                }
                if (field.size() % 2 != 0) {
                    return fail(std::format(
                        "research '{}': cost has {} entries, and they come in pairs of name and count", name,
                        field.size()));
                }
                for (usize index = 0; index + 1 < field.size(); index += 2) {
                    const t2d::EcfgValue& item = field.children()[index];
                    const t2d::EcfgValue& count = field.children()[index + 1];
                    if (!item.is_string() || item.as_string().empty()) {
                        return fail(std::format("research '{}': cost[{}] is not the quoted name of an item",
                                                name, index));
                    }
                    if (count.type() != t2d::EcfgType::Int || count.as_int() <= 0) {
                        return fail(std::format("research '{}': cost[{}] for '{}' is not a count above zero",
                                                name, index + 1, item.as_string()));
                    }
                    parsed.cost.push_back(ContentStack{std::string(item.as_string()), count.as_int()});
                }
                continue;
            }
            if (field.key() == "parent") {
                if (!field.is_array()) {
                    return fail(
                        std::format("research '{}': parent must be an array of research node names", name));
                }
                for (const t2d::EcfgValue& item : field.children()) {
                    if (!item.is_string() || item.as_string().empty()) {
                        return fail(std::format(
                            "research '{}': a parent is not the quoted name of a research node", name));
                    }
                    parsed.parents.push_back(std::string(item.as_string()));
                }
                continue;
            }
            if (field.key() == "is_root") {
                if (field.type() != t2d::EcfgType::Bool) {
                    return fail(std::format("research '{}': is_root must be true or false", name));
                }
                parsed.is_root = field.as_bool();
                continue;
            }
            if (field.key() == "tree") {
                if (!field.is_string() || field.as_string().empty()) {
                    return fail(std::format("research '{}': tree must be the quoted name of a tree", name));
                }
                parsed.tree = std::string(field.as_string());
                continue;
            }
            if (field.key() == "min_layer") {
                if (field.type() != t2d::EcfgType::Int) {
                    return fail(std::format("research '{}': min_layer must be a whole mine layer", name));
                }
                const t2d::i64 value = field.as_int();
                if (value < 0 || value > 1000000) {
                    return fail(std::format(
                        "research '{}': min_layer is {}, and a mine layer is not negative", name, value));
                }
                parsed.min_layer = static_cast<i32>(value);
                continue;
            }
            return fail(std::format(
                "research '{}': the research table has no field '{}' (cost, parent, is_root, tree, "
                "min_layer)",
                name, field.key()));
        }
        definition.research = std::move(parsed);
    }
    // "tasks::" - the chain a task block works through (types/task.h). A definition attribute, and the
    // order of the entries is the order they are done in. A field the engine reads and cannot use is
    // refused here, where the file is: a task nobody could complete is not a step to guess at.
    if (const t2d::EcfgValue* tasks = entry.find("tasks"); tasks != nullptr) {
        if (!tasks->is_table()) {
            return fail(std::format("{} '{}': tasks must be a table of tasks (tasks:: <name>:: type:\"deliver\" "
                                    "item:\"stone\" count:10)",
                                    content_kind_name(kind), name));
        }
        for (const t2d::EcfgValue& task_entry : tasks->children()) {
            if (definition.tasks.size() >= runtime::content::kMaxTaskSteps) {
                return fail(std::format(
                    "{} '{}': tasks has more than {} entries, and a task block carries at most {}",
                    content_kind_name(kind), name, runtime::content::kMaxTaskSteps,
                    runtime::content::kMaxTaskSteps));
            }
            if (!task_entry.is_table()) {
                return fail(std::format("{} '{}': task '{}' must be a table (type, item, count)",
                                        content_kind_name(kind), name, task_entry.key()));
            }
            TaskSpec task;
            task.name = std::string(task_entry.key());
            const auto about = [&](std::string_view what) {
                return std::format("{} '{}': task '{}' {}", content_kind_name(kind), name, task.name, what);
            };
            bool has_type = false;
            bool has_item = false;
            bool has_count = false;
            bool has_rate = false;
            bool has_window = false;
            bool has_hold = false;
            std::vector<std::string> unknown;
            for (const t2d::EcfgValue& field : task_entry.children()) {
                if (field.key() == "type") {
                    if (!field.is_string()) {
                        return fail(about("takes a quoted task type (type:\"deliver\" or type:\"rate\")"));
                    }
                    const std::optional<TaskType> type = task_type_from_name(field.as_string());
                    if (!type.has_value()) {
                        return fail(about(std::format(
                            "names task type '{}', and this build has only 'deliver' and 'rate'",
                            field.as_string())));
                    }
                    task.type = *type;
                    has_type = true;
                    continue;
                }
                if (field.key() == "item") {
                    if (!field.is_string() || field.as_string().empty()) {
                        return fail(about("takes the quoted name of the item to deliver (item:\"stone\")"));
                    }
                    task.item = std::string(field.as_string());
                    has_item = true;
                    continue;
                }
                if (field.key() == "count") {
                    if (field.type() != t2d::EcfgType::Int) {
                        return fail(about("takes a whole number of items (count:10)"));
                    }
                    if (field.as_int() <= 0) {
                        return fail(about(std::format(
                            "has a count of {}, and a task asks for at least one item", field.as_int())));
                    }
                    task.count = static_cast<usize>(field.as_int());
                    has_count = true;
                    continue;
                }
                if (field.key() == "rate" || field.key() == "window") {
                    const bool is_rate = field.key() == "rate";
                    if (field.type() != t2d::EcfgType::Int && field.type() != t2d::EcfgType::Float) {
                        return fail(about(std::format("takes a number of {} ({}:3)",
                                                      is_rate ? "items per second" : "seconds",
                                                      field.key())));
                    }
                    if (field.as_float() <= 0.0) {
                        return fail(about(std::format("has a {} of {}, and it must be greater than zero",
                                                      field.key(), field.as_float())));
                    }
                    if (is_rate) {
                        task.rate = static_cast<f32>(field.as_float());
                        has_rate = true;
                    } else {
                        task.window = static_cast<f32>(field.as_float());
                        has_window = true;
                    }
                    continue;
                }
                if (field.key() == "hold") {
                    if (field.type() != t2d::EcfgType::Int && field.type() != t2d::EcfgType::Float) {
                        return fail(about("takes a number of seconds (hold:2)"));
                    }
                    if (field.as_float() < 0.0) {
                        return fail(about(std::format(
                            "has a hold of {}, and a hold cannot be negative", field.as_float())));
                    }
                    task.hold = static_cast<f32>(field.as_float());
                    has_hold = true;
                    continue;
                }
                // The fields a step may carry depend on its type, and the type may come after this one:
                // remember it and say which type's fields were allowed once the type is known.
                unknown.emplace_back(field.key());
            }
            if (!has_type) {
                return fail(about("does not say its type (type:\"deliver\" or type:\"rate\")"));
            }
            const bool rate_step = task.type == TaskType::Rate;
            const std::string_view allowed =
                rate_step ? "type, item, rate, window, hold" : "type, item, count";
            if (!unknown.empty()) {
                return fail(about(std::format("has no field '{}' ({})", unknown.front(), allowed)));
            }
            if (!has_item) return fail(about("does not say what to deliver (item:\"stone\")"));
            if (rate_step) {
                if (has_count) {
                    return fail(about(std::format(
                        "is a rate task and takes no count (rate measures a window: {})", allowed)));
                }
                if (!has_rate) return fail(about("does not say its rate (rate:3)"));
                // window is optional: one second of measurement when the entry does not say. What the
                // step needs is ceil(rate * window) items in that span, and never fewer than one.
                const t2d::f64 need = std::ceil(static_cast<t2d::f64>(task.rate) *
                                                static_cast<t2d::f64>(task.window));
                task.count = need >= 1.0 ? static_cast<usize>(need) : 1u;
            } else {
                if (has_rate || has_window || has_hold) {
                    return fail(about(std::format(
                        "is a deliver task and takes no rate, window or hold (deliver counts the "
                        "container: {})",
                        allowed)));
                }
                if (!has_count) return fail(about("does not say how many (count:10)"));
            }
            definition.tasks.push_back(std::move(task));
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
                return fail(
                    std::format("drill '{}': spin is {}, and a turn takes time", name, spin->as_float()));
            }
            definition.spin = static_cast<f32>(spin->as_float());
        }
        // **Where** it turns: a point of the top's picture, in that picture's own coordinates. Both halves
        // are required and both are inside the picture - a pivot outside it is a part that swings rather
        // than turns, which is not what this field means (docs/GAME_DESIGN.md section 1.38).
        if (const t2d::EcfgValue* origin = entry.find("spin_origin"); origin != nullptr) {
            if (!origin->is_table()) {
                return fail(
                    std::format("drill '{}': spin_origin must be a table (spin_origin:: x:0.5 y:0.5)", name));
            }
            f32 x = 0.5f;
            f32 y = 0.5f;
            for (const t2d::EcfgValue& half : origin->children()) {
                if (half.key() != "x" && half.key() != "y") {
                    return fail(
                        std::format("drill '{}': spin_origin has no field '{}' (x, y)", name, half.key()));
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
    // "capacity" - how many things a container holds. Read for every plot kind, not just warehouse::
    // (docs/GAME_DESIGN.md section 1.51): any content whose tile type carries the container component can
    // declare one - the stone warehouse and the task block today. A capacity is a count, so it has to be
    // a whole number: "capacity:2.5" is a container whose size its author cannot have meant, and 0 is one
    // that can hold nothing.
    if (const t2d::EcfgValue* capacity = entry.find("capacity"); capacity != nullptr) {
        if (capacity->type() != t2d::EcfgType::Int) {
            return fail(std::format("{} '{}': capacity must be a whole number", content_kind_name(kind), name));
        }
        const t2d::i64 value = capacity->as_int();
        if (value <= 0) {
            return fail(
                std::format("{} '{}': capacity must be greater than zero", content_kind_name(kind), name));
        }
        definition.capacity = static_cast<usize>(value);
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
            std::optional<TileDefinition> definition = read_tile_definition(kind, entry.key(), entry, &error);
            if (!definition.has_value()) {
                if (errors != nullptr) errors->push_back(std::move(error));
                continue;
            }
            definitions.push_back(std::move(*definition));
        }
    }
    return definitions;
}

std::unique_ptr<Tile> make_tile(const ContentDefinitions& definitions, const TileDefinition& definition,
                                const ContentRegistry& registry, ContentId id, i32 layer, GridPos anchor,
                                i32 width, i32 height) {
    if (!kind_is_a_tile(definition.kind)) return nullptr;
    // The definitions own the shared records and the archetype; a Tile keeps the library alive through
    // its shared_ptr, so a content reload does not free the layout under an object that already exists.
    const std::shared_ptr<const TileRuntimeLibrary> library = definitions.runtime(registry);
    if (library == nullptr) return nullptr;
    const std::shared_ptr<const RuntimeTileDefinition> runtime_definition =
        library->definition_for(definition, registry);
    if (runtime_definition == nullptr || runtime_definition->archetype == nullptr ||
        runtime_definition->tile_type == nullptr) {
        return nullptr;
    }
    // 0 is "the size the definition declares": a placement that says nothing takes the entry's
    // footprint, and one that names a size of its own - a save of a plot that had one - keeps it.
    const i32 cells_wide = width > 0 ? width : definition.width;
    const i32 cells_high = height > 0 ? height : definition.height;
    auto tile = std::make_unique<Tile>(library, runtime_definition, id, layer, anchor, cells_wide, cells_high);
    // The drag half reaches its own container through the object's data, so the link is made once the
    // object exists. It is process-local state and is not saved (game_save.cpp).
    if (tile->has_drag()) (void)runtime::content::link_drag(*tile->object());
    return tile;
}

} // namespace mine::types
