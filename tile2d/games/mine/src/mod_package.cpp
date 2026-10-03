#include <mine/mod_package.h>

#include <mine/content_loader.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <map>

namespace mine {
namespace {

namespace fs = std::filesystem;

/// The keys a manifest may use. Anything else is refused: a typo in a manifest is a mod that silently
/// does not do what its author meant.
constexpr const char* kManifestKeys[] = {"id",    "name",     "version", "description",
                                         "api",   "requires", "content", "native",
                                         // "data" is the mod's own namespace: the host parses it and
                                         // hands it back through mod_value()/mod_int(), and does not
                                         // care what is inside.
                                         "data"};

[[nodiscard]] bool is_known_key(std::string_view key) {
    for (const char* known : kManifestKeys) {
        if (key == known) return true;
    }
    return false;
}

/// A string, or an array of strings: "requires:"a"" and "requires:["a","b"]" both read naturally.
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

/// Mod ids end up in log lines, in "requires" lists and in content names one day, so they are kept to
/// a character set that needs no quoting anywhere.
[[nodiscard]] bool is_valid_id(std::string_view id) {
    if (id.empty()) return false;
    for (const char character : id) {
        const bool ok = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                        (character >= '0' && character <= '9') || character == '_' || character == '-' ||
                        character == '.';
        if (!ok) return false;
    }
    return true;
}

// --- the C ABI's thunks ---------------------------------------------------------------------------
//
// A module is handed a ModContext: the host, and which of its mods is calling. Every function below is
// a plain C function so its address can live in MineModApi, and every one of them finds the host
// through that context - which is what keeps the host free of globals.

[[nodiscard]] ModHost* host_of(void* self) {
    auto* context = static_cast<ModContext*>(self);
    return context != nullptr ? static_cast<ModHost*>(context->host) : nullptr;
}

void mod_log(void* self, u32 level, const char* message) {
    if (ModHost* host = host_of(self); host != nullptr) host->api_log(self, level, message);
}

ContentId mod_register_content(void* self, u32 kind, const char* name) {
    ModHost* host = host_of(self);
    return host != nullptr ? host->api_register_content(self, kind, name) : kNoContent;
}

ContentId mod_find_content(void* self, u32 kind, const char* name) {
    ModHost* host = host_of(self);
    return host != nullptr ? host->api_find_content(self, kind, name) : kNoContent;
}

const char* mod_content_name(void* self, u32 kind, ContentId id) {
    ModHost* host = host_of(self);
    return host != nullptr ? host->api_content_name(self, kind, id) : nullptr;
}

const char* mod_value(void* self, const char* key) {
    ModHost* host = host_of(self);
    return host != nullptr ? host->api_mod_value(self, key) : nullptr;
}

i64 mod_int(void* self, const char* key, i64 fallback) {
    ModHost* host = host_of(self);
    return host != nullptr ? host->api_mod_int(self, key, fallback) : fallback;
}

} // namespace

// --- the manifest ---------------------------------------------------------------------------------

std::optional<ModManifest> ModManifest::from_document(const t2d::EcfgDocument& document,
                                                      const std::string& directory, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ModManifest> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };

    ModManifest manifest;
    manifest.directory = directory;
    const t2d::EcfgValue& root = document.root();
    for (const t2d::EcfgValue& entry : root.children()) {
        if (!is_known_key(entry.key())) {
            return fail(std::format("{}: '{}' is not a manifest key", document.source_name(), entry.key()));
        }
    }

    const t2d::EcfgValue* id = root.find("id");
    if (id == nullptr || !id->is_string() || id->as_string().empty()) {
        return fail(std::format("{}: a manifest needs a non empty id", document.source_name()));
    }
    manifest.id = std::string(id->as_string());
    if (!is_valid_id(manifest.id)) {
        return fail(std::format("{}: id '{}' may only use letters, digits, '_', '-' and '.'",
                                document.source_name(), manifest.id));
    }

    if (const t2d::EcfgValue* name = root.find("name"); name != nullptr) {
        if (!name->is_string()) return fail(std::format("{}: name must be a string", document.source_name()));
        manifest.name = std::string(name->as_string());
    }
    if (manifest.name.empty()) manifest.name = manifest.id;
    if (const t2d::EcfgValue* version = root.find("version"); version != nullptr) {
        if (!version->is_string()) {
            return fail(std::format("{}: version must be a string", document.source_name()));
        }
        manifest.version = std::string(version->as_string());
    }
    if (const t2d::EcfgValue* description = root.find("description"); description != nullptr) {
        if (!description->is_string()) {
            return fail(std::format("{}: description must be a string", document.source_name()));
        }
        manifest.description = std::string(description->as_string());
    }
    if (const t2d::EcfgValue* native = root.find("native"); native != nullptr) {
        if (!native->is_string()) {
            return fail(std::format("{}: native must be a file name", document.source_name()));
        }
        manifest.native = std::string(native->as_string());
    }
    if (const t2d::EcfgValue* api = root.find("api"); api != nullptr) {
        const i64 value = api->as_int(-1);
        if (value < 0) return fail(std::format("{}: api must be a positive number", document.source_name()));
        manifest.api = static_cast<u32>(value);
    }
    if (const t2d::EcfgValue* listed = root.find("requires"); listed != nullptr) {
        manifest.requirements = read_string_list(*listed);
        if (manifest.requirements.empty() && !listed->is_array() && !listed->is_string()) {
            return fail(std::format("{}: requires must be a name or a list of names", document.source_name()));
        }
    }
    if (const t2d::EcfgValue* data = root.find("data"); data != nullptr) {
        if (!data->is_table()) {
            return fail(std::format("{}: data must be a table", document.source_name()));
        }
    }
    if (const t2d::EcfgValue* content = root.find("content"); content != nullptr) {
        manifest.content = read_string_list(*content);
        if (manifest.content.empty()) {
            return fail(std::format("{}: content must be a file name or a list of file names",
                                    document.source_name()));
        }
    }

    // A native mod has to say which ABI it was built against; the host cannot guess, and calling a
    // module through a layout it does not have is the one failure that cannot be reported politely.
    if (!manifest.native.empty() && manifest.api == 0) {
        return fail(std::format("{}: a native mod needs api:{}", document.source_name(), kModApiVersion));
    }
    if (manifest.native.empty() && manifest.api != 0) {
        T2D_WARN("mod '{}': api is only meaningful with native, ignoring it", manifest.id);
        manifest.api = 0;
    }
    if (manifest.native.empty() && manifest.content.empty()) {
        T2D_WARN("mod '{}': nothing to load (no content files and no native library)", manifest.id);
    }
    return manifest;
}

std::optional<ModManifest> ModManifest::parse_file(const std::string& path, std::string* error) {
    t2d::EcfgError parse_error;
    const std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &parse_error);
    if (!document.has_value()) {
        if (error != nullptr) *error = parse_error.describe(path);
        return std::nullopt;
    }
    return from_document(*document, fs::path(path).parent_path().string(), error);
}

// --- the host -------------------------------------------------------------------------------------

ModHost::ModHost() {
    api_.abi_version = kModApiVersion;
    api_.struct_size = sizeof(MineModApi);
    api_.log = &mod_log;
    api_.register_content = &mod_register_content;
    api_.find_content = &mod_find_content;
    api_.content_name = &mod_content_name;
    api_.mod_value = &mod_value;
    api_.mod_int = &mod_int;
}

ModHost::~ModHost() { unload(); }

usize ModHost::count() const {
    usize running = 0;
    for (const Slot& slot : slots_) {
        if (slot.loaded.ok) ++running;
    }
    return running;
}

usize ModHost::native_count() const {
    usize count = 0;
    for (const Slot& slot : slots_) {
        if (slot.module.has_value()) ++count;
    }
    return count;
}

const ModLoadReport& ModHost::load(const std::vector<std::string>& directories, ContentRegistry& registry) {
    unload();
    registry_ = &registry;
    report_ = ModLoadReport{};

    // --- find the packages ------------------------------------------------------------------------
    struct Found {
        ModManifest manifest;
        t2d::EcfgDocument document;
    };
    std::vector<Found> found;
    std::map<std::string, usize> by_id;
    for (const std::string& directory : directories) {
        std::error_code code;
        const fs::path root(directory);
        if (!fs::exists(root, code)) {
            report_.errors.push_back(std::format("mods: '{}' does not exist", directory));
            continue;
        }
        std::vector<fs::path> manifests;
        if (fs::is_regular_file(root / "mod.ecfg", code)) {
            manifests.push_back(root / "mod.ecfg");   // the directory *is* a package
        } else {
            for (const fs::directory_entry& entry : fs::directory_iterator(root, code)) {
                if (entry.is_directory(code) && fs::exists(entry.path() / "mod.ecfg", code)) {
                    manifests.push_back(entry.path() / "mod.ecfg");
                }
            }
        }
        // Sorted, so the load order does not depend on what the filesystem happens to return first.
        std::sort(manifests.begin(), manifests.end());
        for (const fs::path& manifest_path : manifests) {
            t2d::EcfgError parse_error;
            std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(manifest_path.string(), &parse_error);
            if (!document.has_value()) {
                report_.errors.push_back(parse_error.describe(manifest_path.string()));
                continue;
            }
            std::string error;
            std::optional<ModManifest> manifest =
                ModManifest::from_document(*document, manifest_path.parent_path().string(), &error);
            if (!manifest.has_value()) {
                report_.errors.push_back(error);
                continue;
            }
            if (by_id.find(manifest->id) != by_id.end()) {
                report_.errors.push_back(
                    std::format("mods: '{}' is defined twice ('{}' and '{}')", manifest->id,
                                found[by_id[manifest->id]].manifest.directory, manifest->directory));
                continue;
            }
            by_id.emplace(manifest->id, found.size());
            found.push_back(Found{std::move(*manifest), std::move(*document)});
        }
    }

    // --- check the dependencies, and order what is left -------------------------------------------
    std::vector<usize> order;
    std::vector<bool> placed(found.size(), false);
    std::vector<bool> dropped(found.size(), false);
    for (usize index = 0; index < found.size(); ++index) {
        for (const std::string& need : found[index].manifest.requirements) {
            if (by_id.find(need) == by_id.end()) {
                const std::string message = std::format("requires '{}', which is not installed", need);
                report_.errors.push_back(std::format("mod '{}': {}", found[index].manifest.id, message));
                dropped[index] = true;
                LoadedMod failed;
                failed.manifest = found[index].manifest;
                failed.ok = false;
                failed.error = message;
                slots_.push_back(Slot{std::move(failed), std::nullopt, std::nullopt, nullptr});
                break;
            }
        }
    }
    while (order.size() + static_cast<usize>(std::count(dropped.begin(), dropped.end(), true)) <
           found.size()) {
        bool progress = false;
        for (usize index = 0; index < found.size(); ++index) {
            if (placed[index] || dropped[index]) continue;
            bool ready = true;
            for (const std::string& need : found[index].manifest.requirements) {
                const auto found_need = by_id.find(need);
                if (found_need == by_id.end() || !placed[found_need->second]) {
                    ready = false;
                    break;
                }
            }
            if (!ready) continue;
            placed[index] = true;
            order.push_back(index);
            progress = true;
        }
        if (!progress) break;   // everything left is waiting for something that is waiting for it
    }
    for (usize index = 0; index < found.size(); ++index) {
        if (placed[index] || dropped[index]) continue;
        report_.errors.push_back(std::format("mod '{}': its requirements form a cycle, not loaded",
                                             found[index].manifest.id));
        LoadedMod failed;
        failed.manifest = found[index].manifest;
        failed.ok = false;
        failed.error = "its requirements form a cycle";
        slots_.push_back(Slot{std::move(failed), std::nullopt, std::nullopt, nullptr});
    }

    // --- load, in order ---------------------------------------------------------------------------
    slots_.reserve(slots_.size() + order.size());
    for (const usize index : order) {
        Found& item = found[index];
        const ModManifest& manifest = item.manifest;
        slots_.push_back(Slot{});
        Slot& slot = slots_.back();
        slot.loaded.manifest = manifest;
        slot.document = std::move(item.document);

        // Data first: parse every file before registering anything, so a package whose second file is
        // broken does not half load.
        std::vector<t2d::EcfgDocument> documents;
        bool parsed = true;
        for (const std::string& relative : manifest.content) {
            const fs::path file = fs::path(manifest.directory) / relative;
            std::error_code code;
            if (!fs::exists(file, code)) {
                slot.loaded.ok = false;
                slot.loaded.error = std::format("content file '{}' is missing", file.string());
                parsed = false;
                break;
            }
            t2d::EcfgError parse_error;
            std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(file.string(), &parse_error);
            if (!document.has_value()) {
                slot.loaded.ok = false;
                slot.loaded.error = parse_error.describe(file.string());
                parsed = false;
                break;
            }
            slot.loaded.content_paths.push_back(file.string());
            documents.push_back(std::move(*document));
        }
        if (!parsed) {
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        for (const t2d::EcfgDocument& document : documents) {
            std::vector<std::string> unknown_tables;
            const std::vector<ContentEntry> declared = content_declarations(document, &unknown_tables);
            std::vector<ContentEntry> added;
            const ContentRegistrationReport registered = register_declared_content(registry, declared, &added);
            for (const ContentEntry& entry : added) {
                slot.loaded.registered.push_back(ModContentEntry{entry.kind, entry.id, entry.name});
            }
            report_.content_registered += registered.registered;
            for (const ContentEntry& clash : registered.collisions) {
                // Taken by the game's own content, by an earlier mod, or by an earlier file of this
                // mod: all three are reported, and none of them is merged silently.
                const std::string message =
                    std::format("mod '{}': {} '{}' is already registered and was not replaced", manifest.id,
                                content_kind_name(clash.kind), clash.name);
                report_.errors.push_back(message);
                if (slot.loaded.error.empty()) slot.loaded.error = message;
            }
            for (const std::string& table : unknown_tables) {
                report_.warnings.push_back(std::format("mod '{}': '{}' is not a content kind", manifest.id, table));
            }
        }

        // Then the code, if there is any.
        if (manifest.native.empty()) continue;
        const fs::path library = fs::path(manifest.directory) / manifest.native;
        std::error_code code;
        if (!fs::exists(library, code)) {
            slot.loaded.ok = false;
            slot.loaded.error = std::format("native library '{}' is missing", library.string());
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        std::string load_error;
        std::optional<t2d::Module> module = t2d::Module::load(library.string(), &load_error);
        if (!module.has_value() || !module->valid()) {
            slot.loaded.ok = false;
            slot.loaded.error = std::format("'{}' could not be loaded: {}", library.string(), load_error);
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        const auto entry = module->function<const MineModDesc*()>(kModEntrySymbol);
        if (entry == nullptr) {
            slot.loaded.ok = false;
            slot.loaded.error = std::format("'{}' does not export {}", library.string(), kModEntrySymbol);
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        const MineModDesc* desc = entry();
        if (desc == nullptr) {
            slot.loaded.ok = false;
            slot.loaded.error = std::format("{}() returned nothing", kModEntrySymbol);
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        if (desc->abi_version != kModApiVersion) {
            slot.loaded.ok = false;
            slot.loaded.error = std::format("built against mod API {}, this build is {} (rebuild the mod)",
                                            desc->abi_version, kModApiVersion);
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        if (desc->struct_size < sizeof(MineModDesc)) {
            slot.loaded.ok = false;
            slot.loaded.error = std::format("its descriptor is {} bytes, this build's is {} (rebuild the mod)",
                                            desc->struct_size, sizeof(MineModDesc));
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        if (desc->id == nullptr || manifest.id != desc->id) {
            slot.loaded.ok = false;
            slot.loaded.error = std::format("the library calls itself '{}', the manifest says '{}'",
                                            desc->id != nullptr ? desc->id : "(nothing)", manifest.id);
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }

        slot.module = std::move(module);
        slot.desc = desc;
        const ModContext context{this, slots_.size() - 1};
        const int result = desc->on_load != nullptr ? desc->on_load(&api_, const_cast<ModContext*>(&context)) : 0;
        if (result != 0) {
            // The module's own on_load refused to run: close it again, and keep nothing of it.
            slot.desc = nullptr;
            slot.module.reset();
            slot.loaded.ok = false;
            slot.loaded.error = std::format("on_load() returned {}", result);
            report_.errors.push_back(std::format("mod '{}': {}", manifest.id, slot.loaded.error));
            continue;
        }
        slot.loaded.native_loaded = true;
        ++report_.native_modules;
    }

    report_.mods.reserve(slots_.size());
    for (const Slot& slot : slots_) report_.mods.push_back(slot.loaded);
    T2D_INFO("mods: {} of {} loaded ({} native), {} content registered, {} error(s)", count(),
             report_.mods.size(), report_.native_modules, report_.content_registered, report_.errors.size());
    for (const std::string& error : report_.errors) T2D_WARN("mods: {}", error);
    for (const std::string& warning : report_.warnings) T2D_WARN("mods: {}", warning);
    return report_;
}

void ModHost::unload() {
    // Reverse order, and every module is told before its library goes away: a module that still has
    // something to release gets the chance, and nothing runs after its code is gone.
    for (usize index = slots_.size(); index > 0; --index) {
        Slot& slot = slots_[index - 1];
        if (slot.desc != nullptr && slot.desc->on_unload != nullptr) {
            ModContext context{this, index - 1};
            slot.desc->on_unload(&context);
        }
        slot.desc = nullptr;
        slot.module.reset();
    }
    slots_.clear();
    registry_ = nullptr;
}

ModHost::Slot* ModHost::slot_from(void* self) {
    auto* context = static_cast<ModContext*>(self);
    if (context == nullptr || context->host != this) return nullptr;
    if (context->slot >= slots_.size()) return nullptr;
    return &slots_[context->slot];
}

const char* ModHost::render_value(const t2d::EcfgValue& value) {
    switch (value.type()) {
        case t2d::EcfgType::String: scratch_ = std::string(value.as_string()); break;
        case t2d::EcfgType::Int: scratch_ = std::format("{}", value.as_int()); break;
        case t2d::EcfgType::Float: scratch_ = std::format("{}", value.as_float()); break;
        case t2d::EcfgType::Bool: scratch_ = value.as_bool() ? "true" : "false"; break;
        default: return nullptr;   // a table or an array has no text form
    }
    return scratch_.c_str();
}

void ModHost::api_log(void* self, u32 level, const char* message) {
    const Slot* slot = slot_from(self);
    const std::string text = std::format("mod '{}': {}", slot != nullptr ? slot->loaded.manifest.id : "?",
                                         message != nullptr ? message : "");
    if (level >= 2) T2D_ERROR("{}", text);
    else if (level == 1) T2D_WARN("{}", text);
    else T2D_INFO("{}", text);
}

ContentId ModHost::api_register_content(void* self, u32 kind, const char* name) {
    Slot* slot = slot_from(self);
    if (slot == nullptr || registry_ == nullptr) return kNoContent;
    if (kind >= kContentKindCount) {
        report_.errors.push_back(std::format("mod '{}': content kind {} does not exist",
                                             slot->loaded.manifest.id, kind));
        return kNoContent;
    }
    if (name == nullptr || name[0] == '\0') {
        report_.errors.push_back(
            std::format("mod '{}': tried to register an empty name", slot->loaded.manifest.id));
        return kNoContent;
    }
    const auto content_kind = static_cast<ContentKind>(kind);
    if (registry_->find(content_kind, name) != kNoContent) {
        report_.errors.push_back(std::format("mod '{}': {} '{}' is already registered and was not replaced",
                                             slot->loaded.manifest.id, content_kind_name(content_kind), name));
        return kNoContent;
    }
    const ContentId id = registry_->register_content(content_kind, name);
    if (id == kNoContent) return kNoContent;
    slot->loaded.registered.push_back(ModContentEntry{content_kind, id, name});
    ++report_.content_registered;
    return id;
}

ContentId ModHost::api_find_content(void* self, u32 kind, const char* name) {
    if (registry_ == nullptr || kind >= kContentKindCount || name == nullptr) return kNoContent;
    return registry_->find(static_cast<ContentKind>(kind), name);
}

const char* ModHost::api_content_name(void* self, u32 kind, ContentId id) {
    if (registry_ == nullptr || kind >= kContentKindCount) return nullptr;
    const ContentEntry* entry = registry_->find(static_cast<ContentKind>(kind), id);
    if (entry == nullptr) return nullptr;
    scratch_ = entry->name;
    return scratch_.c_str();
}

const t2d::EcfgValue* ModHost::mod_value_of(Slot& slot, const char* key) {
    if (!slot.document.has_value() || key == nullptr) return nullptr;
    const t2d::EcfgValue& root = slot.document->root();
    // The mod's own namespace first: "data::" is free form, so a parameter named "version" still
    // means the mod's parameter and not the manifest's field.
    if (const t2d::EcfgValue* data = root.find("data"); data != nullptr) {
        if (const t2d::EcfgValue* value = data->find(key); value != nullptr) return value;
    }
    return root.find(key);
}

const char* ModHost::api_mod_value(void* self, const char* key) {
    Slot* slot = slot_from(self);
    if (slot == nullptr) return nullptr;
    const t2d::EcfgValue* value = mod_value_of(*slot, key);
    if (value == nullptr) return nullptr;
    return render_value(*value);
}

i64 ModHost::api_mod_int(void* self, const char* key, i64 fallback) {
    Slot* slot = slot_from(self);
    if (slot == nullptr) return fallback;
    const t2d::EcfgValue* value = mod_value_of(*slot, key);
    if (value == nullptr) return fallback;
    if (value->type() == t2d::EcfgType::Int || value->type() == t2d::EcfgType::Bool) return value->as_int(fallback);
    if (value->type() == t2d::EcfgType::Float) return static_cast<i64>(value->as_float(static_cast<t2d::f64>(fallback)));
    if (value->type() == t2d::EcfgType::String) {
        const std::string text(value->as_string());
        try {
            return std::stoll(text);
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

} // namespace mine
