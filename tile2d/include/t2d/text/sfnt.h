// Tile2D - the binary reading primitives every font table needs.
//
// Fonts are big endian and arrive from disk, so every access is bounds checked: a truncated or
// hostile font must be rejected, never read past its end.
#pragma once

#include <t2d/core/types.h>

namespace t2d {

class BigEndianReader {
public:
    explicit BigEndianReader(ConstSpan<const u8> data) : data_(data) {}

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] usize position() const { return cursor_; }
    [[nodiscard]] usize remaining() const { return ok_ && cursor_ <= data_.size() ? data_.size() - cursor_ : 0; }
    [[nodiscard]] bool at_end() const { return cursor_ >= data_.size(); }

    u8 read_u8() {
        if (!take(1)) return 0;
        return data_[cursor_ - 1];
    }
    u16 read_u16() {
        if (!take(2)) return 0;
        return static_cast<u16>((static_cast<u16>(data_[cursor_ - 2]) << 8) | data_[cursor_ - 1]);
    }
    i16 read_i16() { return static_cast<i16>(read_u16()); }
    u32 read_u32() {
        if (!take(4)) return 0;
        return (static_cast<u32>(data_[cursor_ - 4]) << 24) | (static_cast<u32>(data_[cursor_ - 3]) << 16) |
               (static_cast<u32>(data_[cursor_ - 2]) << 8) | static_cast<u32>(data_[cursor_ - 1]);
    }
    i32 read_i32() { return static_cast<i32>(read_u32()); }

    /// Reads \p count bytes without copying. The span is empty when the read fails.
    [[nodiscard]] ConstSpan<const u8> bytes(usize count) {
        if (!take(count)) return {};
        return data_.subspan(cursor_ - count, count);
    }
    void skip(usize count) { (void)take(count); }

    /// A reader over a sub-range, without moving this one. Used for tables and offsets.
    [[nodiscard]] BigEndianReader window(usize offset, usize count) const {
        if (offset > data_.size() || count > data_.size() - offset) return BigEndianReader({});
        return BigEndianReader(data_.subspan(offset, count));
    }
    [[nodiscard]] ConstSpan<const u8> data() const { return data_; }

private:
    [[nodiscard]] bool take(usize count) {
        if (!ok_ || cursor_ + count > data_.size()) {
            ok_ = false;
            return false;
        }
        cursor_ += count;
        return true;
    }

    ConstSpan<const u8> data_{};
    usize cursor_ = 0;
    bool ok_ = true;
};

} // namespace t2d
