// Ore framework - the debug input server: press the application's keys from another process.
//
// A window is not always reachable by the tools that normally type into one. A Wayland session decides
// for itself who owns the keyboard, a nested compositor takes the events first, a headless run has no
// window at all - and none of that matters to the application, which reads an InputState the platform
// layer fills in. So this server fills it in instead, from a TCP socket on the loopback interface,
// through the same calls the window layer makes.
//
//   $ mine_game --input-server 7777
//   [INFO] debug input server: listening on 127.0.0.1:7777
//   $ printf 'key press F6\nshot /tmp/list.png\nquit\n' | nc 127.0.0.1 7777
//   ok
//   ok
//   ok
//
// One command per line, one reply per command:
//
//   key <down|up|press> <NAME>    press is a down and an up in the same frame, which is what a tap is
//   mouse move <x> <y>            framebuffer pixels: the coordinates the application draws in
//   mouse <down|up|click> <left|right|middle>
//   scroll <x> <y>                wheel deltas, positive y is up
//   text <utf8>                   typed characters, as the keyboard would deliver them
//   shot <path>                   write the next rendered frame to a PNG
//   quit                          ask the application to close
//
// The reply is "ok" only once the frame loop has applied the command, so a script that reads it knows
// the application has seen the input and a screenshot taken after it is a screenshot of the result.
// "error ..." means the line made no sense; "timeout" means no frame ran in time (a minimised window,
// a stalled application).
//
// This is a debug facility and it is honest about it: it listens on 127.0.0.1 only, it is off unless
// --input-server asks for it, and it can do anything a player sitting at the keyboard can do.
#pragma once

#include <ore/core/types.h>
#include <ore/platform/input.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ore::debug {

/// One thing to do to the input state, or to the application.
struct InputEvent {
    enum class Kind : u8 { Key, MouseButton, MouseMove, Scroll, Text, Screenshot, Quit };
    Kind kind = Kind::Key;
    Key key = Key::Unknown;                  ///< Key
    MouseButton button = MouseButton::Left;  ///< MouseButton
    bool down = false;                       ///< Key/MouseButton: true is a press, false a release
    f32 x = 0.0f;                            ///< MouseMove: pixels; Scroll: the wheel delta
    f32 y = 0.0f;
    std::string text;                        ///< Text: the characters; Screenshot: the file to write

    /// True for the two events the application itself has to run: the input state cannot write a file
    /// or close a window.
    [[nodiscard]] bool is_application_action() const {
        return kind == Kind::Screenshot || kind == Kind::Quit;
    }
};

/// The key a name stands for: "F6", "DOWN", "A", "SPACE", "PAGEUP", "LEFTBRACKET", ... Case does not
/// matter, and neither does the punctuation some names carry ("F6", "f6", "PAGE_UP" and "PAGE-UP" are
/// the same key).
[[nodiscard]] std::optional<Key> key_from_name(std::string_view name);

/// Parses one command line. "key press X" produces two events - the press and the release - because
/// that is what a tap is; the frame loop applies them in order, and the press is still readable as a
/// press on that frame. Returns false and fills \p error when the line is not a command, in which case
/// nothing is appended.
[[nodiscard]] bool parse_input_line(std::string_view line, std::vector<InputEvent>& out, std::string* error);

/// A TCP server on the loopback interface that feeds commands to an InputState.
///
/// It owns one thread, which does the socket work and nothing else: every event it accepts is handed
/// to the frame loop through pump(), which applies it on the thread that owns the input state. The
/// reply to a command is sent once that has happened.
class InputServer {
public:
    /// Starts a server on 127.0.0.1:\p port; 0 asks the system for a free one. Returns nullptr and
    /// fills \p error when the socket cannot be opened.
    [[nodiscard]] static Scope<InputServer> create(u16 port, std::string* error);
    ~InputServer();
    ORE_NON_MOVABLE(InputServer);

    /// A socket handle, in the platform's own width: an int on POSIX, a SOCKET on Windows. It is an
    /// integer here so the class's layout does not depend on which headers the implementation pulls in.
    using SocketHandle = i64;

    /// The port actually bound. Worth reading when 0 was asked for.
    [[nodiscard]] u16 port() const { return port_; }
    [[nodiscard]] usize client_count() const { return clients_total_.load(); }
    [[nodiscard]] usize applied_count() const { return applied_.load(); }

    /// Applies every event that arrived since the last call to \p input, in order, and returns the
    /// ones the application has to run itself (a screenshot, a quit). Called from the frame loop, on
    /// the thread that owns the input state - never from the server's own thread.
    std::vector<InputEvent> pump(InputState& input);

private:
    InputServer() = default;
    void serve();
    /// Handles one complete line: parses it, queues it, and writes the reply. False when the socket is
    /// gone, which is how the server learns a client left.
    [[nodiscard]] bool serve_line(SocketHandle fd, const std::string& line);

    struct Pending {
        explicit Pending(std::vector<InputEvent> received) : events(std::move(received)) {}
        std::vector<InputEvent> events;
        std::atomic<bool> applied{false};
        /// Set when the client gave up waiting: the frame loop drops it instead of running it later,
        /// so "timeout" means the command did not happen rather than that it happened quietly.
        std::atomic<bool> cancelled{false};
    };
    struct Client {
        SocketHandle fd = -1;
        std::string line;   ///< what has arrived of the line being typed
    };

    /// Applies one event to \p input, and collects the ones the application has to run itself.
    static void apply(const InputEvent& event, InputState& input, std::vector<InputEvent>& actions);
    /// Queues \p events for the frame loop and waits for it to take them. Takes them by value: they
    /// are handed over, not shared with the caller's frame.
    [[nodiscard]] bool submit(std::vector<InputEvent> events);

    std::thread thread_;
    std::atomic<bool> running_{true};
    std::atomic<usize> clients_total_{0};
    std::atomic<usize> applied_{0};
    std::vector<Client> clients_;
    std::mutex mutex_;
    std::vector<Ref<Pending>> queue_;
    SocketHandle listen_fd_ = -1;
    u16 port_ = 0;
};

} // namespace ore::debug
