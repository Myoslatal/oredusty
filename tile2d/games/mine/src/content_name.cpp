#include <mine/content_name.h>

#include <format>

namespace mine {

std::string content_label(const ContentRegistry& registry, ContentKind kind, ContentId id,
                          const TextLookup& text, bool* from_table) {
    if (from_table != nullptr) *from_table = false;
    const ContentEntry* entry = registry.find(kind, id);
    const std::string name = entry != nullptr ? entry->name : std::format("#{}", id);

    const std::string by_kind = std::format("content.name.{}.{}", content_kind_name(kind), name);
    const std::string_view specific = text(by_kind);
    if (specific != by_kind) {
        if (from_table != nullptr) *from_table = true;
        return std::string(specific);
    }

    const std::string plain = std::format("content.name.{}", name);
    const std::string_view general = text(plain);
    if (general != plain) {
        if (from_table != nullptr) *from_table = true;
        return std::string(general);
    }

    return name;
}

} // namespace mine
