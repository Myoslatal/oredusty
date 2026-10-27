#include <mefemma/settings.h>

#include <filesystem>
#include <format>

namespace mefemma {
namespace {

/// A window smaller than this is a window with no room for the interface, and one larger is a number
/// someone typed by accident: both are refused rather than passed to the window system.
constexpr u32 kMinWidth = 320;
constexpr u32 kMaxWidth = 7680;
constexpr u32 kMinHeight = 240;
constexpr u32 kMaxHeight = 4320;

void fail(t2d::EcfgError* error, std::string message) {
    if (error != nullptr) *error = t2d::EcfgError{0, 0, std::move(message)};
}

} // namespace

Settings Settings::defaults() { return Settings{}; }

Settings Settings::load(const std::string& path, t2d::EcfgError* error) {
    std::error_code code;
    if (!std::filesystem::is_regular_file(path, code)) {
        // No file: the defaults are the game. This is not a failure to report - a game that ships no
        // settings is a game that runs on them.
        return defaults();
    }
    t2d::EcfgError parse_error;
    std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::load(path, &parse_error);
    if (!document.has_value()) {
        if (error != nullptr) *error = parse_error;
        return defaults();
    }
    return from_document(*document, error);
}

Settings Settings::from_document(const t2d::EcfgDocument& document, t2d::EcfgError* error) {
    Settings settings;
    settings.document = document;
    const t2d::EcfgValue& root = document.root();

    if (const t2d::EcfgValue* title = root.find("title"); title != nullptr) {
        if (title->is_string()) settings.title = std::string(title->as_string());
        else fail(error, "settings: title must be a quoted string");
    }
    if (const t2d::EcfgValue* width = root.find("width"); width != nullptr) {
        if (width->type() != t2d::EcfgType::Int) {
            fail(error, "settings: width must be a whole number of pixels");
        } else {
            const t2d::i64 value = width->as_int();
            if (value < static_cast<t2d::i64>(kMinWidth) || value > static_cast<t2d::i64>(kMaxWidth)) {
                fail(error, std::format("settings: width is {}, and a window is {}..{} pixels wide", value,
                                        kMinWidth, kMaxWidth));
            } else {
                settings.width = static_cast<u32>(value);
            }
        }
    }
    if (const t2d::EcfgValue* height = root.find("height"); height != nullptr) {
        if (height->type() != t2d::EcfgType::Int) {
            fail(error, "settings: height must be a whole number of pixels");
        } else {
            const t2d::i64 value = height->as_int();
            if (value < static_cast<t2d::i64>(kMinHeight) || value > static_cast<t2d::i64>(kMaxHeight)) {
                fail(error, std::format("settings: height is {}, and a window is {}..{} pixels tall", value,
                                        kMinHeight, kMaxHeight));
            } else {
                settings.height = static_cast<u32>(value);
            }
        }
    }
    if (const t2d::EcfgValue* vsync = root.find("vsync"); vsync != nullptr) {
        if (vsync->type() != t2d::EcfgType::Bool) {
            fail(error, "settings: vsync must be true or false");
        } else {
            settings.vsync = vsync->as_bool();
        }
    }
    if (const t2d::EcfgValue* language = root.find("language"); language != nullptr) {
        if (!language->is_string()) fail(error, "settings: language must be a quoted language code");
        else settings.language = std::string(language->as_string());
    }
    if (const t2d::EcfgValue* seed = root.find("seed"); seed != nullptr) {
        if (seed->type() != t2d::EcfgType::Int || seed->as_int() < 0) {
            fail(error, "settings: seed must be a whole number that is not negative");
        } else {
            settings.seed = static_cast<u64>(seed->as_int());
        }
    }
    return settings;
}

const t2d::EcfgValue* Settings::find(std::string_view key) const {
    if (!document.has_value()) return nullptr;
    return document->find(key);
}

} // namespace mefemma
