#include <mine/content_pack.h>

#include <mine/types/tile_definition.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace mine {
namespace {

namespace fs = std::filesystem;

/// The keys a pack header may carry: the same ones a mod's manifest has, so the two forms of "a
/// directory of content" are read the same way.
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

/// True when \p directory carries a mod.ecfg: it is a **mod package**, so its files belong to the mod
/// host (mod_package.h), which loads them in the order its manifest gives. Loading them as a pack as
/// well would register every name in them twice, and the second registration is reported as a
/// collision - which is what dropping a pack and a mod into one directory (the game's own "packs"
/// directory, content_search.h) would produce.
[[nodiscard]] bool is_mod_package(const fs::path& directory) {
    std::error_code code;
    return fs::is_regular_file(directory / "mod.ecfg", code);
}

/// What reading one pack's files into the registry did.
struct PackFileResult {
    usize registered = 0;
    usize images = 0;
    usize images_failed = 0;
};

/// Reads every file of \p pack into \p registry: the names it declares, the pictures it names (each
/// resolved against the file that declares it, because that is what a relative path in the file
/// means), and the two fields the engine reads. Everything that goes wrong is reported against
/// \p label - "content" for the game's own content, "pack 'x'" for a pack - and marks the pack not-ok,
/// so the content list says so instead of showing a source that quietly lost something.
PackFileResult load_pack_files(ContentPack& pack, ContentRegistry& registry, ContentPipelineReport& report,
                               std::string_view label) {
    PackFileResult result;
    for (usize index = 0; index < pack.documents.size(); ++index) {
        const t2d::EcfgDocument& document = pack.documents[index];
        const std::string& file = pack.files[index];
        std::vector<std::string> unknown_tables;
        const std::vector<ContentEntry> declared = content_declarations(document, &unknown_tables);
        for (ResolvedImage& image : resolve_content_images(document, file)) {
            if (!image.ok) {
                const std::string message = std::format("{}: {} '{}': {}", label,
                                                        content_kind_name(image.kind), image.content,
                                                        image.error);
                report.errors.push_back(message);
                if (pack.error.empty()) pack.error = message;
                ++result.images_failed;
            } else {
                ++result.images;
            }
            report.images.push_back(std::move(image));
        }
        // The other field the engine reads: a "random_reverse" it cannot read is a data error, not a
        // plot that quietly never turns around. The definitions are what building a layer reads
        // (world.h), so they are kept rather than only checked.
        std::vector<std::string> definition_errors;
        for (types::TileDefinition& definition : types::tile_definitions(document, &definition_errors)) {
            report.definitions.add(std::move(definition));
        }
        for (const std::string& definition_error : definition_errors) {
            const std::string message = std::format("{}: {}", label, definition_error);
            report.errors.push_back(message);
            if (pack.error.empty()) pack.error = message;
        }
        std::vector<ContentEntry> added;
        const ContentRegistrationReport registered = register_declared_content(registry, declared, &added);
        result.registered += registered.registered;
        for (const ContentEntry& entry : added) pack.registered.push_back(entry);
        for (const ContentEntry& clash : registered.collisions) {
            const std::string message = std::format("{}: {} '{}' is already registered and was not replaced",
                                                    label, content_kind_name(clash.kind), clash.name);
            report.errors.push_back(message);
            if (pack.error.empty()) pack.error = message;
        }
        for (const std::string& table : unknown_tables) {
            report.warnings.push_back(std::format("{}: '{}' is not a content kind", label, table));
        }
    }
    return result;
}

} // namespace

std::vector<std::string> ContentPack::content_files(const std::string& directory) {
    std::vector<std::string> files;
    std::error_code code;
    const fs::path root(directory);
    if (!fs::is_directory(root, code)) return files;
    // Recursive, so a pack can hold its content in whatever files it likes - one per kind, one per
    // feature, or a content/ subdirectory. Entries that start with a dot are skipped, which keeps .git
    // and editor leftovers out of a load, and a mod package is not descended into.
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
            if (is_mod_package(path)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(entry_code)) continue;
        if (path.extension() != ".ecfg") continue;
        if (name == kPackHeaderName) continue;   // the header says who the pack is; it is not content
        files.push_back(path.string());
    }
    // Sorted, so the load order does not depend on what the filesystem happens to return first.
    std::sort(files.begin(), files.end());
    return files;
}

bool ContentPack::is_pack(const std::string& directory) {
    std::error_code code;
    const fs::path root(directory);
    if (!fs::is_directory(root, code)) return false;
    if (is_mod_package(root)) return false;
    if (fs::is_regular_file(root / kPackHeaderName, code)) return true;
    return !content_files(directory).empty();
}

std::vector<std::string> ContentPack::find_packs(const std::string& directory,
                                                 std::vector<std::string>* errors) {
    std::vector<std::string> found;
    std::error_code code;
    const fs::path root(directory);
    if (!fs::exists(root, code)) {
        if (errors != nullptr) errors->push_back(std::format("packs: '{}' does not exist", directory));
        return found;
    }
    if (!fs::is_directory(root, code)) {
        if (errors != nullptr) {
            errors->push_back(std::format("packs: '{}' is a file: a content pack is a directory holding its "
                                          ".ecfg files and its art",
                                          directory));
        }
        return found;
    }
    if (is_mod_package(root)) return found;   // a mod package is the mod host's business

    // The packs directly inside come first: a directory that holds packs is a **workspace**, not a pack
    // itself, and that is what makes "drop your pack folder into packs/" the whole installation story.
    std::vector<fs::path> children;
    for (const fs::directory_entry& entry : fs::directory_iterator(root, code)) {
        const std::string name = entry.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        if (!entry.is_directory(code)) continue;
        if (is_pack(entry.path().string())) children.push_back(entry.path());
    }
    std::sort(children.begin(), children.end());
    if (!children.empty()) {
        for (const fs::path& child : children) found.push_back(child.string());
        // A loose *.ecfg file at the top of a workspace is reported: a pack is a directory now, and a
        // file that is silently not loaded is content that silently disappeared.
        for (const fs::directory_entry& entry : fs::directory_iterator(root, code)) {
            if (!entry.is_regular_file(code)) continue;
            if (entry.path().extension() != ".ecfg") continue;
            if (errors != nullptr) {
                errors->push_back(std::format("packs: '{}' is a loose file: a pack is a directory holding "
                                              "its .ecfg files and its art",
                                              entry.path().string()));
            }
        }
        return found;
    }

    // Nothing that is a pack sits inside: the directory the caller named is the pack.
    if (is_pack(directory)) found.push_back(root.string());
    return found;
}

std::optional<ContentPack> ContentPack::load(const std::string& directory, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ContentPack> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    std::error_code code;
    const fs::path root(directory);
    if (!fs::is_directory(root, code)) {
        return fail(std::format("'{}' is not a directory: a content pack is a directory holding its .ecfg "
                                "files and its art, not a single file",
                                directory));
    }
    if (is_mod_package(root)) {
        return fail(std::format("'{}' is a mod package (it has a mod.ecfg): its content files are loaded by "
                                "the mod host, in the order its manifest gives",
                                directory));
    }

    ContentPack pack;
    pack.directory = directory;
    pack.id = root.filename().string();
    if (pack.id.empty()) return fail(std::format("'{}' has no name to be a pack under", directory));

    // The header, when there is one: a file of its own at the top of the pack.
    const fs::path header_path = root / kPackHeaderName;
    if (fs::is_regular_file(header_path, code)) {
        t2d::EcfgError parse_error;
        std::optional<t2d::EcfgDocument> header = t2d::EcfgDocument::load(header_path.string(), &parse_error);
        if (!header.has_value()) return fail(parse_error.describe(header_path.string()));
        pack.declared = true;
        for (const t2d::EcfgValue& entry : header->root().children()) {
            if (!is_pack_key(entry.key())) {
                return fail(std::format("{}: '{}' is not a pack key ({} takes id, name, version, requires)",
                                        header_path.string(), entry.key(), kPackHeaderName));
            }
        }
        if (const t2d::EcfgValue* id = header->find("id"); id != nullptr) {
            if (!id->is_string() || id->as_string().empty()) {
                return fail(std::format("{}: id must be a non empty string", header_path.string()));
            }
            pack.id = std::string(id->as_string());
        }
        if (const t2d::EcfgValue* name = header->find("name"); name != nullptr) {
            if (!name->is_string()) return fail(std::format("{}: name must be a string", header_path.string()));
            pack.name = std::string(name->as_string());
        }
        if (const t2d::EcfgValue* version = header->find("version"); version != nullptr) {
            if (!version->is_string()) {
                return fail(std::format("{}: version must be a string", header_path.string()));
            }
            pack.version = std::string(version->as_string());
        }
        if (const t2d::EcfgValue* requirements = header->find("requires"); requirements != nullptr) {
            pack.requirements = read_string_list(*requirements);
            if (pack.requirements.empty() && !requirements->is_array() && !requirements->is_string()) {
                return fail(std::format("{}: requires must be a name or a list of names", header_path.string()));
            }
        }
    }
    if (pack.name.empty()) pack.name = pack.id;

    pack.files = content_files(directory);
    if (pack.files.empty()) return fail(std::format("'{}' holds no .ecfg file", directory));
    for (const std::string& file : pack.files) {
        t2d::EcfgError parse_error;
        std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(file, &parse_error);
        if (!document.has_value()) return fail(parse_error.describe(file));
        // The header is a file of its own now: the "pack::" table that used to sit at the top of a
        // single file pack is told where it went, rather than being reported as an unknown content kind.
        if (document->find("pack") != nullptr) {
            return fail(std::format("{}: the pack header is '{}' now, not a 'pack::' table inside a content file",
                                    file, kPackHeaderName));
        }
        pack.documents.push_back(std::move(*document));
    }
    return pack;
}

void ContentPipeline::set_base_packs(std::vector<std::string> directories) {
    base_packs_ = std::move(directories);
}

void ContentPipeline::set_pack_directories(std::vector<std::string> directories) {
    pack_directories_ = std::move(directories);
}

void ContentPipeline::set_mod_directories(std::vector<std::string> directories) {
    mod_directories_ = std::move(directories);
}

bool ContentPipeline::empty() const {
    return base_packs_.empty() && pack_directories_.empty() && mod_directories_.empty();
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
    // The first stage, and the reason its ids never move: whatever it registers is registered before
    // anything else, so a pack can only append. It is a pack like any other - a directory of .ecfg
    // files and art - and one that is not there is a line on the list saying so.
    for (const std::string& directory : base_packs_) {
        std::string error;
        std::optional<ContentPack> base = ContentPack::load(directory, &error);
        if (!base.has_value()) {
            report_.errors.push_back(error);
            ContentSource source;
            source.kind = SourceKind::File;
            source.path = directory;
            source.id = fs::path(directory).filename().string();
            source.name = source.id;
            source.ok = false;
            source.error = std::move(error);
            report_.sources.push_back(std::move(source));
            continue;
        }
        const PackFileResult result = load_pack_files(*base, registry, report_, "content");
        report_.base_files += base->files.size();
        report_.base_registered += result.registered;
        report_.base_images += result.images;
        ++report_.base_packs;
        ContentSource source;
        source.kind = SourceKind::File;
        source.path = base->directory;
        source.id = base->id;
        source.name = base->name;
        source.version = base->version;
        source.requirements = base->requirements;
        source.entries = base->registered;
        source.images = result.images;
        source.images_failed = result.images_failed;
        source.ok = base->ok;
        source.error = base->error;
        report_.sources.push_back(std::move(source));
    }

    // --- content packs ----------------------------------------------------------------------------
    std::vector<std::string> pack_directories;
    for (const std::string& directory : pack_directories_) {
        const std::vector<std::string> found = ContentPack::find_packs(directory, &report_.errors);
        pack_directories.insert(pack_directories.end(), found.begin(), found.end());
    }
    std::vector<ContentPack> parsed;
    for (const std::string& directory : pack_directories) {
        std::string error;
        std::optional<ContentPack> pack = ContentPack::load(directory, &error);
        if (!pack.has_value()) {
            // A pack that does not read is a pack: the list names it and says why it is not there.
            report_.errors.push_back(error);
            ContentSource source;
            source.kind = SourceKind::Pack;
            source.path = directory;
            source.id = fs::path(directory).filename().string();
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
                                                        pack.id, same->directory, pack.directory);
                report_.errors.push_back(message);
                ContentSource source;
                source.kind = SourceKind::Pack;
                source.path = pack.directory;
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
        const PackFileResult result =
            load_pack_files(pack, registry, report_, std::format("pack '{}'", pack.id));
        report_.pack_content += result.registered;
        report_.pack_images += result.images;
        pack.images = result.images;
        pack.images_failed = result.images_failed;
        ++report_.packs;
        packs_.push_back(std::move(pack));
    }
    // The packs that got as far as being read, in the order the pipeline handled them.
    for (const ContentPack& pack : packs_) {
        ContentSource source;
        source.kind = SourceKind::Pack;
        source.path = pack.directory;
        source.id = pack.id;
        source.name = pack.name;
        source.version = pack.version;
        source.requirements = pack.requirements;
        source.entries = pack.registered;
        source.images = pack.images;
        source.images_failed = pack.images_failed;
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
    T2D_INFO("content: {} base pack(s) with {} and {} image(s), {} pack(s) with {} and {} image(s), "
             "{} mod(s) with {} -> {} registered, {} plot definition(s), {} error(s)",
             report_.base_packs, report_.base_registered, report_.base_images, report_.packs,
             report_.pack_content, report_.pack_images, report_.mods, report_.mod_content,
             report_.total_content, report_.definitions.size(), report_.errors.size());
    for (const std::string& error : report_.errors) T2D_WARN("content: {}", error);
    for (const std::string& warning : report_.warnings) T2D_WARN("content: {}", warning);
    return report_;
}

} // namespace mine
