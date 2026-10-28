#include <mine/save_library.h>

#include <t2d/text/utf8.h>

#include <algorithm>
#include <array>

namespace mine {
namespace {

using t2d::Codepoint;

/// The names Windows does not let a directory have, compared without regard to case before the first dot:
/// "con", "Con.txt" and "CON" are the same reserved name.
constexpr std::array<std::string_view, 22> kReservedNames = {
    "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
    "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
};

[[nodiscard]] char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] bool equals_ignoring_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (usize index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != right[index]) return false;
    }
    return true;
}

/// True for the characters a directory name cannot hold, and for the controls that would corrupt one.
/// Tab is allowed: the name field inserts one, and a file system takes it (section 1.49).
[[nodiscard]] bool is_forbidden(t2d::u32 codepoint) {
    if ((codepoint < 0x20u && codepoint != 0x09u) || codepoint == 0x7Fu) return true;
    switch (codepoint) {
        case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|':
            return true;
        default:
            return false;
    }
}

/// The name before the first dot, which is what Windows reserves: "con.txt" is CON.
[[nodiscard]] bool is_reserved_name(std::string_view name) {
    const std::string_view stem = name.substr(0, name.find('.'));
    for (const std::string_view reserved : kReservedNames) {
        if (equals_ignoring_case(stem, reserved)) return true;
    }
    return false;
}

} // namespace

const char* save_name_error_id(std::string_view name) {
    if (name.empty()) return "save.name.empty";
    usize codepoints = 0;
    for (usize index = 0; index < name.size();) {
        const Codepoint point = t2d::utf8_decode(name, index);
        if (point.size == 0) break;
        // A malformed byte is U+FFFD with size 1; a real U+FFFD is three bytes, and a player may have one.
        if (point.value == 0xFFFDu && point.size == 1u) return "save.name.invalid";
        if (is_forbidden(point.value)) return "save.name.invalid";
        ++codepoints;
        index += point.size;
    }
    if (codepoints > kMaxSaveNameCodepoints || name.size() > kMaxSaveNameBytes) return "save.name.long";
    if (name.front() == ' ' || name.back() == ' ') return "save.name.invalid";
    if (name.back() == '.') return "save.name.invalid";
    if (name == "." || name == "..") return "save.name.invalid";
    if (is_reserved_name(name)) return "save.name.invalid";
    return "";
}

SaveLibrary::SaveLibrary(std::string root) : root_(std::move(root)) {}

std::string SaveLibrary::slot_directory(std::string_view name) const {
    return (std::filesystem::path(root_) / std::string(name)).string();
}

std::string SaveLibrary::autosave_path(std::string_view name) const {
    return (std::filesystem::path(slot_directory(name)) / std::string(kAutosaveFileName)).string();
}

std::optional<SaveSlot> SaveLibrary::slot_at(std::string_view name) const {
    if (name.empty()) return std::nullopt;
    SaveSlot slot;
    slot.name = std::string(name);
    slot.directory = slot_directory(name);
    slot.autosave_path = autosave_path(name);
    std::error_code code;
    if (std::filesystem::is_regular_file(slot.autosave_path, code)) {
        const std::filesystem::file_time_type written =
            std::filesystem::last_write_time(slot.autosave_path, code);
        if (!code) {
            slot.has_autosave = true;
            slot.modified = written;
        }
    }
    return slot;
}

std::vector<SaveSlot> SaveLibrary::slots() const {
    std::vector<SaveSlot> out;
    std::error_code code;
    std::filesystem::directory_iterator entries(std::filesystem::path(root_), code);
    if (code) return out;
    for (const std::filesystem::directory_entry& entry : entries) {
        std::error_code kind;
        if (!entry.is_directory(kind)) continue;
        std::optional<SaveSlot> slot = slot_at(entry.path().filename().string());
        if (slot.has_value()) out.push_back(std::move(*slot));
    }
    // Newest automatic save first; slots that hold nothing after them, by name: a name is what a reader
    // can go by when there is no time to compare.
    std::sort(out.begin(), out.end(), [](const SaveSlot& left, const SaveSlot& right) {
        if (left.has_autosave != right.has_autosave) return left.has_autosave;
        if (left.has_autosave && left.modified != right.modified) return left.modified > right.modified;
        return left.name < right.name;
    });
    return out;
}

std::optional<SaveSlot> SaveLibrary::latest_autosave() const {
    for (const SaveSlot& slot : slots()) {
        if (slot.has_autosave) return slot;
    }
    return std::nullopt;
}

std::optional<SaveSlot> SaveLibrary::find(std::string_view name) const {
    std::error_code code;
    if (!std::filesystem::is_directory(slot_directory(name), code)) return std::nullopt;
    return slot_at(name);
}

bool SaveLibrary::create(std::string_view name, const char*& error_id, SaveSlot& out) {
    error_id = "";
    if (const char* problem = save_name_error_id(name); *problem != '\0') {
        error_id = problem;
        return false;
    }
    const std::filesystem::path directory(slot_directory(name));
    std::error_code code;
    if (std::filesystem::exists(directory, code)) {
        error_id = "save.name.exists";
        return false;
    }
    code.clear();
    if (!std::filesystem::create_directories(directory, code) || code) {
        error_id = "save.name.failed";
        return false;
    }
    out.name = std::string(name);
    out.directory = directory.string();
    out.autosave_path = autosave_path(name);
    out.modified = std::filesystem::file_time_type{};
    out.has_autosave = false;
    return true;
}

} // namespace mine
