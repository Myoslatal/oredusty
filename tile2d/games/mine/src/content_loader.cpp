#include <mine/content_loader.h>

#include <t2d/core/log.h>

#include <filesystem>
#include <format>

namespace mine {

std::vector<ContentEntry> content_declarations(const t2d::EcfgDocument& document,
                                               std::vector<std::string>* unknown_tables,
                                               std::string_view ignore_table) {
    std::vector<ContentEntry> declared;
    for (const t2d::EcfgValue& table : document.root().children()) {
        if (!ignore_table.empty() && table.key() == ignore_table) continue;
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
            // always a typo in the data file. A caller that knows about extra tables of its own (a
            // pack's "pack::" header, for instance) filters them out before calling this.
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

std::vector<ContentImage> content_images(const t2d::EcfgDocument& document, std::string_view ignore_table) {
    std::vector<ContentImage> images;
    for (const t2d::EcfgValue& table : document.root().children()) {
        if (!ignore_table.empty() && table.key() == ignore_table) continue;
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
        if (!known || !table.is_table()) continue;
        for (const t2d::EcfgValue& entry : table.children()) {
            const t2d::EcfgValue* image = entry.find("image");
            if (image == nullptr) continue;
            if (!image->is_string()) {
                T2D_WARN("content: {} '{}' has an image that is not a quoted path", content_kind_name(kind),
                         entry.key());
                continue;
            }
            images.push_back(ContentImage{kind, std::string(entry.key()), std::string(image->as_string())});
        }
    }
    return images;
}

std::vector<ResolvedImage> resolve_content_images(const t2d::EcfgDocument& document,
                                                        const std::string& source_path,
                                                        std::string_view ignore_table) {
    std::vector<ResolvedImage> images;
    const std::filesystem::path base_directory = std::filesystem::path(source_path).parent_path();
    for (const ContentImage& declared : content_images(document, ignore_table)) {
        ResolvedImage entry;
        entry.kind = declared.kind;
        entry.content = declared.name;
        entry.source = source_path;
        entry.path = declared.path;
        const std::filesystem::path resolved = base_directory / declared.path;
        std::error_code code;
        if (declared.path.empty()) {
            entry.error = "the image path is empty";
        } else if (!std::filesystem::is_regular_file(resolved, code)) {
            entry.error = std::format("'{}' is not there", resolved.string());
        } else if (resolved.extension() != ".png") {
            // Ore's decoder reads PNG; anything else would need another one, and pretending otherwise
            // would fail later, at draw time, where it is much harder to explain.
            entry.error = std::format("'{}': only PNG is decoded", resolved.extension().string());
        } else {
            entry.resolved = resolved.string();
            entry.ok = true;
        }
        images.push_back(std::move(entry));
    }
    return images;
}

ContentRegistrationReport register_declared_content(ContentRegistry& registry,
                                                    t2d::ConstSpan<const ContentEntry> declared,
                                                    std::vector<ContentEntry>* registered_out) {
    ContentRegistrationReport report;
    for (const ContentEntry& entry : declared) {
        if (registry.find(entry.kind, entry.name) != kNoContent) {
            report.collisions.push_back(entry);
            continue;
        }
        const ContentId id = registry.register_content(entry.kind, entry.name);
        if (id == kNoContent) continue;   // the registry reported why
        if (registered_out != nullptr) registered_out->push_back(ContentEntry{entry.kind, id, entry.name});
        ++report.registered;
    }
    return report;
}

ContentLoadReport register_content_from_ecfg(ContentRegistry& registry, const t2d::EcfgDocument& document) {
    ContentLoadReport report;
    const std::vector<ContentEntry> declared = content_declarations(document, &report.unknown_tables);
    for (const ContentEntry& entry : declared) {
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
