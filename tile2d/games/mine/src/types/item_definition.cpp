#include <mine/types/item_definition.h>

#include <format>
#include <utility>

namespace mine::types {

std::optional<ItemDefinition> read_item_definition(ContentKind kind, std::string_view name,
                                                   const t2d::EcfgValue& entry, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ItemDefinition> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    if (kind != ContentKind::Item) {
        return fail(std::format("'{}' is not an item: it was declared in the {} table", name,
                                content_kind_name(kind)));
    }

    ItemDefinition definition;
    definition.kind = kind;
    definition.name = std::string(name);

    if (const t2d::EcfgValue* image = entry.find("image"); image != nullptr) {
        if (!image->is_string()) {
            return fail(std::format("item '{}': image must be a quoted path", name));
        }
        definition.image = std::string(image->as_string());
    }
    return definition;
}

std::vector<ItemDefinition> item_definitions(const t2d::EcfgDocument& document,
                                             std::vector<std::string>* errors) {
    std::vector<ItemDefinition> definitions;
    for (const t2d::EcfgValue& table : document.root().children()) {
        if (content_kind_from_name(table.key()) != ContentKind::Item) continue;
        if (!table.is_table()) continue;
        for (const t2d::EcfgValue& entry : table.children()) {
            std::string error;
            std::optional<ItemDefinition> definition =
                read_item_definition(ContentKind::Item, entry.key(), entry, &error);
            if (!definition.has_value()) {
                if (errors != nullptr) errors->push_back(std::move(error));
                continue;
            }
            definitions.push_back(std::move(*definition));
        }
    }
    return definitions;
}

Item make_item(const ItemDefinition& definition, ContentId id) { return Item(id, definition.image); }

} // namespace mine::types
