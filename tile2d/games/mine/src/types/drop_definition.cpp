#include <mine/types/drop_definition.h>

#include <format>
#include <utility>

namespace mine::types {

std::optional<DropDefinition> read_drop_definition(ContentKind kind, std::string_view name,
                                                   const t2d::EcfgValue& entry, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<DropDefinition> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    if (kind != ContentKind::Drop) {
        return fail(std::format("'{}' is not a drop: it was declared in the {} table", name,
                                content_kind_name(kind)));
    }

    DropDefinition definition;
    definition.kind = kind;
    definition.name = std::string(name);

    if (const t2d::EcfgValue* image = entry.find("image"); image != nullptr) {
        if (!image->is_string()) {
            return fail(std::format("drop '{}': image must be a quoted path", name));
        }
        definition.image = std::string(image->as_string());
    }
    return definition;
}

std::vector<DropDefinition> drop_definitions(const t2d::EcfgDocument& document,
                                             std::vector<std::string>* errors) {
    std::vector<DropDefinition> definitions;
    for (const t2d::EcfgValue& table : document.root().children()) {
        if (content_kind_from_name(table.key()) != ContentKind::Drop) continue;
        if (!table.is_table()) continue;
        for (const t2d::EcfgValue& entry : table.children()) {
            std::string error;
            std::optional<DropDefinition> definition =
                read_drop_definition(ContentKind::Drop, entry.key(), entry, &error);
            if (!definition.has_value()) {
                if (errors != nullptr) errors->push_back(std::move(error));
                continue;
            }
            definitions.push_back(std::move(*definition));
        }
    }
    return definitions;
}

std::unique_ptr<Drop> make_drop(const DropDefinition& definition, ContentId id, t2d::Vec2 position) {
    return std::make_unique<Drop>(definition.kind, id, position);
}

} // namespace mine::types
