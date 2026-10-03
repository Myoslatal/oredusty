#include <t2d/text/locale.h>

#include <t2d/core/log.h>

#include <algorithm>

namespace t2d {
namespace {

struct LanguageNames {
    const char* code;
    const char* name;
};

constexpr LanguageNames kNames[kLanguageCount] = {
    {"en", "English"},
    {"zh-Hans", "简体中文"},
    {"zh-Hant", "繁體中文"},
};

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    });
    return out;
}

} // namespace

const char* language_code(Language language) {
    const usize index = static_cast<usize>(language);
    return index < kLanguageCount ? kNames[index].code : "?";
}

const char* language_name(Language language) {
    const usize index = static_cast<usize>(language);
    return index < kLanguageCount ? kNames[index].name : "?";
}

std::optional<Language> parse_language(std::string_view text) {
    std::string key = lower(text);
    std::replace(key.begin(), key.end(), '_', '-');
    if (key.empty()) return std::nullopt;
    if (key == "en" || key == "eng" || key == "english") return Language::English;
    if (key == "zh" || key == "zh-hans" || key == "zh-cn" || key == "zh-sg" || key == "chs" ||
        key == "simplified" || key == "chinese") {
        return Language::SimplifiedChinese;
    }
    if (key == "zh-hant" || key == "zh-tw" || key == "zh-hk" || key == "zh-mo" || key == "cht" ||
        key == "traditional") {
        return Language::TraditionalChinese;
    }
    // A bare language tag with extra subtags still matches its base ("zh-Hans-CN").
    const usize dash = key.find('-');
    if (dash != std::string::npos) return parse_language(key.substr(0, dash));
    return std::nullopt;
}

bool is_chinese(Language language) {
    return language == Language::SimplifiedChinese || language == Language::TraditionalChinese;
}

void StringTable::set(Language language, std::string_view id, std::string_view text) {
    const usize index = static_cast<usize>(language);
    if (index >= kLanguageCount || id.empty()) return;
    for (auto& entry : tables_[index]) {
        if (entry.first == id) {
            entry.second = std::string(text);
            return;
        }
    }
    tables_[index].emplace_back(std::string(id), std::string(text));
}

std::string_view StringTable::find(Language language, std::string_view id) const {
    const usize index = static_cast<usize>(language);
    if (index >= kLanguageCount) return {};
    for (const auto& entry : tables_[index]) {
        if (entry.first == id) return entry.second;
    }
    return {};
}

usize StringTable::count(Language language) const {
    const usize index = static_cast<usize>(language);
    return index < kLanguageCount ? tables_[index].size() : 0;
}

std::vector<std::string_view> StringTable::ids(Language language) const {
    std::vector<std::string_view> out;
    const usize index = static_cast<usize>(language);
    if (index >= kLanguageCount) return out;
    out.reserve(tables_[index].size());
    for (const auto& entry : tables_[index]) out.push_back(entry.first);
    return out;
}

bool StringTable::empty() const {
    for (const auto& table : tables_) {
        if (!table.empty()) return false;
    }
    return true;
}

Locale Locale::from_ecfg(const EcfgDocument& document, std::vector<std::string>* unknown_tables) {
    Locale locale;
    for (const EcfgValue& table : document.root().children()) {
        const std::optional<Language> language = parse_language(table.key());
        if (!language.has_value()) {
            if (unknown_tables != nullptr) unknown_tables->emplace_back(table.key());
            continue;
        }
        if (!table.is_table()) {
            T2D_WARN("locale: '{}' should be a table of string ids", table.key());
            continue;
        }
        for (const EcfgValue& entry : table.children()) {
            const std::string_view value = entry.as_string();
            if (value.empty() && !entry.is_string()) {
                T2D_WARN("locale: '{}' in '{}' is not a string", entry.key(), table.key());
                continue;
            }
            locale.strings_.set(*language, entry.key(), value);
        }
    }
    if (unknown_tables != nullptr && !unknown_tables->empty()) {
        T2D_WARN("locale: {} table(s) are not languages (first: '{}')", unknown_tables->size(),
                 unknown_tables->front());
    }
    return locale;
}

Locale Locale::from_ecfg_text(std::string_view text, EcfgError* error, std::vector<std::string>* unknown_tables) {
    const std::optional<EcfgDocument> document = EcfgDocument::parse(text, error);
    if (!document.has_value()) return Locale{};
    return from_ecfg(*document, unknown_tables);
}

std::string_view Locale::text(std::string_view id, bool& translated) const {
    const std::string_view active = strings_.find(language_, id);
    if (!active.empty()) {
        translated = true;
        return active;
    }
    const std::string_view fallback = strings_.find(Language::English, id);
    translated = false;
    if (!fallback.empty()) return fallback;
    // Nothing anywhere: hand back the id so a missing string is visible in the interface instead of
    // showing up as blank space.
    return id;
}

std::string_view Locale::text(std::string_view id) const {
    bool translated = false;
    return text(id, translated);
}

std::vector<std::string> Locale::missing_for_active_language() const {
    std::vector<std::string> missing;
    if (language_ == Language::English) return missing;
    for (const std::string_view id : strings_.ids(Language::English)) {
        if (strings_.find(language_, id).empty()) missing.emplace_back(id);
    }
    return missing;
}

} // namespace t2d
