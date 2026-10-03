// Tile2D - languages and the string tables the interface reads its text from.
//
// The engine ships no prose: every string the game shows is looked up by id, and each language is a
// table in an .ecfg file. Simplified and Traditional Chinese are separate tables (they are separate
// translations, not a runtime conversion), which is also why a font has to be chosen per language:
// Latin text uses the Latin font, Chinese text needs a CJK one.
//
//     en::
//         menu.start:"START SINGLE PLAYER"
//     zh-Hans::
//         menu.start:"开始单人游戏"
//     zh-Hant::
//         menu.start:"開始單人遊戲"
#pragma once

#include <t2d/core/ecfg.h>
#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace t2d {

enum class Language : u8 { English = 0, SimplifiedChinese, TraditionalChinese, Count };
inline constexpr usize kLanguageCount = static_cast<usize>(Language::Count);

/// "en", "zh-Hans", "zh-Hant".
[[nodiscard]] const char* language_code(Language language);
[[nodiscard]] const char* language_name(Language language); ///< endonym, for a language menu
/// Accepts the usual spellings: en, zh, zh-Hans, zh_CN, zh-TW, zh-Hant, Chinese, ...
[[nodiscard]] std::optional<Language> parse_language(std::string_view text);
/// True for both Chinese variants: they share a font and the line breaking rules.
[[nodiscard]] bool is_chinese(Language language);

/// id -> text, one table per language.
class StringTable {
public:
    void set(Language language, std::string_view id, std::string_view text);
    [[nodiscard]] std::string_view find(Language language, std::string_view id) const;
    [[nodiscard]] usize count(Language language) const;
    [[nodiscard]] bool empty() const;
    /// Ids this language knows, in insertion order.
    [[nodiscard]] std::vector<std::string_view> ids(Language language) const;

private:
    friend class Locale;

    std::vector<std::pair<std::string, std::string>> tables_[kLanguageCount];
};

/// The interface's view of the string tables: one active language, with a fallback chain.
class Locale {
public:
    /// Loads a document whose top level tables are named after languages. Unknown table names are
    /// reported instead of ignored, so a typo in a translation file is visible.
    [[nodiscard]] static Locale from_ecfg(const EcfgDocument& document,
                                          std::vector<std::string>* unknown_tables = nullptr);
    [[nodiscard]] static Locale from_ecfg_text(std::string_view text, EcfgError* error = nullptr,
                                               std::vector<std::string>* unknown_tables = nullptr);

    void set_language(Language language) { language_ = language; }
    [[nodiscard]] Language language() const { return language_; }

    /// The text for \p id: the active language first, then English, then the id itself (so a missing
    /// translation shows up as an obvious identifier rather than as an empty label).
    [[nodiscard]] std::string_view text(std::string_view id) const;
    /// Same, but reports whether the active language actually had it.
    [[nodiscard]] std::string_view text(std::string_view id, bool& translated) const;

    [[nodiscard]] const StringTable& strings() const { return strings_; }
    [[nodiscard]] usize count(Language language) const { return strings_.count(language); }
    /// Ids the active language is missing (English has them). Filled on demand.
    [[nodiscard]] std::vector<std::string> missing_for_active_language() const;

private:
    StringTable strings_;
    Language language_ = Language::English;
};

} // namespace t2d
