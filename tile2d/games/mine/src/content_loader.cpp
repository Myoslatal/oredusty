#include <mine/content_loader.h>

#include <t2d/core/log.h>

namespace mine {

std::vector<ContentEntry> content_declarations(const t2d::EcfgDocument& document,
                                               std::vector<std::string>* unknown_tables) {
    std::vector<ContentEntry> declared;
    for (const t2d::EcfgValue& table : document.root().children()) {
        ContentKind kind = ContentKind::Count;
        bool known = false;
        for (usize index = 0; index < kContentKindCount; ++index) {
            const auto candidate = static_cast<ContentKind>(index);
            if (table.key() == content_kind_name(candidate)) {
                kind = candidate;
                known = true;
                break;
            }
        }
        if (!known) {
            // A table that is not named after a content kind is reported, never ignored: it is almost
            // always a typo in the data file.
            if (unknown_tables != nullptr) unknown_tables->emplace_back(table.key());
            continue;
        }
        if (!table.is_table()) {
            T2D_WARN("content: '{}' should be a table of {} names", table.key(), content_kind_name(kind));
            continue;
        }
        for (const t2d::EcfgValue& entry : table.children()) {
            declared.push_back(ContentEntry{kind, kNoContent, std::string(entry.key())});
        }
    }
    return declared;
}

ContentLoadReport register_content_from_ecfg(ContentRegistry& registry, const t2d::EcfgDocument& document) {
    ContentLoadReport report;
    for (const ContentEntry& entry : content_declarations(document, &report.unknown_tables)) {
        const ContentId before = registry.find(entry.kind, entry.name);
        const ContentId id = registry.register_content(entry.kind, entry.name);
        if (id == kNoContent) continue; // the registry already reported why
        if (before == kNoContent) ++report.registered;
        else ++report.already_present;
    }
    if (!report.unknown_tables.empty()) {
        T2D_WARN("content: {} table(s) are not content kinds (first: '{}')", report.unknown_tables.size(),
                 report.unknown_tables.front());
    }
    T2D_INFO("content: {} name(s) registered, {} already known", report.registered, report.already_present);
    return report;
}

ContentLoadReport register_content_from_ecfg_text(ContentRegistry& registry, std::string_view text,
                                                  t2d::EcfgError* error) {
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(text, error);
    if (!document.has_value()) return ContentLoadReport{};
    return register_content_from_ecfg(registry, *document);
}

} // namespace mine
