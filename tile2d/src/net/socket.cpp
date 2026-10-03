#include <t2d/net/socket.h>

#include <t2d/core/log.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace t2d::net {

std::string Endpoint::to_string() const {
    std::array<char, INET_ADDRSTRLEN> text{};
    in_addr addr{};
    addr.s_addr = address;
    if (::inet_ntop(AF_INET, &addr, text.data(), text.size()) == nullptr) return "0.0.0.0:0";
    return std::format("{}:{}", text.data(), port);
}

std::optional<Endpoint> Endpoint::parse(std::string_view value, u16 default_port) {
    const usize colon = value.find(':');
    Endpoint endpoint;
    endpoint.port = default_port;
    std::string host(value.substr(0, colon));
    if (colon != std::string_view::npos) {
        const std::string_view port_text = value.substr(colon + 1);
        if (!port_text.empty()) {
            try {
                const int port = std::stoi(std::string(port_text));
                if (port <= 0 || port > 65535) return std::nullopt;
                endpoint.port = static_cast<u16>(port);
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    if (host.empty()) host = "0.0.0.0";
    if (host == "localhost") host = "127.0.0.1";
    in_addr addr{};
    if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) return std::nullopt;
    endpoint.address = addr.s_addr;
    return endpoint;
}

Scope<UdpSocket> UdpSocket::create() {
    Scope<UdpSocket> socket(new UdpSocket());
    socket->handle_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket->handle_ < 0) {
        T2D_ERROR("socket(): {}", std::strerror(errno));
        return nullptr;
    }
    const int flags = ::fcntl(socket->handle_, F_GETFL, 0);
    if (flags >= 0) ::fcntl(socket->handle_, F_SETFL, flags | O_NONBLOCK);
    const int one = 1;
    ::setsockopt(socket->handle_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    return socket;
}

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::bind(u16 port) {
    if (handle_ < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (::bind(handle_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        T2D_ERROR("bind(udp:{}): {}", port, std::strerror(errno));
        return false;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        local_port_ = ntohs(bound.sin_port);
    } else {
        local_port_ = port;
    }
    return true;
}

bool UdpSocket::enable_broadcast() {
    if (handle_ < 0) return false;
    const int one = 1;
    return ::setsockopt(handle_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) == 0;
}

bool UdpSocket::send_to(const Endpoint& peer, ConstSpan<const u8> data) {
    if (handle_ < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = peer.address;
    address.sin_port = htons(peer.port);
    const ssize_t sent = ::sendto(handle_, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&address),
                                  sizeof(address));
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return false;
        T2D_DEBUG("sendto({}) failed: {}", peer.to_string(), std::strerror(errno));
        return false;
    }
    return static_cast<usize>(sent) == data.size();
}

usize UdpSocket::receive_from(Endpoint& source, Span<u8> buffer) {
    if (handle_ < 0) return 0;
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    const ssize_t received =
        ::recvfrom(handle_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&address), &length);
    if (received <= 0) return 0;
    source.address = address.sin_addr.s_addr;
    source.port = ntohs(address.sin_port);
    return static_cast<usize>(received);
}

bool UdpSocket::wait_readable(u32 timeout_ms) const {
    if (handle_ < 0) return false;
    pollfd descriptor{};
    descriptor.fd = handle_;
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, static_cast<int>(timeout_ms));
    return result > 0 && (descriptor.revents & POLLIN) != 0;
}

void UdpSocket::close() {
    if (handle_ >= 0) {
        ::close(handle_);
        handle_ = -1;
    }
}

} // namespace t2d::net
