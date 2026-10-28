#include "wayland_text_input.h"

#include <ore/core/log.h>
#include <ore/platform/input.h>

#include <wayland-client.h>

#include "text-input-unstable-v3-client-protocol.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace ore {
namespace {

constexpr std::string_view kManagerInterface = "zwp_text_input_manager_v3";
constexpr std::string_view kSeatInterface = "wl_seat";

[[nodiscard]] const char* safe_text(const char* text) { return text != nullptr ? text : ""; }

} // namespace

WaylandTextInput::WaylandTextInput(InputState& input, wl_display* display, wl_surface* surface)
    : input_(&input), display_(display), surface_(surface) {}

WaylandTextInput::~WaylandTextInput() {
    if (text_input_ != nullptr) {
        // The protocol asks for disable before destroy, and a compositor that is still using the object
        // should not see it disappear with text focused.
        if (enabled_) {
            zwp_text_input_v3_disable(text_input_);
            zwp_text_input_v3_commit(text_input_);
        }
        zwp_text_input_v3_destroy(text_input_);
        text_input_ = nullptr;
    }
    if (manager_ != nullptr) {
        zwp_text_input_manager_v3_destroy(manager_);
        manager_ = nullptr;
    }
    if (seat_ != nullptr) {
        wl_seat_destroy(seat_);
        seat_ = nullptr;
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    // GLFW owns the display and flushes it; the disable above just has to be on the wire.
    if (display_ != nullptr) wl_display_flush(display_);
}

Scope<WaylandTextInput> WaylandTextInput::create(InputState& input, wl_display* display,
                                                 wl_surface* surface) {
    Scope<WaylandTextInput> self(new WaylandTextInput(input, display, surface));
    self->bind_globals();
    return self;
}

void WaylandTextInput::bind_globals() {
    if (display_ == nullptr || surface_ == nullptr) {
        ORE_WARN("wayland text input: no display or surface; the IME stays disabled");
        return;
    }
    registry_ = wl_display_get_registry(display_);
    if (registry_ == nullptr) {
        ORE_WARN("wayland text input: the registry could not be created; the IME stays disabled");
        return;
    }
    static constexpr wl_registry_listener kRegistryListener = {
        &WaylandTextInput::on_global,
        &WaylandTextInput::on_global_remove,
    };
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    // One round trip: the compositor announces every global, including the manager and the seat.
    if (wl_display_roundtrip(display_) < 0) {
        ORE_WARN("wayland text input: the registry round trip failed; the IME stays disabled");
        return;
    }
    if (text_input_ != nullptr) return;
    if (!manager_bound_) {
        ORE_WARN("wayland text input: the compositor offers no zwp_text_input_manager_v3; the IME "
                 "stays disabled");
    } else {
        ORE_WARN("wayland text input: no wl_seat was announced; the IME stays disabled");
    }
}

void WaylandTextInput::on_global(void* data, wl_registry* registry, u32 name, const char* interface,
                                 u32 version) {
    auto* self = static_cast<WaylandTextInput*>(data);
    if (self == nullptr) return;
    const std::string_view announced = safe_text(interface);
    if (announced == kManagerInterface && !self->manager_bound_) {
        const u32 bind_version = std::min<u32>(version, 1u);
        self->manager_ = static_cast<zwp_text_input_manager_v3*>(
            wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, bind_version));
        if (self->manager_ == nullptr) return;
        self->manager_bound_ = true;
        self->manager_version_ = bind_version;
        ORE_INFO("wayland text input: zwp_text_input_manager_v3 bound (version {})", bind_version);
        self->ensure_text_input();
    } else if (announced == kSeatInterface && self->seat_ == nullptr) {
        self->seat_ = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1u));
        self->ensure_text_input();
    }
}

void WaylandTextInput::on_global_remove(void* data, wl_registry* registry, u32 name) {
    (void)data;
    (void)registry;
    (void)name;
    // A compositor does not remove these globals while a client is alive; the proxies are released in
    // the destructor either way.
}

void WaylandTextInput::ensure_text_input() {
    if (manager_ == nullptr || seat_ == nullptr || text_input_ != nullptr) return;
    text_input_ = zwp_text_input_manager_v3_get_text_input(manager_, seat_);
    if (text_input_ == nullptr) {
        ORE_WARN("wayland text input: the compositor refused a text input object; the IME stays "
                 "disabled");
        return;
    }
    static constexpr zwp_text_input_v3_listener kTextInputListener = {
        &WaylandTextInput::on_enter,
        &WaylandTextInput::on_leave,
        &WaylandTextInput::on_preedit,
        &WaylandTextInput::on_commit,
        &WaylandTextInput::on_delete,
        &WaylandTextInput::on_done,
        nullptr, // action (version 2)
        nullptr, // language (version 2)
        nullptr, // preedit_hint (version 2)
    };
    zwp_text_input_v3_add_listener(text_input_, &kTextInputListener, this);
    ORE_INFO("wayland text input: text input object created (manager version {})", manager_version_);
}

void WaylandTextInput::set_enabled(bool enabled) {
    if (enabled == enabled_requested_) return;
    enabled_requested_ = enabled;
    if (enabled) {
        if (focused_) apply_enable();
        return;
    }
    apply_disable();
    if (input_ != nullptr) input_->clear_preedit();
    pending_preedit_.clear();
    pending_commit_.clear();
    pending_preedit_changed_ = false;
    pending_commit_changed_ = false;
}

void WaylandTextInput::apply_enable() {
    if (text_input_ == nullptr || enabled_ || !focused_) return;
    zwp_text_input_v3_enable(text_input_);
    zwp_text_input_v3_set_content_type(text_input_, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                       ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    send_cursor_rect();
    zwp_text_input_v3_commit(text_input_);
    enabled_ = true;
    ORE_INFO("wayland text input: enabled (IME may compose)");
}

void WaylandTextInput::apply_disable() {
    if (text_input_ == nullptr || !enabled_) return;
    zwp_text_input_v3_disable(text_input_);
    zwp_text_input_v3_commit(text_input_);
    enabled_ = false;
    ORE_INFO("wayland text input: disabled");
}

void WaylandTextInput::set_cursor_rect(i32 x, i32 y, i32 w, i32 h) {
    if (text_input_ == nullptr) return;
    if (has_cursor_rect_ && x == cursor_x_ && y == cursor_y_ && w == cursor_w_ && h == cursor_h_) return;
    cursor_x_ = x;
    cursor_y_ = y;
    cursor_w_ = w;
    cursor_h_ = h;
    has_cursor_rect_ = true;
    if (!enabled_) return;
    send_cursor_rect();
    zwp_text_input_v3_commit(text_input_);
}

void WaylandTextInput::send_cursor_rect() {
    if (text_input_ == nullptr || !has_cursor_rect_) return;
    if (sent_cursor_x_ == cursor_x_ && sent_cursor_y_ == cursor_y_ && sent_cursor_w_ == cursor_w_ &&
        sent_cursor_h_ == cursor_h_) {
        return;
    }
    zwp_text_input_v3_set_cursor_rectangle(text_input_, cursor_x_, cursor_y_, cursor_w_, cursor_h_);
    sent_cursor_x_ = cursor_x_;
    sent_cursor_y_ = cursor_y_;
    sent_cursor_w_ = cursor_w_;
    sent_cursor_h_ = cursor_h_;
}

void WaylandTextInput::on_enter(void* data, zwp_text_input_v3* text_input, wl_surface* surface) {
    (void)text_input;
    auto* self = static_cast<WaylandTextInput*>(data);
    if (self == nullptr || surface != self->surface_) return;
    self->focused_ = true;
    ORE_INFO("wayland text input: enter");
    if (self->enabled_requested_) self->apply_enable();
}

void WaylandTextInput::on_leave(void* data, zwp_text_input_v3* text_input, wl_surface* surface) {
    (void)text_input;
    (void)surface;
    auto* self = static_cast<WaylandTextInput*>(data);
    if (self == nullptr) return;
    self->focused_ = false;
    self->apply_disable();
    if (self->input_ != nullptr) self->input_->clear_preedit();
    self->pending_preedit_.clear();
    self->pending_commit_.clear();
    self->pending_preedit_changed_ = false;
    self->pending_commit_changed_ = false;
    ORE_INFO("wayland text input: leave");
}

void WaylandTextInput::on_preedit(void* data, zwp_text_input_v3* text_input, const char* text,
                                  i32 cursor_begin, i32 cursor_end) {
    (void)text_input;
    (void)cursor_begin;
    (void)cursor_end;
    auto* self = static_cast<WaylandTextInput*>(data);
    if (self == nullptr) return;
    self->pending_preedit_ = safe_text(text);
    self->pending_preedit_changed_ = true;
}

void WaylandTextInput::on_commit(void* data, zwp_text_input_v3* text_input, const char* text) {
    (void)text_input;
    auto* self = static_cast<WaylandTextInput*>(data);
    if (self == nullptr) return;
    self->pending_commit_.append(safe_text(text));
    self->pending_commit_changed_ = true;
}

void WaylandTextInput::on_delete(void* data, zwp_text_input_v3* text_input, u32 before_length,
                                 u32 after_length) {
    (void)text_input;
    (void)before_length;
    (void)after_length;
    // The field is a name, not a document, and it does not advertise surrounding text: an IME has
    // nothing to delete around a cursor it was never told about.
    (void)data;
}

void WaylandTextInput::on_done(void* data, zwp_text_input_v3* text_input, u32 serial) {
    (void)serial;
    auto* self = static_cast<WaylandTextInput*>(data);
    if (self == nullptr) return;
    // The protocol's order: replace the preedit, delete surrounding text, insert the commit, then put
    // the new preedit at the cursor. There is no surrounding text to delete here.
    if (self->pending_commit_changed_) {
        if (!self->pending_commit_.empty() && self->input_ != nullptr) {
            self->input_->add_text(self->pending_commit_);
            ORE_INFO("wayland text input: committed {} byte(s)", self->pending_commit_.size());
        }
        self->pending_commit_.clear();
        self->pending_commit_changed_ = false;
    }
    if (self->pending_preedit_changed_) {
        if (self->input_ != nullptr) self->input_->set_preedit(self->pending_preedit_);
        self->pending_preedit_changed_ = false;
    }
    (void)text_input;
}

} // namespace ore
