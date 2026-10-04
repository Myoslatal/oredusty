#include <mine/content_pack.h>

#include <mine/types/tile_definition.h>

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
    // A directory that carries a mod.ecfg is a **mod package**, not a pack project: its content files
    // belong to the mod host, which loads them in the order its manifest gives (mod_package.h). Loading
    // them here as well would register every one of the mod's names twice, and the second registration
    // is reported as a collision - which is what dropping a pack and a mod into one directory (the
    // game's own "packs" directory, content_search.h) would produce.
    if (fs::is_regular_file(root / "mod.ecfg", code)) return files;

    // Recursive, so a workspace can hold one *directory per pack project* - a pack is one .ecfg file,
    // but a project is a folder with that file, its notes and its art in it. Entries that start with a
    // dot are skipped, which keeps .git and editor leftovers out of the load, and a mod package is not
    // descended into.
    std::error_code walk;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, walk), end;
         it != end; it.increment(walk)) {
        if (walk) break;
        const fs::path& path = it->path();
        const std::string name = path.filename().string();
        if (!name.empty() && name[0] == '.') {
            it.disable_recursion_pending();
            continue;
        }
        std::error_code entry_code;
        if (it->is_directory(entry_code)) {
            if (fs::is_regular_file(path / "mod.ecfg", entry_code)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(entry_code)) continue;
        if (path.extension() != ".ecfg") continue;
        files.push_back(path.string());
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
    // One record per file before anything is read, so a file that never gets read - the ones behind a
    // file that failed - is on the list with the reason, instead of quietly missing from it.
    for (const std::string& path : base_files_) {
        ContentSource source;
        source.kind = SourceKind::File;
        source.path = path;
        source.id = fs::path(path).stem().string();
        source.name = source.id;
        report_.sources.push_back(std::move(source));
    }
    std::vector<t2d::EcfgDocument> base_documents;
    bool base_ok = true;
    usize failed_at = 0;
    for (usize index = 0; index < base_files_.size(); ++index) {
        t2d::EcfgError parse_error;
        std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(base_files_[index], &parse_error);
        if (!document.has_value()) {
            const std::string message = parse_error.describe(base_files_[index]);
            report_.errors.push_back(message);
            report_.sources[index].ok = false;
            report_.sources[index].error = message;
            base_ok = false;
            failed_at = index;
            break;
        }
        base_documents.push_back(std::move(*document));
        ++report_.base_files;
    }
    if (!base_ok) {
        // The game's own content is all or nothing - one file that does not parse means none of them
        // register - so the files behind it were never read at all.
        for (usize index = failed_at + 1; index < base_files_.size(); ++index) {
            report_.sources[index].ok = false;
            report_.sources[index].error =
                std::format("not read: '{}' failed first", base_files_[failed_at]);
        }
    } else {
        for (usize index = 0; index < base_documents.size(); ++index) {
            std::vector<std::string> unknown_tables;
            const std::vector<ContentEntry> declared = content_declarations(base_documents[index], &unknown_tables);
            // The game's own content ships its own art, and names it the way everything else does:
            // relative to the file that declares it.
            for (ResolvedImage& image : resolve_content_images(base_documents[index], base_files_[index])) {
                if (!image.ok) {
                    const std::string message =
                        std::format("content: {} '{}': {}", content_kind_name(image.kind), image.content,
                                    image.error);
                    report_.errors.push_back(message);
                    if (report_.sources[index].error.empty()) report_.sources[index].error = message;
                } else {
                    ++report_.base_images;
                    ++report_.sources[index].images;
                }
                if (!image.ok) ++report_.sources[index].images_failed;
                report_.images.push_back(std::move(image));
            }
            // The other field the engine reads: a "random_reverse" it cannot read is a data error, not
            // a plot that quietly never turns around. The definitions are what building a layer reads
            // (world.h), so they are kept rather than only checked.
            std::vector<std::string> definition_errors;
            for (types::TileDefinition& definition :
                 types::tile_definitions(base_documents[index], &definition_errors)) {
                report_.definitions.add(std::move(definition));
            }
            for (const std::string& definition_error : definition_errors) {
                const std::string message = std::format("content: {}", definition_error);
                report_.errors.push_back(message);
                if (report_.sources[index].error.empty()) report_.sources[index].error = message;
            }
            const ContentRegistrationReport registered =
                register_declared_content(registry, declared, &report_.sources[index].entries);
            report_.base_registered += registered.registered;
            for (const ContentEntry& clash : registered.collisions) {
                const std::string message =
                    std::format("content: {} '{}' is registered twice in the game's own files",
                                content_kind_name(clash.kind), clash.name);
                report_.errors.push_back(message);
                // The file loaded and lost a name doing it: the list says PARTIAL, not OK.
                if (report_.sources[index].error.empty()) report_.sources[index].error = message;
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
            // A pack that does not parse is a pack: the list names it and says why it is not there.
            ContentSource source;
            source.kind = SourceKind::Pack;
            source.path = path;
            source.id = fs::path(path).stem().string();
            source.name = source.id;
            source.ok = false;
            source.error = std::move(error);
            report_.sources.push_back(std::move(source));
            continue;
        }
        parsed.push_back(std::move(*pack));
    }
    // Two packs with the same id: the second is refused, and says so.
    {
        std::vector<ContentPack> unique;
        for (ContentPack& pack : parsed) {
            const auto same = std::find_if(unique.begin(), unique.end(), [&](const ContentPack& other) {
                return other.id == pack.id;
            });
            if (same != unique.end()) {
                // The message names the pack that owns the id, which is not always the first one parsed.
                const std::string message = std::format("pack '{}' is defined twice ('{}' and '{}')",
                                                        pack.id, same->path, pack.path);
                report_.errors.push_back(message);
                ContentSource source;
                source.kind = SourceKind::Pack;
                source.path = pack.path;
                source.id = pack.id;
                source.name = pack.name;
                source.version = pack.version;
                source.requirements = pack.requirements;
                source.ok = false;
                source.error = message;
                report_.sources.push_back(std::move(source));
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
        // The pictures: a field of the designer's data the engine reads, because it has to be able to
        // draw what the data describes. Paths are relative to the pack, and a picture that is not there
        // (or is not something the engine can decode) is reported rather than drawn blank.
        for (ResolvedImage& image : resolve_content_images(pack.content, pack.path, kPackTable)) {
            if (!image.ok) {
                report_.errors.push_back(std::format("pack '{}': {} '{}': {}", pack.id,
                                                     content_kind_name(image.kind), image.content, image.error));
                if (pack.error.empty()) pack.error = image.error;
            } else {
                ++report_.pack_images;
            }
            report_.images.push_back(std::move(image));
        }
        // The other field the engine reads, read here so a typo in a pack is caught when the pack
        // loads rather than when a layer is built out of it - and kept, because building a layer needs
        // it.
        std::vector<std::string> definition_errors;
        for (types::TileDefinition& definition :
             types::tile_definitions(pack.content, &definition_errors, kPackTable)) {
            report_.definitions.add(std::move(definition));
        }
        for (const std::string& definition_error : definition_errors) {
            const std::string message = std::format("pack '{}': {}", pack.id, definition_error);
            report_.errors.push_back(message);
            if (pack.error.empty()) pack.error = message;
        }
        ++report_.packs;
        packs_.push_back(std::move(pack));
    }
    // The packs that got as far as being parsed, in the order the pipeline handled them.
    for (const ContentPack& pack : packs_) {
        ContentSource source;
        source.kind = SourceKind::Pack;
        source.path = pack.path;
        source.id = pack.id;
        source.name = pack.name;
        source.version = pack.version;
        source.requirements = pack.requirements;
        source.entries = pack.registered;
        // What it ships: counted from the load's own list, so a source's line and the atlas agree.
        for (const ResolvedImage& image : report_.images) {
            if (image.source != pack.path) continue;
            if (image.ok) ++source.images;
            else ++source.images_failed;
        }
        source.ok = pack.ok;
        source.error = pack.error;
        report_.sources.push_back(std::move(source));
    }

    // --- mod packages -----------------------------------------------------------------------------
    const ModLoadReport& mods = mods_.load(mod_directories_, registry);
    report_.mods = mods_.count();
    report_.native_mods = mods.native_modules;
    report_.mod_content = mods.content_registered;
    for (const std::string& error : mods.errors) report_.errors.push_back(error);
    for (const std::string& warning : mods.warnings) report_.warnings.push_back(warning);
    for (const LoadedMod& mod : mods.mods) {
        ContentSource source;
        source.kind = SourceKind::Mod;
        source.path = mod.manifest.directory;
        // A package whose manifest never parsed has no id: the folder it lives in is what it is called
        // until it has one, which is also what the designer sees in their own file browser.
        source.id = mod.manifest.id.empty() ? fs::path(mod.manifest.directory).filename().string()
                                            : mod.manifest.id;
        source.name = mod.manifest.name.empty() ? source.id : mod.manifest.name;
        source.version = mod.manifest.version;
        source.requirements = mod.manifest.requirements;
        // "Native" is about the package, not about how far it got: a mod whose library refused to
        // load is still a native mod that failed, which is what the list has to say.
        source.native = mod.native_loaded || !mod.manifest.native.empty();
        source.ok = mod.ok;
        source.error = mod.error;
        for (const ModContentEntry& entry : mod.registered) {
            source.entries.push_back(ContentEntry{entry.kind, entry.id, entry.name});
        }
        for (const ResolvedImage& image : mod.images) {
            if (image.ok) {
                ++source.images;
                ++report_.mod_images;
            } else {
                ++source.images_failed;
            }
            report_.images.push_back(image);
        }
        // The definitions the mod's content files declared: the same two engine fields as everywhere
        // else, so a layer can be built out of what a mod added exactly like out of the game's own
        // content.
        for (const types::TileDefinition& definition : mod.definitions) report_.definitions.add(definition);
        report_.sources.push_back(std::move(source));
    }

    report_.total_content = registry.total_count();
    T2D_INFO("content: {} base, {} pack(s) with {} and {} image(s), {} mod(s) with {} -> {} registered, "
             "{} plot definition(s), {} error(s)",
             report_.base_registered, report_.packs, report_.pack_content, report_.pack_images, report_.mods,
             report_.mod_content, report_.total_content, report_.definitions.size(), report_.errors.size());
    for (const std::string& error : report_.errors) T2D_WARN("content: {}", error);
    for (const std::string& warning : report_.warnings) T2D_WARN("content: {}", warning);
    return report_;
}

} // namespace mine
