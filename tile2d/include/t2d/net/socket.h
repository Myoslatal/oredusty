// Tile2D - minimal non blocking IPv4 UDP sockets (POSIX).
#pragma once

#include <t2d/core/types.h>

#include <optional>
#include <string>
#include <string_view>

namespace t2d::net {

/// IPv4 endpoint. Address is kept in network byte order, port in host byte order.
struct Endpoint {
    u32 address = 0;
    u16 port = 0;

    friend constexpr bool operator==(const Endpoint&, const Endpoint&) = default;
    [[nodiscard]] std::string to_string() const;
    /// Parses "1.2.3.4:5678", "1.2.3.4" (with \p default_port) or ":5678".
    [[nodiscard]] static std::optional<Endpoint> parse(std::string_view text, u16 default_port = 0);
};

struct EndpointHash {
    usize operator()(const Endpoint& endpoint) const noexcept {
        return (static_cast<usize>(endpoint.address) << 16) ^ endpoint.port;
    }
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    T2D_NON_COPYABLE(UdpSocket);

    /// Creates a non blocking UDP socket. Returns nullptr when the system call fails.
    [[nodiscard]] static Scope<UdpSocket> create();
    /// Binds to \p port (0 = let the system choose) and returns false on failure.
    bool bind(u16 port);
    /// Joins the multicast group 239.x.x.x (used by LAN discovery; optional).
    bool enable_broadcast();

    [[nodiscard]] bool valid() const { return handle_ >= 0; }
    [[nodiscard]] int native_handle() const { return handle_; }
    [[nodiscard]] u16 local_port() const { return local_port_; }

    /// Sends one datagram. Returns false when the socket would block or the syscall failed.
    [[nodiscard]] bool send_to(const Endpoint& peer, ConstSpan<const u8> data);
    /// Receives one datagram. Returns the number of bytes, or 0 when nothing is pending.
    [[nodiscard]] usize receive_from(Endpoint& source, Span<u8> buffer);
    /// Waits for readability (poll) up to \p timeout_ms.
    [[nodiscard]] bool wait_readable(u32 timeout_ms) const;

    void close();

private:
    int handle_ = -1;
    u16 local_port_ = 0;
};

} // namespace t2d::net
