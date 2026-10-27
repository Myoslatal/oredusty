#include <mefemma/assets.h>

#include <t2d/core/ecfg.h>
#include <t2d/core/log.h>

#include <ore/core/image.h>

#include <filesystem>
#include <format>

namespace mefemma {
namespace {

/// Fonts are looked up in the usual system locations; a game that ships one points the command line at it.
/// The list is the same one the engine's own shell uses - this is where the fonts live on a normal Linux
/// desktop, not a rule about the game.
constexpr const char* kLatinCandidates[] = {
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
};
constexpr const char* kCjkCandidates[] = {
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
};

[[nodiscard]] Scope<t2d::Font> load_font_file(const std::string& path, u32 face) {
    std::string error;
    std::optional<t2d::Font> font = t2d::Font::load(path, face, &error);
    if (!font.has_value()) {
        T2D_WARN("font '{}' face {}: {}", path, face, error);
        return nullptr;
    }
    T2D_INFO("font: '{} {}' ({} glyphs, {} outlines) from {}", font->family_name(), font->style_name(),
             font->metrics().glyph_count, font->outline_kind(), path);
    return t2d::make_scope<t2d::Font>(std::move(*font));
}

/// Picks the face of a CJK collection that matches the language: Simplified and Traditional Chinese are
/// different faces of the same file, and they really do draw some characters differently.
[[nodiscard]] u32 cjk_face_for(const std::string& path, t2d::Language language) {
    const std::vector<std::pair<std::string, std::string>> names = t2d::Font::face_names(path);
    if (names.size() <= 1) return 0;
    const char* wanted = language == t2d::Language::TraditionalChinese ? "TC" : "SC";
    for (usize index = 0; index < names.size(); ++index) {
        if (names[index].first.find(wanted) == std::string::npos) continue;
        if (names[index].first.find("Mono") != std::string::npos) continue;
        return static_cast<u32>(index);
    }
    return 0;
}

[[nodiscard]] std::string first_existing(const char* const* candidates, usize count) {
    for (usize index = 0; index < count; ++index) {
        std::error_code code;
        if (std::filesystem::is_regular_file(candidates[index], code)) return candidates[index];
    }
    return {};
}

} // namespace

Assets::Assets(ore::rhi::GraphicsContext& context, DataPaths paths, std::string latin_font_override,
               std::string cjk_font_override)
    : context_(&context),
      paths_(std::move(paths)),
      latin_font_override_(std::move(latin_font_override)),
      cjk_font_override_(std::move(cjk_font_override)) {}

Assets::~Assets() = default;

bool Assets::load_text() {
    const std::string path = paths_.text("ui");
    std::error_code code;
    if (!std::filesystem::is_regular_file(path, code)) {
        T2D_WARN("strings: '{}' is not there; every label will show its id", path);
        return false;
    }
    t2d::EcfgError error;
    std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &error);
    if (!document.has_value()) {
        if (!t2d::ecfg_skipped(error)) T2D_WARN("strings '{}': {}", path, error.describe(path));
        return false;
    }
    std::vector<std::string> unknown;
    locale_ = t2d::Locale::from_ecfg(*document, &unknown);
    for (const std::string& table : unknown) {
        T2D_WARN("strings: '{}' is not a language", table);
    }
    T2D_INFO("strings: {} language table(s), {} string(s) in '{}'", t2d::kLanguageCount,
             locale_.count(locale_.language()), path);
    return true;
}

bool Assets::load_fonts() {
    if (latin_font_ == nullptr) {
        const std::string latin_path = !latin_font_override_.empty()
                                           ? latin_font_override_
                                           : first_existing(kLatinCandidates, std::size(kLatinCandidates));
        if (!latin_path.empty()) latin_font_ = load_font_file(latin_path, 0);
        if (latin_font_ == nullptr) T2D_ERROR("no usable Latin font: text will not render");
    }

    const usize language = static_cast<usize>(locale_.language());
    // Both Chinese variants need a CJK face, and the test is written out rather than calling
    // t2d::is_chinese(): that function is engine code but not part of the published surface, and a code
    // table may only ask the host for what is in engine.api.
    const bool chinese = locale_.language() == t2d::Language::SimplifiedChinese ||
                         locale_.language() == t2d::Language::TraditionalChinese;
    if (chinese && cjk_fonts_[language] == nullptr) {
        const std::string cjk_path = !cjk_font_override_.empty()
                                         ? cjk_font_override_
                                         : first_existing(kCjkCandidates, std::size(kCjkCandidates));
        if (!cjk_path.empty()) {
            cjk_fonts_[language] = load_font_file(cjk_path, cjk_face_for(cjk_path, locale_.language()));
        }
    }
    fonts_.latin = latin_font_.get();
    // The active language's face first; failing that (English, or a CJK collection that is not installed) a
    // face that was loaded already, so a line that happens to contain Chinese is still drawn.
    fonts_.cjk = cjk_fonts_[language].get();
    if (fonts_.cjk == nullptr) {
        for (const Scope<t2d::Font>& font : cjk_fonts_) {
            if (font != nullptr) {
                fonts_.cjk = font.get();
                break;
            }
        }
    }
    return fonts_.latin != nullptr;
}

void Assets::set_language(t2d::Language language) {
    locale_.set_language(language);
    // The CJK face depends on the language, so the fonts are loaded again; a language that has no CJK face
    // simply has no cjk font, and the text engine falls back to the Latin one per character.
    if (load_fonts()) {
        T2D_INFO("language: {} ({} strings, {} missing)", t2d::language_code(language),
                 locale_.count(language), locale_.missing_for_active_language().size());
    }
}

ore::rhi::Texture* Assets::texture(std::string_view name) {
    const std::string key(name);
    const auto loaded = textures_.find(key);
    if (loaded != textures_.end()) return loaded->second.get();
    const auto failed = texture_failed_.find(key);
    if (failed != texture_failed_.end()) return nullptr;

    const std::string path = paths_.texture(name);
    std::error_code code;
    if (!std::filesystem::is_regular_file(path, code)) {
        const std::string message = std::format("texture '{}': '{}' is not there", key, path);
        T2D_WARN("assets: {}", message);
        texture_failed_.emplace(key, true);
        texture_errors_.push_back(message);
        return nullptr;
    }
    std::optional<ore::Image> image = ore::Image::load_png(path);
    if (!image.has_value()) {
        const std::string message = std::format("texture '{}': '{}' could not be decoded (PNG, 8-bit, not interlaced)",
                                                key, path);
        T2D_WARN("assets: {}", message);
        texture_failed_.emplace(key, true);
        texture_errors_.push_back(message);
        return nullptr;
    }
    Scope<ore::rhi::Texture> texture = context_->create_texture(*image, false, key);
    if (texture == nullptr) {
        const std::string message = std::format("texture '{}': the GPU refused a {}x{} texture", key, image->width,
                                                image->height);
        T2D_WARN("assets: {}", message);
        texture_failed_.emplace(key, true);
        texture_errors_.push_back(message);
        return nullptr;
    }
    auto inserted = textures_.emplace(key, std::move(texture));
    return inserted.first->second.get();
}

bool Assets::has_texture(std::string_view name) const {
    return textures_.find(std::string(name)) != textures_.end();
}

} // namespace mefemma
