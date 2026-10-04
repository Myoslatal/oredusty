#include <mine/types/tile_definition.h>

#include <mine/types/entity_tile.h>
#include <mine/types/floor.h>
#include <mine/types/scene_tile.h>

#include <format>

namespace mine::types {
namespace {

/// The name of a table as a content kind, or ContentKind::Count when it is not one.
[[nodiscard]] ContentKind kind_of_table(std::string_view table) {
    for (usize index = 0; index < kContentKindCount; ++index) {
        const auto candidate = static_cast<ContentKind>(index);
        if (table == content_kind_name(candidate)) return candidate;
    }
    return ContentKind::Count;
}

} // namespace

bool kind_is_a_tile(ContentKind kind) {
    // What occupies cells. A floor is one (it is the thing the others are placed on), a structure and a
    // machine are; an item, a recipe, a layer and a channel are not on the map at all.
    switch (kind) {
        case ContentKind::Floor:
        case ContentKind::Structure:
        case ContentKind::Machine: return true;
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
    return definition;
}

std::vector<TileDefinition> tile_definitions(const t2d::EcfgDocument& document,
                                             std::vector<std::string>* errors) {
    std::vector<TileDefinition> definitions;
    for (const t2d::EcfgValue& table : document.root().children()) {
        const ContentKind kind = kind_of_table(table.key());
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
    std::unique_ptr<SceneTile> tile;
    switch (definition.kind) {
        case ContentKind::Floor:
            tile = std::make_unique<Floor>(definition.kind, id, layer, anchor, width, height);
            break;
        case ContentKind::Machine:
            // A machine is scenery that also runs, so it is an entity plot.
            tile = std::make_unique<EntityTile>(definition.kind, id, layer, anchor, width, height);
            break;
        case ContentKind::Structure:
            tile = std::make_unique<SceneTile>(definition.kind, id, layer, anchor, width, height);
            break;
        default:
            // Not a plot: an item, a recipe, a layer or a channel describes something else.
            return nullptr;
    }
    tile->set_random_reverse(definition.random_reverse);
    return tile;
}

} // namespace mine::types
