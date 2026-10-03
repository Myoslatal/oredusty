#include <mine/content_pack.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace mine {
namespace {

namespace fs = std::filesystem;

/// The one table a pack may carry besides content. It is metadata, not content, so the walk over the
/// document has to skip it - and a pack that misspells it gets told, because "packk::" would otherwise
/// be reported as an unknown content kind and the pack would look anonymous.
constexpr const char* kPackTable = "pack";

constexpr const char* kPackKeys[] = {"id", "name", "version", "requires"};

[[nodiscard]] bool is_pack_key(std::string_view key) {
    for (const char* known : kPackKeys) {
        if (key == known) return true;
    }
    return false;
}

[[nodiscard]] std::vector<std::string> read_string_list(const t2d::EcfgValue& value) {
    std::vector<std::string> out;
    if (value.is_string()) {
        out.emplace_back(value.as_string());
        return out;
    }
    if (!value.is_array()) return out;
    for (const t2d::EcfgValue& item : value.children()) {
        if (item.is_string()) out.emplace_back(item.as_string());
    }
    return out;
}

} // namespace

std::optional<ContentPack> ContentPack::load(const std::string& path, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ContentPack> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    t2d::EcfgError parse_error;
    std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &parse_error);
    if (!document.has_value()) return fail(parse_error.describe(path));

    ContentPack pack;
    pack.path = path;
    const fs::path file(path);
    pack.id = file.stem().string();
    pack.content = std::move(*document);

    const t2d::EcfgValue* header = pack.content.root().find(kPackTable);
    if (header != nullptr) {
        if (!header->is_table()) return fail(std::format("{}: '{}' must be a table", path, kPackTable));
        pack.declared = true;
        for (const t2d::EcfgValue& entry : header->children()) {
            if (!is_pack_key(entry.key())) {
                return fail(std::format("{}: '{}' is not a pack key (pack:: takes id, name, version, requires)",
                                        path, entry.key()));
            }
        }
        if (const t2d::EcfgValue* id = header->find("id"); id != nullptr) {
            if (!id->is_string() || id->as_string().empty()) {
                return fail(std::format("{}: pack::id must be a non empty string", path));
            }
            pack.id = std::string(id->as_string());
        }
        if (const t2d::EcfgValue* name = header->find("name"); name != nullptr) {
            if (!name->is_string()) return fail(std::format("{}: pack::name must be a string", path));
            pack.name = std::string(name->as_string());
        }
        if (const t2d::EcfgValue* version = header->find("version"); version != nullptr) {
            if (!version->is_string()) return fail(std::format("{}: pack::version must be a string", path));
            pack.version = std::string(version->as_string());
        }
        if (const t2d::EcfgValue* requirements = header->find("requires"); requirements != nullptr) {
            pack.requirements = read_string_list(*requirements);
            if (pack.requirements.empty() && !requirements->is_array() && !requirements->is_string()) {
                return fail(std::format("{}: pack::requires must be a name or a list of names", path));
            }
        }
    }
    if (pack.name.empty()) pack.name = pack.id;
    return pack;
}

std::vector<std::string> ContentPack::scan_directory(const std::string& directory,
                                                     std::vector<std::string>* errors) {
    std::vector<std::string> files;
    std::error_code code;
    const fs::path root(directory);
    if (!fs::exists(root, code)) {
        if (errors != nullptr) errors->push_back(std::format("packs: '{}' does not exist", directory));
        return files;
    }
    if (fs::is_regular_file(root, code)) {
        files.push_back(root.string());   // a caller that passed a file meant that file
        return files;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(root, code)) {
        if (!entry.is_regular_file(code)) continue;
        if (entry.path().extension() != ".ecfg") continue;
        files.push_back(entry.path().string());
    }
    // Sorted, so the load order does not depend on what the filesystem happens to return first.
    std::sort(files.begin(), files.end());
    return files;
}

void ContentPipeline::set_base_files(std::vector<std::string> paths) { base_files_ = std::move(paths); }
void ContentPipeline::set_pack_files(std::vector<std::string> paths) { pack_files_ = std::move(paths); }

void ContentPipeline::set_pack_directories(std::vector<std::string> directories) {
    pack_directories_ = std::move(directories);
}

void ContentPipeline::set_mod_directories(std::vector<std::string> directories) {
    mod_directories_ = std::move(directories);
}

bool ContentPipeline::empty() const {
    return base_files_.empty() && pack_files_.empty() && pack_directories_.empty() && mod_directories_.empty();
}

void ContentPipeline::unload() {
    mods_.unload();
    packs_.clear();
}

const ContentPipelineReport& ContentPipeline::load(ContentRegistry& registry) {
    unload();
    report_ = ContentPipelineReport{};
    registry.clear();

    // --- the game's own content -------------------------------------------------------------------
    std::vector<t2d::EcfgDocument> base_documents;
    bool base_ok = true;
    for (const std::string& path : base_files_) {
        t2d::EcfgError parse_error;
        std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &parse_error);
        if (!document.has_value()) {
            report_.errors.push_back(parse_error.describe(path));
            base_ok = false;
            break;
        }
        base_documents.push_back(std::move(*document));
        ++report_.base_files;
    }
    if (base_ok) {
        for (const t2d::EcfgDocument& document : base_documents) {
            std::vector<std::string> unknown_tables;
            const std::vector<ContentEntry> declared = content_declarations(document, &unknown_tables);
            const ContentRegistrationReport registered = register_declared_content(registry, declared);
            report_.base_registered += registered.registered;
            for (const ContentEntry& clash : registered.collisions) {
                report_.errors.push_back(std::format("content: {} '{}' is registered twice in the game's own files",
                                                     content_kind_name(clash.kind), clash.name));
            }
            for (const std::string& table : unknown_tables) {
                report_.warnings.push_back(std::format("content: '{}' is not a content kind", table));
            }
        }
    }

    // --- content packs ----------------------------------------------------------------------------
    std::vector<std::string> pack_paths = pack_files_;
    for (const std::string& directory : pack_directories_) {
        const std::vector<std::string> found = ContentPack::scan_directory(directory, &report_.errors);
        pack_paths.insert(pack_paths.end(), found.begin(), found.end());
    }
    std::vector<ContentPack> parsed;
    for (const std::string& path : pack_paths) {
        std::string error;
        std::optional<ContentPack> pack = ContentPack::load(path, &error);
        if (!pack.has_value()) {
            report_.errors.push_back(error);
            continue;
        }
        parsed.push_back(std::move(*pack));
    }
    // Two packs with the same id: the second is refused, and says so.
    {
        std::vector<ContentPack> unique;
        for (ContentPack& pack : parsed) {
            const bool duplicate = std::any_of(unique.begin(), unique.end(), [&](const ContentPack& other) {
                return other.id == pack.id;
            });
            if (duplicate) {
                report_.errors.push_back(std::format("packs: '{}' is defined twice ('{}' and '{}')", pack.id,
                                                     unique.front().path, pack.path));
                continue;
            }
            unique.push_back(std::move(pack));
        }
        parsed = std::move(unique);
    }
    const RequirementOrder order = order_by_requirements(
        parsed.size(), [&](usize index) { return parsed[index].id; },
        [&](usize index) { return parsed[index].requirements; });
    for (const std::string& error : order.errors) report_.errors.push_back(std::format("pack {}", error));
    for (const usize index : order.dropped) {
        ContentPack failed = parsed[index];
        failed.ok = false;
        failed.error = "not loaded: a requirement is missing, or the requirements form a cycle";
        packs_.push_back(std::move(failed));
    }
    for (const usize index : order.order) {
        ContentPack& pack = parsed[index];
        std::vector<std::string> unknown_tables;
        const std::vector<ContentEntry> declared =
            content_declarations(pack.content, &unknown_tables, kPackTable);
        const ContentRegistrationReport registered =
            register_declared_content(registry, declared, &pack.registered);
        report_.pack_content += registered.registered;
        for (const ContentEntry& clash : registered.collisions) {
            const std::string message = std::format("pack '{}': {} '{}' is already registered and was not replaced",
                                                    pack.id, content_kind_name(clash.kind), clash.name);
            report_.errors.push_back(message);
            if (pack.error.empty()) pack.error = message;
        }
        for (const std::string& table : unknown_tables) {
            report_.warnings.push_back(std::format("pack '{}': '{}' is not a content kind", pack.id, table));
        }
        ++report_.packs;
        packs_.push_back(std::move(pack));
    }

    // --- mod packages -----------------------------------------------------------------------------
    const ModLoadReport& mods = mods_.load(mod_directories_, registry);
    report_.mods = mods_.count();
    report_.native_mods = mods.native_modules;
    report_.mod_content = mods.content_registered;
    for (const std::string& error : mods.errors) report_.errors.push_back(error);
    for (const std::string& warning : mods.warnings) report_.warnings.push_back(warning);

    report_.total_content = registry.total_count();
    T2D_INFO("content: {} base, {} pack(s) with {}, {} mod(s) with {} -> {} registered, {} error(s)",
             report_.base_registered, report_.packs, report_.pack_content, report_.mods, report_.mod_content,
             report_.total_content, report_.errors.size());
    for (const std::string& error : report_.errors) T2D_WARN("content: {}", error);
    for (const std::string& warning : report_.warnings) T2D_WARN("content: {}", warning);
    return report_;
}

} // namespace mine
