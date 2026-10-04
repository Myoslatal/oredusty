#include <ore/debug/input_server.h>

#include <ore/core/log.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ore::debug {
namespace {

#if defined(_WIN32)
using PollFd = WSAPOLLFD;
using NativeSocket = SOCKET;
constexpr int kSendFlags = 0;
/// Winsock has to be started before the first socket call; doing it twice is harmless.
void start_sockets() {
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
[[nodiscard]] std::string last_socket_error() {
    return std::system_category().message(static_cast<int>(::WSAGetLastError()));
}
#else
using PollFd = pollfd;
using NativeSocket = int;
constexpr int kSendFlags = MSG_NOSIGNAL;   // a client that hung up must not kill the process
void start_sockets() {}
[[nodiscard]] std::string last_socket_error() { return std::strerror(errno); }
#endif

using SocketHandle = InputServer::SocketHandle;

[[nodiscard]] NativeSocket native_of(SocketHandle handle) { return static_cast<NativeSocket>(handle); }
[[nodiscard]] SocketHandle portable_of(NativeSocket handle) { return static_cast<SocketHandle>(handle); }

/// Closing a socket has nothing to report: the handle is gone either way.
int close_socket(SocketHandle fd) {
#if defined(_WIN32)
    return ::closesocket(native_of(fd));
#else
    return ::close(native_of(fd));
#endif
}

[[nodiscard]] int poll_sockets(PollFd* fds, usize count, int timeout_ms) {
#if defined(_WIN32)
    return ::WSAPoll(fds, static_cast<ULONG>(count), timeout_ms);
#else
    return ::poll(fds, static_cast<nfds_t>(count), timeout_ms);
#endif
}

/// How long a command waits for a frame to pick it up before its client is told "timeout". It is
/// generous because a frame can be busy: writing a 3200x1885 screenshot takes seconds.
constexpr int kApplyTimeoutMs = 5000;
/// A line that never ends is not a command: a client sending one is cut off rather than buffered.
constexpr usize kMaxLineBytes = 4096;

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

/// Splits on spaces and tabs. The line is already trimmed.
[[nodiscard]] std::vector<std::string_view> words_of(std::string_view line) {
    std::vector<std::string_view> words;
    usize index = 0;
    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) ++index;
        const usize start = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') ++index;
        if (index > start) words.push_back(line.substr(start, index - start));
    }
    return words;
}

/// Uppercase, with the separators names are usually written with removed: "page_up", "PAGE-UP" and
/// "PageUp" are one name.
[[nodiscard]] std::string normalise(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char character : name) {
        if (character == '_' || character == '-') continue;
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }
    return out;
}

struct KeyName {
    std::string_view name;
    Key key;
};

/// Everything that is not a letter or a digit. Letters and digits are worked out from their own name,
/// because they are their own key codes.
constexpr KeyName kKeyNames[] = {
    {"SPACE", Key::Space},           {"APOSTROPHE", Key::Apostrophe}, {"COMMA", Key::Comma},
    {"MINUS", Key::Minus},           {"PERIOD", Key::Period},         {"SLASH", Key::Slash},
    {"SEMICOLON", Key::Semicolon},   {"EQUAL", Key::Equal},           {"LEFTBRACKET", Key::LeftBracket},
    {"BACKSLASH", Key::Backslash},   {"RIGHTBRACKET", Key::RightBracket},
    {"GRAVEACCENT", Key::GraveAccent},
    {"ESCAPE", Key::Escape},         {"ESC", Key::Escape},            {"ENTER", Key::Enter},
    {"RETURN", Key::Enter},          {"TAB", Key::Tab},               {"BACKSPACE", Key::Backspace},
    {"INSERT", Key::Insert},         {"DELETE", Key::Delete},         {"DEL", Key::Delete},
    {"RIGHT", Key::Right},           {"LEFT", Key::Left},             {"DOWN", Key::Down},
    {"UP", Key::Up},                 {"PAGEUP", Key::PageUp},         {"PAGEDOWN", Key::PageDown},
    {"PGUP", Key::PageUp},           {"PGDN", Key::PageDown},         {"HOME", Key::Home},
    {"END", Key::End},               {"CAPSLOCK", Key::CapsLock},     {"SCROLLLOCK", Key::ScrollLock},
    {"NUMLOCK", Key::NumLock},       {"PRINTSCREEN", Key::PrintScreen}, {"PAUSE", Key::Pause},
    {"F1", Key::F1},                 {"F2", Key::F2},                 {"F3", Key::F3},
    {"F4", Key::F4},                 {"F5", Key::F5},                 {"F6", Key::F6},
    {"F7", Key::F7},                 {"F8", Key::F8},                 {"F9", Key::F9},
    {"F10", Key::F10},               {"F11", Key::F11},               {"F12", Key::F12},
    {"KP0", Key::Kp0},               {"KP1", Key::Kp1},               {"KP2", Key::Kp2},
    {"KP3", Key::Kp3},               {"KP4", Key::Kp4},               {"KP5", Key::Kp5},
    {"KP6", Key::Kp6},               {"KP7", Key::Kp7},               {"KP8", Key::Kp8},
    {"KP9", Key::Kp9},               {"KPDECIMAL", Key::KpDecimal},   {"KPDIVIDE", Key::KpDivide},
    {"KPMULTIPLY", Key::KpMultiply}, {"KPSUBTRACT", Key::KpSubtract}, {"KPADD", Key::KpAdd},
    {"KPENTER", Key::KpEnter},       {"KPEQUAL", Key::KpEqual},       {"LEFTSHIFT", Key::LeftShift},
    {"LEFTCONTROL", Key::LeftControl}, {"LEFTCTRL", Key::LeftControl}, {"LEFTALT", Key::LeftAlt},
    {"LEFTSUPER", Key::LeftSuper},   {"LEFTMETA", Key::LeftSuper},    {"RIGHTSHIFT", Key::RightShift},
    {"RIGHTCONTROL", Key::RightControl}, {"RIGHTCTRL", Key::RightControl}, {"RIGHTALT", Key::RightAlt},
    {"RIGHTSUPER", Key::RightSuper}, {"RIGHTMETA", Key::RightSuper},  {"MENU", Key::Menu},
};

[[nodiscard]] std::optional<MouseButton> button_from_name(std::string_view name) {
    const std::string normalised = normalise(name);
    if (normalised == "LEFT") return MouseButton::Left;
    if (normalised == "RIGHT") return MouseButton::Right;
    if (normalised == "MIDDLE") return MouseButton::Middle;
    if (normalised == "BUTTON4") return MouseButton::Button4;
    if (normalised == "BUTTON5") return MouseButton::Button5;
    if (normalised == "BUTTON6") return MouseButton::Button6;
    if (normalised == "BUTTON7") return MouseButton::Button7;
    return std::nullopt;
}

[[nodiscard]] bool parse_number(std::string_view text, f32& out) {
    if (text.empty()) return false;
    std::string copy(text);
    char* end = nullptr;
    const f32 value = std::strtof(copy.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    out = value;
    return true;
}

[[nodiscard]] bool send_text(SocketHandle fd, const std::string& text) {
    usize sent = 0;
    while (sent < text.size()) {
        const auto count = ::send(native_of(fd), text.data() + sent, text.size() - sent, kSendFlags);
        if (count <= 0) return false;
        sent += static_cast<usize>(count);
    }
    return true;
}

} // namespace

std::optional<Key> key_from_name(std::string_view name) {
    const std::string normalised = normalise(name);
    if (normalised.size() == 1) {
        const char character = normalised[0];
        // The key codes are the ASCII letters and digits, so a one character name is its own answer.
        if (character >= 'A' && character <= 'Z') return static_cast<Key>(character);
        if (character >= '0' && character <= '9') return static_cast<Key>(character);
    }
    for (const KeyName& entry : kKeyNames) {
        if (normalised == entry.name) return entry.key;
    }
    return std::nullopt;
}

bool parse_input_line(std::string_view line, std::vector<InputEvent>& out, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    line = trim(line);
    // An empty line and a comment are not commands, and not mistakes either: a script may be written
    // with either in it.
    if (line.empty() || line.front() == '#') return true;

    const std::vector<std::string_view> words = words_of(line);
    const std::string_view command = words[0];

    if (command == "key") {
        if (words.size() != 3) return fail("key wants <down|up|press> <NAME>");
        const std::optional<Key> key = key_from_name(words[2]);
        if (!key.has_value()) return fail(std::format("unknown key '{}'", words[2]));
        const std::string_view what = words[1];
        const bool down = what == "down" || what == "press";
        const bool up = what == "up" || what == "press";
        if (!down && !up) return fail(std::format("key wants down, up or press, not '{}'", what));
        // A press is both halves of a tap, in order: the frame loop applies them together, and the
        // press is still readable as a press on that frame.
        if (down) out.push_back(InputEvent{.kind = InputEvent::Kind::Key, .key = *key, .down = true});
        if (up) out.push_back(InputEvent{.kind = InputEvent::Kind::Key, .key = *key, .down = false});
        return true;
    }

    if (command == "mouse") {
        if (words.size() < 2) return fail("mouse wants <move|down|up|click> ...");
        if (words[1] == "move") {
            if (words.size() != 4) return fail("mouse move wants <x> <y> in framebuffer pixels");
            InputEvent event{.kind = InputEvent::Kind::MouseMove};
            if (!parse_number(words[2], event.x) || !parse_number(words[3], event.y)) {
                return fail("mouse move wants two numbers");
            }
            out.push_back(event);
            return true;
        }
        const std::string_view what = words[1];
        const bool down = what == "down" || what == "click";
        const bool up = what == "up" || what == "click";
        if (!down && !up) return fail(std::format("mouse wants move, down, up or click, not '{}'", what));
        if (words.size() != 3) return fail("mouse down wants a button: left, right or middle");
        const std::optional<MouseButton> button = button_from_name(words[2]);
        if (!button.has_value()) return fail(std::format("unknown mouse button '{}'", words[2]));
        if (down) out.push_back(InputEvent{.kind = InputEvent::Kind::MouseButton, .button = *button, .down = true});
        if (up) out.push_back(InputEvent{.kind = InputEvent::Kind::MouseButton, .button = *button, .down = false});
        return true;
    }

    if (command == "scroll") {
        if (words.size() != 3) return fail("scroll wants <x> <y>");
        InputEvent event{.kind = InputEvent::Kind::Scroll};
        if (!parse_number(words[1], event.x) || !parse_number(words[2], event.y)) {
            return fail("scroll wants two numbers");
        }
        out.push_back(event);
        return true;
    }

    if (command == "text") {
        // Everything after the command, verbatim: text is allowed to contain spaces.
        const usize space = line.find_first_of(" \t");
        const std::string_view rest = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space + 1));
        if (rest.empty()) return fail("text wants the characters to type");
        out.push_back(InputEvent{.kind = InputEvent::Kind::Text, .text = std::string(rest)});
        return true;
    }

    if (command == "shot") {
        if (words.size() != 2) return fail("shot wants <path.png>");
        out.push_back(InputEvent{.kind = InputEvent::Kind::Screenshot, .text = std::string(words[1])});
        return true;
    }

    if (command == "quit") {
        if (words.size() != 1) return fail("quit takes nothing");
        out.push_back(InputEvent{.kind = InputEvent::Kind::Quit});
        return true;
    }

    return fail(std::format("unknown command '{}' (key, mouse, scroll, text, shot, quit)", command));
}

Scope<InputServer> InputServer::create(u16 port, std::string* error) {
    const auto fail = [&](std::string message) -> Scope<InputServer> {
        if (error != nullptr) *error = std::move(message);
        return nullptr;
    };
    start_sockets();
    const NativeSocket raw = ::socket(AF_INET, SOCK_STREAM, 0);
    if (raw == static_cast<NativeSocket>(-1)
#if defined(_WIN32)
        || raw == INVALID_SOCKET
#endif
    ) {
        return fail(std::format("socket(): {}", last_socket_error()));
    }
    const SocketHandle fd = portable_of(raw);
    int one = 1;
    ::setsockopt(raw, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    // Loopback only: a debug facility that presses keys on the machine it runs on has no business
    // listening to the network.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(raw, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string message = std::format("bind(127.0.0.1:{}): {}", port, last_socket_error());
        close_socket(fd);
        return fail(message);
    }
    if (::listen(raw, 4) != 0) {
        const std::string message = std::format("listen(): {}", last_socket_error());
        close_socket(fd);
        return fail(message);
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (::getsockname(raw, reinterpret_cast<sockaddr*>(&bound), &length) == 0) port = ntohs(bound.sin_port);

    Scope<InputServer> server(new InputServer());
    server->listen_fd_ = fd;
    server->port_ = port;
    server->thread_ = std::thread([self = server.get()] { self->serve(); });
    return server;
}

InputServer::~InputServer() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    {
        // Anything still waiting is released: a client gets its "timeout" rather than a hang.
        std::lock_guard<std::mutex> lock(mutex_);
        for (const Ref<Pending>& pending : queue_) pending->applied = true;
        queue_.clear();
    }
    if (listen_fd_ >= 0) close_socket(listen_fd_);
    listen_fd_ = -1;
}

void InputServer::serve() {
    std::vector<PollFd> fds;
    while (running_.load()) {
        fds.clear();
        fds.push_back(PollFd{native_of(listen_fd_), POLLIN, 0});
        for (const Client& client : clients_) fds.push_back(PollFd{native_of(client.fd), POLLIN, 0});
        const int ready = poll_sockets(fds.data(), fds.size(), 100);
        if (ready <= 0) continue;   // nothing yet, or a signal: either way, look again

        if ((fds[0].revents & POLLIN) != 0) {
            const NativeSocket accepted = ::accept(native_of(listen_fd_), nullptr, nullptr);
            if (accepted != static_cast<NativeSocket>(-1)
#if defined(_WIN32)
                && accepted != INVALID_SOCKET
#endif
            ) {
                clients_.push_back(Client{portable_of(accepted), {}});
                clients_total_.fetch_add(1);
                ORE_INFO("debug input server: client {} connected", clients_total_.load());
            }
        }

        // Only the clients that were in this poll: one accepted just above is polled next time round,
        // and reading its slot now would read past the end of the poll list.
        const usize polled = fds.size() - 1;
        std::vector<usize> dead;
        for (usize index = 0; index < polled && index < clients_.size(); ++index) {
            const short revents = fds[index + 1].revents;
            bool drop = (revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
            if (!drop && (revents & POLLIN) != 0) {
                char buffer[512];
                const auto count = ::recv(native_of(clients_[index].fd), buffer, sizeof(buffer), 0);
                if (count <= 0) drop = true;
                else clients_[index].line.append(buffer, static_cast<usize>(count));
            }
            if (!drop) {
                // A line at a time: one write may carry several commands.
                while (true) {
                    const usize newline = clients_[index].line.find('\n');
                    if (newline == std::string::npos) break;
                    const std::string line = clients_[index].line.substr(0, newline);
                    clients_[index].line.erase(0, newline + 1);
                    if (!serve_line(clients_[index].fd, line)) {
                        drop = true;
                        break;
                    }
                }
                if (!drop && clients_[index].line.size() > kMaxLineBytes) drop = true;
            }
            if (drop) {
                close_socket(clients_[index].fd);
                dead.push_back(index);
            }
        }
        for (usize index = dead.size(); index > 0; --index) {
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(dead[index - 1]));
        }
    }
    for (const Client& client : clients_) close_socket(client.fd);
    clients_.clear();
}

bool InputServer::serve_line(SocketHandle fd, const std::string& line) {
    std::vector<InputEvent> events;
    std::string error;
    if (!parse_input_line(line, events, &error)) {
        ORE_WARN("debug input server: {}", error);
        return send_text(fd, std::format("error {}\n", error));
    }
    if (events.empty()) return send_text(fd, "ok\n");
    const std::string_view shown = trim(line);
    if (!submit(events)) return send_text(fd, "timeout\n");
    // The application's own log is the evidence a script cannot fake: it says what it was told to do.
    ORE_INFO("debug input: {}", shown);
    return send_text(fd, "ok\n");
}

bool InputServer::submit(std::vector<InputEvent> events) {
    Ref<Pending> pending = make_ref<Pending>(std::move(events));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) return false;
        queue_.push_back(pending);
    }
    // The reply waits for the frame loop: "ok" has to mean "the application has seen this", or a
    // screenshot taken after it would be a screenshot of the frame before it.
    for (int waited = 0; waited < kApplyTimeoutMs; ++waited) {
        if (pending->applied.load()) return true;
        if (!running_.load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Nothing picked it up in time. Dropping it is the honest answer to "timeout": a command that
    // took effect a second later than its client was told it would not is worse than one that did
    // not happen at all.
    pending->cancelled = true;
    return pending->applied.load();
}

std::vector<InputEvent> InputServer::pump(InputState& input) {
    std::vector<Ref<Pending>> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(queue_);
    }
    std::vector<InputEvent> actions;
    for (const Ref<Pending>& pending : batch) {
        if (pending->cancelled.load()) continue;   // its client was already told "timeout"
        for (const InputEvent& event : pending->events) apply(event, input, actions);
        applied_.fetch_add(pending->events.size());
        pending->applied = true;
    }
    return actions;
}

void InputServer::apply(const InputEvent& event, InputState& input, std::vector<InputEvent>& actions) {
    switch (event.kind) {
        case InputEvent::Kind::Key: input.set_key(event.key, event.down); break;
        case InputEvent::Kind::MouseButton: input.set_mouse_button(event.button, event.down); break;
        case InputEvent::Kind::MouseMove:
            // The pointer is stored as a framebuffer pixel, so a scale of 1 is the whole conversion:
            // the numbers in the command are the ones the application draws with. The first move also
            // puts the pointer inside the window, and - like a real pointer arriving - carries no
            // delta, so a jump does not drag the view with it.
            if (!input.cursor_inside_window()) input.set_cursor_inside(true);
            input.add_pointer_event(event.x, event.y, PixelScale{});
            break;
        case InputEvent::Kind::Scroll: input.add_scroll(event.x, event.y); break;
        case InputEvent::Kind::Text: input.add_text(event.text); break;
        case InputEvent::Kind::Screenshot:
        case InputEvent::Kind::Quit: actions.push_back(event); break;
    }
}

} // namespace ore::debug
