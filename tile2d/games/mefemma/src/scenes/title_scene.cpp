#include <mefemma/scenes/title_scene.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace mefemma {
namespace {

// --- the picture the screen is drawn on ---------------------------------------------------------------
// start_screen.png is 1920 x 1080. Its paper face runs from (272, 220) to (1581, 833); everything below is
// placed in those image coordinates and mapped through SignLayout, so the menu stays on the paper when the
// window changes size or aspect.
constexpr f32 kImageWidth = 1920.0f;
constexpr f32 kImageHeight = 1080.0f;
constexpr f32 kPaperCentreX = 926.0f;

constexpr u32 kBackground = rgba(18, 12, 6);
constexpr u32 kWood = rgba(162, 105, 0);
constexpr u32 kWoodDark = rgba(105, 66, 0);
constexpr u32 kOrange = rgba(255, 166, 0);
constexpr u32 kCream = rgba(247, 226, 184);
constexpr u32 kInk = rgba(61, 36, 4);
constexpr u32 kShadow = rgba(32, 18, 2, 130);
constexpr u32 kOverlay = rgba(8, 5, 2, 190);

constexpr Aabb2 kTitleBox{Vec2{436.0f, 244.0f}, Vec2{1416.0f, 368.0f}};
constexpr f32 kItemLeft = 566.0f;
constexpr f32 kItemWidth = 720.0f;
constexpr f32 kItemHeight = 62.0f;
constexpr f32 kItemPitch = 76.0f;
constexpr f32 kFirstItemTop = 402.0f;
constexpr Aabb2 kItemBoxes[kStartActionCount] = {
    Aabb2{Vec2{kItemLeft, kFirstItemTop + 0.0f * kItemPitch}, Vec2{kItemLeft + kItemWidth, kFirstItemTop + 0.0f * kItemPitch + kItemHeight}},
    Aabb2{Vec2{kItemLeft, kFirstItemTop + 1.0f * kItemPitch}, Vec2{kItemLeft + kItemWidth, kFirstItemTop + 1.0f * kItemPitch + kItemHeight}},
    Aabb2{Vec2{kItemLeft, kFirstItemTop + 2.0f * kItemPitch}, Vec2{kItemLeft + kItemWidth, kFirstItemTop + 2.0f * kItemPitch + kItemHeight}},
    Aabb2{Vec2{kItemLeft, kFirstItemTop + 3.0f * kItemPitch}, Vec2{kItemLeft + kItemWidth, kFirstItemTop + 3.0f * kItemPitch + kItemHeight}},
    Aabb2{Vec2{kItemLeft, kFirstItemTop + 4.0f * kItemPitch}, Vec2{kItemLeft + kItemWidth, kFirstItemTop + 4.0f * kItemPitch + kItemHeight}},
};
constexpr Aabb2 kModalBox{Vec2{456.0f, 400.0f}, Vec2{1396.0f, 704.0f}};
constexpr Vec2 kTitleCentre{kPaperCentreX, 306.0f};
constexpr Vec2 kHintCentre{kPaperCentreX, 801.0f};
constexpr Vec2 kModalTitleCentre{kPaperCentreX, 462.0f};
constexpr Vec2 kModalBodyCentre{kPaperCentreX, 566.0f};
constexpr Vec2 kModalBackCentre{kPaperCentreX, 654.0f};

constexpr const char* kActionIds[kStartActionCount] = {
    "start.new_game", "start.load_game", "start.settings", "start.mods", "start.quit",
};

[[nodiscard]] const char* label_of(StartAction action) {
    const usize index = static_cast<usize>(action);
    return index < kStartActionCount ? kActionIds[index] : "start.new_game";
}

/// The picture's own coordinates mapped onto the window: the image is fitted whole (letterboxed when the
/// aspect differs) and centred.
struct SignLayout {
    f32 scale = 1.0f;
    Vec2 offset{};

    [[nodiscard]] Vec2 point(Vec2 image) const { return offset + image * scale; }
    [[nodiscard]] Aabb2 box(const Aabb2& image) const { return Aabb2{point(image.min), point(image.max)}; }
    [[nodiscard]] f32 length(f32 image_units) const { return image_units * scale; }
};

[[nodiscard]] SignLayout sign_layout(Vec2 viewport) {
    const f32 scale = std::max(0.001f, std::min(viewport.x / kImageWidth, viewport.y / kImageHeight));
    const Vec2 drawn{kImageWidth * scale, kImageHeight * scale};
    return SignLayout{scale, Vec2{(viewport.x - drawn.x) * 0.5f, (viewport.y - drawn.y) * 0.5f}};
}

/// A font size given in image pixels, mapped to the window and kept inside a readable range: a 320 px wide
/// window still gets 10 px text instead of a two-pixel smear.
[[nodiscard]] u16 scaled_size(f32 image_pixels, const SignLayout& sign) {
    return static_cast<u16>(std::clamp(image_pixels * sign.scale, 9.0f, 160.0f));
}

/// The largest size up to \p base that keeps \p text inside \p max_width. A language with longer words
/// (or a wider font) gets a smaller row rather than a row that runs off the plaque.
[[nodiscard]] u16 fitted_size(GameServices& services, std::string_view text, u16 base, f32 max_width) {
    u16 size = base;
    while (size > 8 && services.measure_text(text, size).width > max_width) --size;
    return size;
}

/// One line of text with its box centred on \p centre. measure_text() and the renderer use the same text
/// engine, so the pen computed here is the box the renderer will fill.
void centred_text(DrawList& out, GameServices& services, std::string_view text, Vec2 centre, u16 size,
                  u32 color) {
    const t2d::TextMetrics metrics = services.measure_text(text, size);
    out.text(std::string(text), Vec2{centre.x - metrics.width * 0.5f, centre.y - metrics.height * 0.5f}, size,
             color);
}

} // namespace

std::unique_ptr<Scene> make_title_scene() { return std::make_unique<TitleScene>(); }

t2d::Aabb2 TitleScene::item_rect(t2d::Vec2 viewport, usize index) {
    if (index >= kStartActionCount) return {};
    return sign_layout(viewport).box(kItemBoxes[index]);
}

void TitleScene::on_enter(GameServices& services) {
    (void)services;
    T2D_INFO("scene: title (start screen, {} rows)", kStartActionCount);
}

void TitleScene::move_selection(i32 delta) {
    const i32 count = static_cast<i32>(kStartActionCount);
    const i32 next = ((static_cast<i32>(selected_) + delta) % count + count) % count;
    selected_ = static_cast<usize>(next);
}

void TitleScene::activate(GameServices& services) {
    const StartAction action = selected_action();
    if (action == StartAction::Quit) {
        services.quit();
        return;
    }
    // The four other rows are the game's, and the game does not exist yet: the screen says so instead of
    // silently doing nothing. Replacing this with the real transition is where a game starts.
    modal_open_ = true;
    modal_action_ = action;
    T2D_INFO("start: '{}' is not implemented yet", label_of(action));
}

void TitleScene::update(GameServices& services, f32 delta_seconds) {
    (void)delta_seconds;
    const InputState& input = services.input();

    // The pointer selects what it is over - when it moves, or when it first comes into the window. A
    // pointer that is merely resting must not undo keyboard navigation: the two are one selection, and the
    // last one to act wins.
    const bool inside = input.cursor_inside_window();
    const bool pointer_moved = input.mouse_delta_x() != 0.0f || input.mouse_delta_y() != 0.0f;
    const bool pointer_entered = inside && !pointer_was_inside_;
    pointer_was_inside_ = inside;

    if (modal_open_) {
        // Any of "get me out of here": the note is not a form, it has nothing to confirm.
        if (input.key_pressed(Key::Escape) || input.key_pressed(Key::Enter) || input.key_pressed(Key::Space) ||
            input.mouse_pressed(MouseButton::Left)) {
            modal_open_ = false;
        }
        return;
    }

    const t2d::Vec2 viewport = services.viewport();
    const t2d::Vec2 mouse{input.mouse_x(), input.mouse_y()};
    if (inside && (pointer_moved || pointer_entered)) {
        for (usize index = 0; index < kStartActionCount; ++index) {
            if (!item_rect(viewport, index).contains(mouse)) continue;
            selected_ = index;
            break;
        }
    }

    // Keyboard after the pointer, so on a frame where both happened the keyboard answer is the one the
    // player is looking at.
    if (input.key_pressed(Key::Up) || input.key_pressed(Key::W)) move_selection(-1);
    if (input.key_pressed(Key::Down) || input.key_pressed(Key::S)) move_selection(1);

    if (input.key_pressed(Key::Enter) || input.key_pressed(Key::Space)) {
        activate(services);
        return;
    }
    if (inside && input.mouse_pressed(MouseButton::Left)) {
        // A click acts on the row it landed on, whether or not the pointer had moved in this frame.
        for (usize index = 0; index < kStartActionCount; ++index) {
            if (!item_rect(viewport, index).contains(mouse)) continue;
            selected_ = index;
            activate(services);
            return;
        }
    }
    if (input.key_pressed(Key::Escape)) services.quit();
}

void TitleScene::draw(GameServices& services, DrawList& out) const {
    const t2d::Vec2 viewport = services.viewport();
    const SignLayout sign = sign_layout(viewport);

    out.rect(Aabb2{Vec2{0.0f, 0.0f}, viewport}, kBackground);
    // **Linear** filtering: this is a painted sign scaled to the window, not a pixel-art tile. Nearest would
    // make every plank edge crawl when the window is resized.
    out.sprite("start_screen", sign.box(Aabb2{Vec2{0.0f, 0.0f}, Vec2{kImageWidth, kImageHeight}}), full_uv(),
               kWhite, TextureFilter::Linear);

    // --- the name plaque -----------------------------------------------------------------------------
    const Aabb2 title = sign.box(kTitleBox);
    const f32 border = std::max(1.0f, sign.length(5.0f));
    out.rect(title.translated(Vec2{sign.length(6.0f), sign.length(8.0f)}), kShadow);
    out.rect(title, kWood);
    out.rect_outline(title, border, kOrange);
    out.rect_outline(title.expanded(-sign.length(14.0f)), std::max(1.0f, sign.length(2.0f)), rgba(255, 196, 84, 90));
    const f32 stud = sign.length(13.0f);
    for (const f32 at : {478.0f, 1374.0f}) {
        const Vec2 centre = sign.point(Vec2{at, 306.0f});
        out.rect(Aabb2{Vec2{centre.x - stud, centre.y - stud}, Vec2{centre.x + stud, centre.y + stud}}, kOrange);
    }
    const std::string name(services.text("start.title"));
    const u16 title_size = fitted_size(services, name, scaled_size(76.0f, sign), title.width() - sign.length(180.0f));
    centred_text(out, services, name, sign.point(kTitleCentre), title_size, kCream);

    // --- the five rows -------------------------------------------------------------------------------
    for (usize index = 0; index < kStartActionCount; ++index) {
        const Aabb2 box = sign.box(kItemBoxes[index]);
        const bool active = index == selected_ && !modal_open_;
        out.rect(box.translated(Vec2{sign.length(4.0f), sign.length(5.0f)}), kShadow);
        out.rect(box, active ? kOrange : kWood);
        out.rect_outline(box, std::max(1.0f, sign.length(3.0f)), active ? kCream : kOrange);
        if (active) {
            // A cream tab on the left: the selected row is the bright one, and the tab says which edge it is
            // read from when the whole row is already orange.
            const f32 tab = sign.length(9.0f);
            const Vec2 centre{box.min.x + sign.length(26.0f), (box.min.y + box.max.y) * 0.5f};
            out.rect(Aabb2{Vec2{centre.x - tab * 0.5f, centre.y - sign.length(16.0f)},
                           Vec2{centre.x + tab * 0.5f, centre.y + sign.length(16.0f)}},
                     kCream);
        }
        const std::string label(services.text(label_of(static_cast<StartAction>(index))));
        const u16 size = fitted_size(services, label, scaled_size(33.0f, sign),
                                     box.width() - sign.length(170.0f));
        centred_text(out, services, label, Vec2{(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f},
                     size, active ? kInk : kCream);
    }

    // --- the hint, on the paper under the last row ----------------------------------------------------
    const std::string hint(services.text("start.hint"));
    const u16 hint_size = fitted_size(services, hint, scaled_size(22.0f, sign), sign.length(1180.0f));
    centred_text(out, services, hint, sign.point(kHintCentre), hint_size, kInk);

    // --- the note the unimplemented rows open ---------------------------------------------------------
    if (!modal_open_) return;
    out.rect(Aabb2{Vec2{0.0f, 0.0f}, viewport}, kOverlay);
    const Aabb2 panel = sign.box(kModalBox);
    out.rect(panel.translated(Vec2{sign.length(8.0f), sign.length(10.0f)}), kShadow);
    out.rect(panel, kWoodDark);
    out.rect_outline(panel, std::max(1.0f, sign.length(5.0f)), kOrange);
    out.rect_outline(panel.expanded(-sign.length(16.0f)), std::max(1.0f, sign.length(2.0f)), rgba(255, 196, 84, 90));

    const std::string what(services.text(label_of(modal_action_)));
    const std::string body(services.text("start.not_implemented"));
    const std::string back(services.text("start.back"));
    centred_text(out, services, what,
                 sign.point(kModalTitleCentre),
                 fitted_size(services, what, scaled_size(46.0f, sign), panel.width() - sign.length(120.0f)), kCream);
    out.rect(Aabb2{sign.point(Vec2{kPaperCentreX - 300.0f, 506.0f}),
                   sign.point(Vec2{kPaperCentreX + 300.0f, 510.0f})},
             kOrange);
    centred_text(out, services, body,
                 sign.point(kModalBodyCentre),
                 fitted_size(services, body, scaled_size(30.0f, sign), panel.width() - sign.length(120.0f)), kCream);
    centred_text(out, services, back, sign.point(kModalBackCentre), scaled_size(22.0f, sign), kOrange);
}

} // namespace mefemma
