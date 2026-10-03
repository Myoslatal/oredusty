#include <mine/content_loader.h>

#include <t2d/core/log.h>

namespace mine {

ContentLoadReport register_content_from_ecfg(ContentRegistry& registry, const t2d::EcfgDocument& document) {
    ContentLoadReport report;
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
            report.unknown_tables.emplace_back(table.key());
            continue;
        }
        if (!table.is_table()) {
            T2D_WARN("content: '{}' should be a table of {} names", table.key(), content_kind_name(kind));
            continue;
        }
        for (const t2d::EcfgValue& entry : table.children()) {
            const ContentId before = registry.find(kind, entry.key());
            const ContentId id = registry.register_content(kind, entry.key());
            if (id == kNoContent) continue; // the registry already reported why
            if (before == kNoContent) ++report.registered;
            else ++report.already_present;
        }
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
