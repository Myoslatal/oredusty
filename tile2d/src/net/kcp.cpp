// Tile2D - KCP: an ARQ (automatic repeat request) protocol over UDP.
//
// This is a reimplementation of the classic ikcp algorithm (skywind3000/kcp) on top of the
// framework's containers. The wire format is byte identical to the reference implementation
// (24 byte header, little endian, the same four commands) so C, C#, Go and Rust ports interoperate.
//
// Everything the protocol decides is derived from the timestamps passed to update()/input() - no
// clock is read here - which keeps the behaviour reproducible in tests and in replays.

#include <t2d/net/kcp.h>

#include <t2d/core/log.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

namespace t2d::net {
namespace {

constexpr u32 kOverhead = kKcpHeaderSize;
constexpr u32 kMtuMin = 50;      // ikcp refuses anything smaller
constexpr u32 kMtuMax = 65507;   // largest payload a UDP datagram can carry
constexpr u32 kIntervalMin = 10;
constexpr u32 kIntervalMax = 5000;
constexpr u32 kRtoDef = 200;     // initial RTO before the first RTT sample (IKCP_RTO_DEF)
constexpr u32 kRtoMax = 60000;
constexpr u32 kThreshInit = 2;
constexpr u32 kThreshMin = 2;
constexpr u32 kProbeInit = 7000;  // first window probe when the peer advertises a zero window
constexpr u32 kProbeLimit = 120000;
constexpr u32 kAskSend = 1;
constexpr u32 kAskTell = 2;
constexpr u32 kNoFastResend = 0xFFFF'FFFFu;
constexpr u32 kMaxFragments = 255;  // frg is one byte
constexpr u32 kQueueFloor = 256;    // segments a caller may queue even with a tiny send window

/// Wrap safe comparison of two millisecond timestamps, identical to ikcp's _itimediff: the result
/// is positive when \p later is newer than \p earlier.
[[nodiscard]] constexpr i32 time_diff(u32 later, u32 earlier) { return static_cast<i32>(later - earlier); }

[[nodiscard]] constexpr u32 min_u32(u32 a, u32 b) { return a < b ? a : b; }
[[nodiscard]] constexpr u32 max_u32(u32 a, u32 b) { return a > b ? a : b; }
[[nodiscard]] constexpr u32 bound_u32(u32 low, u32 value, u32 high) { return min_u32(max_u32(low, value), high); }

void write_u16(std::vector<u8>& out, u16 value) {
    out.push_back(static_cast<u8>(value & 0xFFu));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
}

void write_u32(std::vector<u8>& out, u32 value) {
    for (u32 i = 0; i < 4; ++i) out.push_back(static_cast<u8>((value >> (i * 8)) & 0xFFu));
}

[[nodiscard]] u16 read_u16(const u8* data) {
    return static_cast<u16>(static_cast<u16>(data[0]) | static_cast<u16>(static_cast<u16>(data[1]) << 8));
}

[[nodiscard]] u32 read_u32(const u8* data) {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8) | (static_cast<u32>(data[2]) << 16) |
           (static_cast<u32>(data[3]) << 24);
}

/// KCP segments live in four intrusive lists in ikcp; here they are plain values in deques.
struct Segment {
    std::vector<u8> data;
    u32 cmd = 0;
    u32 frg = 0;
    u32 wnd = 0;
    u32 ts = 0;
    u32 sn = 0;
    u32 una = 0;
    u32 resendts = 0;
    u32 rto = 0;
    u32 fastack = 0;
    u32 xmit = 0;
};

void encode_segment(std::vector<u8>& out, u32 conv, KcpCommand cmd, u32 frg, u32 wnd, u32 ts, u32 sn, u32 una,
                    ConstSpan<const u8> payload) {
    write_u32(out, conv);
    out.push_back(static_cast<u8>(cmd));
    out.push_back(static_cast<u8>(frg));
    write_u16(out, static_cast<u16>(wnd));
    write_u32(out, ts);
    write_u32(out, sn);
    write_u32(out, una);
    write_u32(out, static_cast<u32>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

/// Hard limit on the number of segments a caller may pile up: a misbehaving caller must not be able
/// to grow the process without bound, but the limit is generous enough to keep a 256 segment pipe
/// busy while the window opens.
[[nodiscard]] constexpr usize send_queue_capacity(u32 send_window) {
    const usize scaled = static_cast<usize>(send_window) * 8;
    return scaled < kQueueFloor ? kQueueFloor : scaled;
}

} // namespace

struct Kcp::Impl {
    explicit Impl(const KcpConfig& config) { reconfigure(config); }

    KcpConfig cfg{};
    mutable KcpStats st{};
    OutputFn output;

    u32 mss = 0;
    u32 snd_una = 0;
    u32 snd_nxt = 0;
    u32 rcv_nxt = 0;
    u32 ssthresh = kThreshInit;
    u32 incr = 0;
    u32 snd_wnd = 0;
    u32 rcv_wnd = 0;
    u32 rmt_wnd = 0;
    u32 cwnd = 1;
    u32 probe = 0;
    u32 current = 0;
    u32 interval = kIntervalMin;
    u32 ts_flush = 0;
    u32 ts_probe = 0;
    u32 probe_wait = 0;
    u32 dead_link = 20;
    i32 rx_rttval = 0;
    i32 rx_srtt = 0;
    i32 rx_rto = static_cast<i32>(kRtoDef);
    i32 rx_minrto = 30;
    bool started = false;  // update() has been called at least once
    bool dead = false;

    u32 nsnd_que = 0;
    u32 nsnd_buf = 0;
    u32 nrcv_que = 0;
    u32 nrcv_buf = 0;
    usize rcv_que_bytes = 0;

    std::deque<Segment> snd_queue;
    std::deque<Segment> snd_buf;
    std::deque<Segment> rcv_queue;
    std::deque<Segment> rcv_buf;
    std::vector<std::pair<u32, u32>> acklist;  // (sn, ts) pairs waiting for the next flush
    std::vector<u8> buffer;

    // -- configuration --------------------------------------------------------

    void reconfigure(const KcpConfig& config) {
        cfg = config;
        set_mtu(config.mtu);
        snd_wnd = config.send_window == 0 ? 1u : static_cast<u32>(config.send_window);
        rcv_wnd = config.receive_window == 0 ? 1u : static_cast<u32>(config.receive_window);
        rmt_wnd = config.receive_window == 0 ? 1u : static_cast<u32>(config.receive_window);
        interval = bound_u32(kIntervalMin, config.interval_ms, kIntervalMax);
        rx_minrto = static_cast<i32>(config.min_rto_ms == 0 ? 1u : config.min_rto_ms);
        rx_rto = static_cast<i32>(bound_u32(static_cast<u32>(rx_minrto), kRtoDef, kRtoMax));
        dead_link = config.dead_link_threshold == 0 ? 1u : config.dead_link_threshold;
        ssthresh = kThreshInit;
        cwnd = 1;
        incr = mss;
    }

    void set_mtu(u32 mtu) {
        cfg.mtu = bound_u32(kMtuMin, mtu, kMtuMax);
        mss = cfg.mtu - kOverhead;
        buffer.reserve(cfg.mtu);
    }

    void set_window_size(u16 send_window, u16 receive_window) {
        snd_wnd = send_window == 0 ? 1u : static_cast<u32>(send_window);
        rcv_wnd = receive_window == 0 ? 1u : static_cast<u32>(receive_window);
    }

    void set_nodelay(bool no_delay, u32 interval_ms, u32 fast_resend, bool no_congestion_control) {
        cfg.no_delay = no_delay;
        cfg.interval_ms = bound_u32(kIntervalMin, interval_ms, kIntervalMax);
        cfg.fast_resend_threshold = fast_resend;
        cfg.no_congestion_control = no_congestion_control;
        interval = cfg.interval_ms;
    }

    // -- helpers --------------------------------------------------------------

    [[nodiscard]] usize max_message_size() const { return static_cast<usize>(mss) * kMaxFragments; }

    [[nodiscard]] u32 wnd_unused() const { return nrcv_que < rcv_wnd ? rcv_wnd - nrcv_que : 0; }

    /// The congestion window the flush uses; no-cc mode always keeps the full send window.
    [[nodiscard]] u32 effective_cwnd() const {
        u32 window = min_u32(snd_wnd, rmt_wnd);
        if (!cfg.no_congestion_control) window = min_u32(cwnd, window);
        return window;
    }

    void emit(ConstSpan<const u8> packet) {
        if (packet.empty()) return;
        if (output) output(packet);
        ++st.output_packets;
        st.output_bytes += packet.size();
    }

    void emit_buffer() {
        if (buffer.empty()) return;
        emit(ConstSpan<const u8>(buffer.data(), buffer.size()));
        buffer.clear();
    }

    [[nodiscard]] bool fits(u32 need) const { return static_cast<u64>(buffer.size()) + need <= cfg.mtu; }

    [[nodiscard]] u32 queued_receive_bytes() const {
        return static_cast<u32>(rcv_que_bytes > 0xFFFF'FFFFull ? 0xFFFF'FFFFull : rcv_que_bytes);
    }

    // -- sending --------------------------------------------------------------

    bool send(ConstSpan<const u8> data) {
        if (data.empty()) return false;  // a zero length message cannot be told apart from "nothing"
        if (data.size() > max_message_size()) return false;
        const usize fragments = (data.size() + mss - 1) / mss;
        if (fragments > kMaxFragments) return false;
        const usize queued = static_cast<usize>(nsnd_que) + static_cast<usize>(nsnd_buf);
        if (queued + fragments > send_queue_capacity(snd_wnd)) return false;

        usize offset = 0;
        for (usize i = 0; i < fragments; ++i) {
            const usize size = std::min<usize>(mss, data.size() - offset);
            Segment segment;
            segment.data.assign(data.begin() + static_cast<isize>(offset),
                                data.begin() + static_cast<isize>(offset + size));
            // Fragments count down so the receiver can spot the first one; stream mode has no
            // message boundaries, every fragment is final.
            segment.frg = cfg.stream_mode ? 0u : static_cast<u32>(fragments - i - 1);
            offset += size;
            snd_queue.push_back(std::move(segment));
            ++nsnd_que;
        }
        return true;
    }

    // -- receive queue --------------------------------------------------------

    [[nodiscard]] usize peek_size() const {
        if (rcv_queue.empty()) return 0;
        if (cfg.stream_mode) return rcv_que_bytes;
        const Segment& first = rcv_queue.front();
        if (first.frg == 0) return first.data.size();
        const usize need = static_cast<usize>(first.frg) + 1;
        if (rcv_queue.size() < need) return 0;  // the message is still missing fragments
        usize total = 0;
        for (usize i = 0; i < need; ++i) {
            total += rcv_queue[i].data.size();
            if (rcv_queue[i].frg == 0) return total;
        }
        return 0;  // no terminator in the window: treat as incomplete instead of guessing
    }

    usize receive(Span<u8> destination) {
        if (destination.empty() || rcv_queue.empty()) return 0;
        if (cfg.stream_mode) {
            const usize take = std::min(destination.size(), rcv_que_bytes);
            usize written = 0;
            while (written < take) {
                Segment& front = rcv_queue.front();
                const usize chunk = std::min(take - written, front.data.size());
                std::memcpy(destination.data() + written, front.data.data(), chunk);
                written += chunk;
                if (chunk == front.data.size()) {
                    rcv_que_bytes -= front.data.size();
                    rcv_queue.pop_front();
                    --nrcv_que;
                } else {
                    front.data.erase(front.data.begin(), front.data.begin() + static_cast<isize>(chunk));
                    rcv_que_bytes -= chunk;
                }
            }
            return written;
        }

        const usize size = peek_size();
        if (size == 0 || destination.size() < size) return 0;
        usize written = 0;
        while (written < size) {
            Segment& front = rcv_queue.front();
            const usize chunk = std::min(size - written, front.data.size());
            std::memcpy(destination.data() + written, front.data.data(), chunk);
            written += chunk;
            if (chunk == front.data.size()) {
                rcv_que_bytes -= front.data.size();
                rcv_queue.pop_front();
                --nrcv_que;
            } else {
                front.data.erase(front.data.begin(), front.data.begin() + static_cast<isize>(chunk));
                rcv_que_bytes -= chunk;
            }
        }
        return written;
    }

    // -- protocol bookkeeping -------------------------------------------------

    void shrink_buf() { snd_una = snd_buf.empty() ? snd_nxt : snd_buf.front().sn; }

    void parse_una(u32 una) {
        while (!snd_buf.empty() && time_diff(una, snd_buf.front().sn) > 0) {
            snd_buf.pop_front();
            --nsnd_buf;
        }
    }

    /// Removes the segment acknowledged by \p sn; false when nothing was waiting for that ACK.
    bool parse_ack(u32 sn) {
        if (time_diff(sn, snd_una) < 0 || time_diff(sn, snd_nxt) >= 0) return false;
        for (usize i = 0; i < snd_buf.size(); ++i) {
            if (snd_buf[i].sn == sn) {
                snd_buf.erase(snd_buf.begin() + static_cast<isize>(i));
                --nsnd_buf;
                return true;
            }
            if (time_diff(sn, snd_buf[i].sn) < 0) break;
        }
        return false;
    }

    /// Every segment older than the newest acknowledged one was skipped by the peer, which is the
    /// duplicate ACK signal fast retransmission is built on.
    void parse_fastack(u32 sn) {
        if (time_diff(sn, snd_una) < 0 || time_diff(sn, snd_nxt) >= 0) return;
        for (usize i = 0; i < snd_buf.size(); ++i) {
            Segment& segment = snd_buf[i];
            if (time_diff(sn, segment.sn) < 0) break;
            if (sn != segment.sn) ++segment.fastack;
        }
    }

    void update_ack(i32 rtt) {
        if (rtt <= 0) rtt = 1;
        if (rtt > static_cast<i32>(kRtoMax)) return;  // a bogus timestamp (foreign clock): ignore it
        if (rx_srtt == 0) {
            rx_srtt = rtt;
            rx_rttval = rtt / 2;
        } else {
            i32 delta = rtt - rx_srtt;
            if (delta < 0) delta = -delta;
            rx_rttval = (3 * rx_rttval + delta) / 4;
            rx_srtt = (7 * rx_srtt + rtt) / 8;
            if (rx_srtt < 1) rx_srtt = 1;
        }
        const i32 variation = 4 * rx_rttval;
        const i32 rto = rx_srtt + static_cast<i32>(max_u32(interval, static_cast<u32>(variation)));
        rx_rto = static_cast<i32>(bound_u32(static_cast<u32>(rx_minrto), static_cast<u32>(rto), kRtoMax));
    }

    void ack_push(u32 sn, u32 ts) { acklist.emplace_back(sn, ts); }

    void parse_data(Segment&& segment) {
        const u32 sn = segment.sn;
        if (time_diff(sn, rcv_nxt + rcv_wnd) >= 0 || time_diff(sn, rcv_nxt) < 0) return;

        usize position = rcv_buf.size();
        for (usize i = 0; i < rcv_buf.size(); ++i) {
            const i32 relative = time_diff(sn, rcv_buf[i].sn);
            if (relative == 0) return;  // duplicate: the peer already sent this one
            if (relative < 0) {
                position = i;
                break;
            }
        }
        rcv_buf.insert(rcv_buf.begin() + static_cast<isize>(position), std::move(segment));
        ++nrcv_buf;

        // Hand everything that is now contiguous to the receive queue. Zero length segments are
        // consumed (they still own a sequence number) but never queued: they would make receive()
        // report an empty message forever.
        while (!rcv_buf.empty() && rcv_buf.front().sn == rcv_nxt) {
            if (rcv_buf.front().data.empty()) {
                rcv_buf.pop_front();
                --nrcv_buf;
                ++rcv_nxt;
                continue;
            }
            if (nrcv_que >= rcv_wnd) break;  // window full: leave the rest in rcv_buf
            Segment ready = std::move(rcv_buf.front());
            rcv_buf.pop_front();
            --nrcv_buf;
            rcv_que_bytes += ready.data.size();
            rcv_queue.push_back(std::move(ready));
            ++nrcv_que;
            ++rcv_nxt;
        }
    }

    // -- flush ----------------------------------------------------------------

    void flush() {
        const u32 now = current;
        const u32 wnd = wnd_unused();
        buffer.clear();

        // 1. acknowledgements, packed into as few datagrams as the mtu allows (as ikcp does).
        for (const std::pair<u32, u32>& ack : acklist) {
            if (!fits(kOverhead)) emit_buffer();
            encode_segment(buffer, cfg.conv, KcpCommand::Ack, 0, wnd, ack.second, ack.first, rcv_nxt, {});
            ++st.segments_sent;
        }
        acklist.clear();

        // 2. window probes: only needed while the peer claims to have no room at all.
        if (rmt_wnd == 0) {
            if (probe_wait == 0) {
                probe_wait = kProbeInit;
                ts_probe = now + probe_wait;
            } else if (time_diff(now, ts_probe) >= 0) {
                if (probe_wait < kProbeInit) probe_wait = kProbeInit;
                probe_wait += probe_wait / 2;
                if (probe_wait > kProbeLimit) probe_wait = kProbeLimit;
                ts_probe = now + probe_wait;
                probe |= kAskSend;
            }
        } else {
            ts_probe = 0;
            probe_wait = 0;
        }
        if ((probe & kAskSend) != 0) {
            if (!fits(kOverhead)) emit_buffer();
            encode_segment(buffer, cfg.conv, KcpCommand::Wask, 0, wnd, now, 0, rcv_nxt, {});
            ++st.segments_sent;
        }
        if ((probe & kAskTell) != 0) {
            if (!fits(kOverhead)) emit_buffer();
            encode_segment(buffer, cfg.conv, KcpCommand::Wins, 0, wnd, now, 0, rcv_nxt, {});
            ++st.segments_sent;
        }
        probe = 0;

        // 3. move freshly queued segments into the in flight list, respecting the windows.
        const u32 window = effective_cwnd();
        while (!snd_queue.empty() && time_diff(snd_nxt, snd_una + window) < 0) {
            Segment segment = std::move(snd_queue.front());
            snd_queue.pop_front();
            --nsnd_que;
            segment.cmd = static_cast<u32>(KcpCommand::Push);
            segment.wnd = wnd;
            segment.ts = now;
            segment.sn = snd_nxt++;
            segment.una = rcv_nxt;
            segment.resendts = now;
            segment.rto = static_cast<u32>(rx_rto);
            segment.fastack = 0;
            segment.xmit = 0;
            snd_buf.push_back(std::move(segment));
            ++nsnd_buf;
        }

        // 4. first transmission, timeout retransmission and fast retransmission.
        const u32 resent = cfg.fast_resend_threshold > 0 ? cfg.fast_resend_threshold : kNoFastResend;
        const u32 rtomin = cfg.no_delay ? 0u : static_cast<u32>(rx_rto) >> 3;
        u32 change = 0;
        bool lost = false;
        for (usize i = 0; i < snd_buf.size(); ++i) {
            Segment& segment = snd_buf[i];
            bool needsend = false;
            if (segment.xmit == 0) {
                needsend = true;
                segment.xmit = 1;
                segment.rto = static_cast<u32>(rx_rto);
                segment.resendts = now + segment.rto + rtomin;
            } else if (time_diff(now, segment.resendts) >= 0) {
                needsend = true;
                ++segment.xmit;
                ++st.retransmissions;
                if (!cfg.no_delay) {
                    segment.rto += max_u32(segment.rto, static_cast<u32>(rx_rto));  // exponential backoff
                } else {
                    segment.rto += segment.rto / 2;
                }
                segment.resendts = now + segment.rto;
                lost = true;
            } else if (segment.fastack >= resent) {
                needsend = true;
                ++segment.xmit;
                ++st.fast_retransmissions;
                segment.fastack = 0;
                segment.resendts = now + segment.rto;
                ++change;
            }
            if (!needsend) continue;
            segment.ts = now;
            segment.wnd = wnd;
            segment.una = rcv_nxt;
            if (!fits(kOverhead + static_cast<u32>(segment.data.size()))) emit_buffer();
            encode_segment(buffer, cfg.conv, KcpCommand::Push, segment.frg, segment.wnd, segment.ts, segment.sn,
                           segment.una, segment.data);
            ++st.segments_sent;
            // xmit counts the first transmission too: the link is dead once a segment has been
            // retransmitted dead_link_threshold times (the wording of the header's contract).
            if (segment.xmit > dead_link) dead = true;
        }
        emit_buffer();

        if (!cfg.no_congestion_control && change > 0) {
            if (cwnd > rmt_wnd) cwnd = rmt_wnd;
            ssthresh = cwnd / 2;
            if (ssthresh < kThreshMin) ssthresh = kThreshMin;
            cwnd = ssthresh + resent;
            if (cwnd > snd_wnd) cwnd = snd_wnd;
            incr = cwnd * mss;
        }
        if (!cfg.no_congestion_control && lost) {
            ssthresh = window / 2;
            if (ssthresh < kThreshMin) ssthresh = kThreshMin;
            cwnd = 1;
            incr = mss;
        }
        if (cwnd < 1) {
            cwnd = 1;
            incr = mss;
        }
    }

    // -- input ----------------------------------------------------------------

    /// Validates a whole datagram before any state is touched: a bad segment anywhere means the
    /// packet is rejected as a unit, which keeps a corrupt peer from half-applying a datagram.
    [[nodiscard]] bool valid_packet(ConstSpan<const u8> packet) const {
        if (packet.size() < kOverhead) return false;
        usize offset = 0;
        while (offset < packet.size()) {
            if (packet.size() - offset < kOverhead) return false;
            const u8* header = packet.data() + offset;
            const u8 cmd = header[4];
            if (read_u32(header) != cfg.conv) return false;
            if (cmd != static_cast<u8>(KcpCommand::Push) && cmd != static_cast<u8>(KcpCommand::Ack) &&
                cmd != static_cast<u8>(KcpCommand::Wask) && cmd != static_cast<u8>(KcpCommand::Wins)) {
                return false;
            }
            const u32 len = read_u32(header + 20);
            const usize remaining = packet.size() - offset - kOverhead;
            if (static_cast<u64>(len) > remaining) return false;
            offset += kOverhead + len;
        }
        return true;
    }

    bool input(ConstSpan<const u8> packet) {
        if (!valid_packet(packet)) {
            ++st.dropped_malformed;
            return false;
        }

        const u32 now = current;
        const u32 previous_una = snd_una;
        bool acked = false;
        u32 max_ack = 0;

        usize offset = 0;
        while (offset < packet.size()) {
            const u8* header = packet.data() + offset;
            const u8 cmd = header[4];
            const u32 frg = header[5];
            const u16 wnd = read_u16(header + 6);
            const u32 ts = read_u32(header + 8);
            const u32 sn = read_u32(header + 12);
            const u32 una = read_u32(header + 16);
            const u32 len = read_u32(header + 20);
            offset += kOverhead;

            ++st.segments_received;
            rmt_wnd = wnd;
            parse_una(una);
            shrink_buf();

            if (cmd == static_cast<u8>(KcpCommand::Ack)) {
                if (time_diff(now, ts) >= 0) update_ack(time_diff(now, ts));
                if (!parse_ack(sn) && time_diff(sn, snd_nxt) < 0) ++st.duplicate_acks;
                shrink_buf();
                if (!acked) {
                    acked = true;
                    max_ack = sn;
                } else if (time_diff(sn, max_ack) > 0) {
                    max_ack = sn;  // fast retransmission reacts to the newest acknowledgement
                }
            } else if (cmd == static_cast<u8>(KcpCommand::Push)) {
                // Inside the receive window (duplicates included, so the peer can move on).
                if (time_diff(sn, rcv_nxt + rcv_wnd) < 0) {
                    ack_push(sn, ts);
                    if (time_diff(sn, rcv_nxt) >= 0) {
                        Segment segment;
                        segment.sn = sn;
                        segment.ts = ts;
                        segment.frg = frg;
                        segment.wnd = wnd;
                        segment.una = una;
                        segment.data.assign(packet.data() + offset, packet.data() + offset + len);
                        parse_data(std::move(segment));
                    }
                }
            } else if (cmd == static_cast<u8>(KcpCommand::Wask)) {
                probe |= kAskTell;  // the peer wants to know our window: answer in the next flush
            }
            offset += len;
        }

        if (acked) parse_fastack(max_ack);

        // Congestion window growth: slow start doubles per round trip, then one segment per rtt.
        if (time_diff(snd_una, previous_una) > 0 && !cfg.no_congestion_control && cwnd < rmt_wnd) {
            if (cwnd < ssthresh) {
                ++cwnd;
                incr += mss;
            } else {
                if (incr < mss) incr = mss;
                incr += (mss * mss) / incr + (mss / 16);
                if ((cwnd + 1) * mss <= incr) cwnd = (incr + mss - 1) / mss;
            }
            if (cwnd > rmt_wnd) cwnd = rmt_wnd;
            if (cwnd > snd_wnd) cwnd = snd_wnd;
        }

        ++st.input_packets;
        return true;
    }

    // -- timers ---------------------------------------------------------------

    /// Shifts every pending retransmission deadline by \p delta so a clock jump (suspend/resume)
    /// cannot turn into a burst of spurious retransmissions.
    void rebase(u64 delta) {
        if (delta == 0) return;
        for (Segment& segment : snd_buf) {
            segment.resendts = static_cast<u32>(static_cast<u64>(segment.resendts) + delta);
        }
        ts_probe = static_cast<u32>(static_cast<u64>(ts_probe) + delta);
    }

    void tick(u64 now_ms_value) {
        const u32 now = static_cast<u32>(now_ms_value);
        if (!started) {
            started = true;
            if (current != 0) rebase(static_cast<u32>(now - current));
            current = now;
            ts_flush = now;
        } else {
            const i32 slap = time_diff(now, current);
            if (slap < 0) {
                // The clock contract says time never runs backwards; resync only when it is absurd.
                if (slap < -10000) {
                    current = now;
                    ts_flush = now;
                }
                return;
            }
            if (slap >= 10000) {
                rebase(static_cast<u32>(now - current));
                current = now;
                ts_flush = now;
            }
        }
        if (time_diff(now, ts_flush) < 0) {
            current = now;
            return;  // called more often than the interval: nothing to do, nothing to send
        }
        current = now;
        flush();
        ts_flush = now + interval;
    }

    [[nodiscard]] u64 check(u64 now_ms_value) const {
        if (!started) return now_ms_value;
        const u32 now = static_cast<u32>(now_ms_value);
        u32 flush_at = ts_flush;
        if (time_diff(now, flush_at) >= 10000 || time_diff(now, flush_at) < -10000) flush_at = now;
        if (time_diff(now, flush_at) >= 0) return now_ms_value;

        i32 earliest = time_diff(flush_at, now);
        for (const Segment& segment : snd_buf) {
            const i32 due = time_diff(segment.resendts, now);
            if (due <= 0) return now_ms_value;
            if (due < earliest) earliest = due;
        }
        if (earliest > static_cast<i32>(interval)) earliest = static_cast<i32>(interval);
        if (earliest < 1) earliest = 1;
        return now_ms_value + static_cast<u32>(earliest);
    }

    void reset() {
        snd_queue.clear();
        snd_buf.clear();
        rcv_queue.clear();
        rcv_buf.clear();
        acklist.clear();
        buffer.clear();
        nsnd_que = 0;
        nsnd_buf = 0;
        nrcv_que = 0;
        nrcv_buf = 0;
        rcv_que_bytes = 0;
        snd_una = 0;
        snd_nxt = 0;
        rcv_nxt = 0;
        ssthresh = kThreshInit;
        incr = mss;
        cwnd = 1;
        probe = 0;
        probe_wait = 0;
        ts_probe = 0;
        ts_flush = current;
        rx_srtt = 0;
        rx_rttval = 0;
        rx_rto = static_cast<i32>(bound_u32(static_cast<u32>(rx_minrto), kRtoDef, kRtoMax));
        rmt_wnd = rcv_wnd;
        dead = false;
    }

    void refresh_stats() const {
        st.rtt_ms = rx_srtt > 0 ? static_cast<u32>(rx_srtt) : 0;
        st.rto_ms = static_cast<u32>(rx_rto);
        st.cwnd = cfg.no_congestion_control ? snd_wnd : cwnd;
        st.send_window = snd_wnd;
        st.receive_window = rcv_wnd;
        st.remote_window = rmt_wnd;
        st.wait_send_segments = nsnd_que + nsnd_buf;
        st.queued_receive_bytes = queued_receive_bytes();
        st.send_queue_segments = nsnd_que;
        st.receive_queue_segments = nrcv_que;
        st.xmit = snd_buf.empty() ? 0u : snd_buf.front().xmit;
    }
};

Kcp::Kcp(u32 conv, const KcpConfig& config) : impl_(make_scope<Impl>(config)) { impl_->cfg.conv = conv; }

Kcp::Kcp(const KcpConfig& config, u32 conv) : Kcp(conv, config) {}

Kcp::~Kcp() = default;

Kcp::Kcp(Kcp&&) noexcept = default;

Kcp& Kcp::operator=(Kcp&&) noexcept = default;

void Kcp::set_output(OutputFn output) {
    if (impl_) impl_->output = std::move(output);
}

u32 Kcp::conv() const { return impl_ ? impl_->cfg.conv : 0u; }

bool Kcp::send(ConstSpan<const u8> data) { return impl_ != nullptr && impl_->send(data); }

bool Kcp::input(ConstSpan<const u8> packet) { return impl_ != nullptr && impl_->input(packet); }

void Kcp::update(u64 now_ms_value) {
    if (impl_) impl_->tick(now_ms_value);
}

u64 Kcp::check(u64 now_ms_value) const { return impl_ ? impl_->check(now_ms_value) : now_ms_value; }

usize Kcp::receive(Span<u8> destination) { return impl_ ? impl_->receive(destination) : 0; }

usize Kcp::peek_size() const { return impl_ ? impl_->peek_size() : 0; }

usize Kcp::queued_receive_bytes() const { return impl_ ? impl_->rcv_que_bytes : 0; }

usize Kcp::wait_send_segments() const {
    return impl_ ? static_cast<usize>(impl_->nsnd_que) + static_cast<usize>(impl_->nsnd_buf) : 0;
}

bool Kcp::dead_link() const { return impl_ != nullptr && impl_->dead; }

void Kcp::set_mtu(u32 mtu) {
    if (impl_) impl_->set_mtu(mtu);
}

void Kcp::set_window_size(u16 send_window, u16 receive_window) {
    if (impl_) impl_->set_window_size(send_window, receive_window);
}

void Kcp::set_nodelay(bool no_delay, u32 interval_ms, u32 fast_resend_threshold, bool no_congestion_control) {
    if (impl_) impl_->set_nodelay(no_delay, interval_ms, fast_resend_threshold, no_congestion_control);
}

const KcpConfig& Kcp::config() const {
    static const KcpConfig kFallback{};
    return impl_ ? impl_->cfg : kFallback;
}

const KcpStats& Kcp::stats() const {
    static const KcpStats kFallback{};
    if (!impl_) return kFallback;
    impl_->refresh_stats();
    return impl_->st;
}

void Kcp::flush() {
    if (impl_) impl_->flush();
}

void Kcp::reset() {
    if (impl_) impl_->reset();
}

usize Kcp::max_message_size() const { return impl_ ? impl_->max_message_size() : 0; }

} // namespace t2d::net
