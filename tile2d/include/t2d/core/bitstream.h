// Tile2D - little endian byte streams used by the snapshot and protocol serialisation.
//
// The reader never trusts its input: every access is bounds checked and a failed read leaves the
// stream in a valid (if empty) state, which matters a lot for data that arrives from the network.
#pragma once

#include <t2d/core/log.h>
#include <t2d/core/types.h>

#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace t2d {

/// Varint encoding (LEB128) for non negative values; zigzag for signed ones.
[[nodiscard]] constexpr u32 zigzag_encode(i32 value) {
    return (static_cast<u32>(value) << 1) ^ static_cast<u32>(value >> 31);
}
[[nodiscard]] constexpr i32 zigzag_decode(u32 value) {
    return static_cast<i32>(value >> 1) ^ -static_cast<i32>(value & 1u);
}

class ByteWriter {
public:
    explicit ByteWriter(std::vector<u8>& buffer) : buffer_(&buffer) {}
    ByteWriter(u8* data, usize capacity) : external_(data), capacity_(capacity) {}

    void write_u8(u8 value) { push(&value, 1); }
    void write_bool(bool value) { write_u8(value ? 1u : 0u); }
    void write_u16(u16 value) { write_le(value, 2); }
    void write_u32(u32 value) { write_le(value, 4); }
    void write_u64(u64 value) { write_le(value, 8); }
    void write_i32(i32 value) { write_u32(zigzag_encode(value)); }
    void write_varint(u32 value) {
        while (value >= 0x80u) {
            write_u8(static_cast<u8>(value) | 0x80u);
            value >>= 7;
        }
        write_u8(static_cast<u8>(value));
    }
    void write_f32(f32 value) { write_u32(std::bit_cast<u32>(value)); }
    void write_f64(f64 value) { write_u64(std::bit_cast<u64>(value)); }
    void write_bytes(ConstSpan<const u8> data) {
        if (data.empty()) return;
        push(data.data(), data.size());
    }
    void write_string(std::string_view text) {
        write_varint(static_cast<u32>(text.size()));
        push(reinterpret_cast<const u8*>(text.data()), text.size());
    }
    /// Fixed point helpers: 1/16 pixel precision is plenty for a 2D game and keeps snapshots small.
    void write_fixed_16(f32 value) { write_i32(static_cast<i32>(value * 16.0f + (value >= 0.0f ? 0.5f : -0.5f))); }

    [[nodiscard]] usize size() const { return external_ != nullptr ? cursor_ : buffer_->size(); }
    [[nodiscard]] bool valid() const { return !overflow_; }

private:
    void push(const void* data, usize count) {
        if (overflow_ || count == 0) return;
        if (external_ != nullptr) {
            if (cursor_ + count > capacity_) {
                overflow_ = true;
                return;
            }
            std::memcpy(external_ + cursor_, data, count);
            cursor_ += count;
            return;
        }
        // resize + memcpy rather than insert(): GCC 16 at -O3 folds the inlined range insert into a
        // bogus -Wstringop-overflow warning, and this form is neither slower nor harder to read.
        const usize offset = buffer_->size();
        buffer_->resize(offset + count);
        std::memcpy(buffer_->data() + offset, data, count);
    }

    template <class T>
    void write_le(T value, usize count) {
        u8 bytes[8];
        for (usize i = 0; i < count; ++i) bytes[i] = static_cast<u8>((static_cast<u64>(value) >> (i * 8)) & 0xFFu);
        push(bytes, count);
    }

    std::vector<u8>* buffer_ = nullptr;
    u8* external_ = nullptr;
    usize capacity_ = 0;
    usize cursor_ = 0;
    bool overflow_ = false;
};

class ByteReader {
public:
    explicit ByteReader(ConstSpan<const u8> data) : data_(data) {}

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] bool empty() const { return cursor_ >= data_.size(); }
    [[nodiscard]] usize remaining() const { return ok_ ? data_.size() - cursor_ : 0; }
    [[nodiscard]] usize position() const { return cursor_; }

    u8 read_u8() {
        u8 value = 0;
        if (!take(1)) return 0;
        value = data_[cursor_ - 1];
        return value;
    }
    bool read_bool() { return read_u8() != 0; }
    u16 read_u16() { return static_cast<u16>(read_le(2)); }
    u32 read_u32() { return static_cast<u32>(read_le(4)); }
    u64 read_u64() { return read_le(8); }
    i32 read_i32() { return zigzag_decode(read_u32()); }
    u32 read_varint() {
        u32 result = 0;
        u32 shift = 0;
        for (int i = 0; i < 5; ++i) {
            const u8 byte = read_u8();
            if (!ok_) return 0;
            result |= static_cast<u32>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0u) return result;
            shift += 7;
        }
        ok_ = false; // malformed varint
        return 0;
    }
    f32 read_f32() { return std::bit_cast<f32>(read_u32()); }
    f64 read_f64() { return std::bit_cast<f64>(read_u64()); }
    f32 read_fixed_16() { return static_cast<f32>(read_i32()) * (1.0f / 16.0f); }

    bool read_bytes(Span<u8> destination) {
        if (!take(destination.size())) return false;
        std::memcpy(destination.data(), data_.data() + cursor_ - destination.size(), destination.size());
        return true;
    }
    [[nodiscard]] ConstSpan<const u8> read_span(usize count) {
        if (!take(count)) return {};
        return data_.subspan(cursor_ - count, count);
    }
    [[nodiscard]] std::string read_string() {
        const u32 size = read_varint();
        if (!ok_ || size > remaining()) {
            ok_ = false;
            return {};
        }
        const auto span = read_span(size);
        return std::string(reinterpret_cast<const char*>(span.data()), span.size());
    }

private:
    [[nodiscard]] bool take(usize count) {
        if (!ok_ || cursor_ + count > data_.size()) {
            ok_ = false;
            return false;
        }
        cursor_ += count;
        return true;
    }
    [[nodiscard]] u64 read_le(usize count) {
        if (!take(count)) return 0;
        u64 value = 0;
        for (usize i = 0; i < count; ++i) value |= static_cast<u64>(data_[cursor_ - count + i]) << (i * 8);
        return value;
    }

    ConstSpan<const u8> data_;
    usize cursor_ = 0;
    bool ok_ = true;
};

} // namespace t2d
