// Tile2D - CFF (Type 2 charstrings), the outlines the CJK families ship in.
//
// The table is read once: the four INDEXes at the front, the Top DICT, and then either the FDArray
// and FDSelect of a CID keyed font or the single Private DICT of a plain one. Glyphs are interpreted
// on demand by a small stack machine, because a Type 2 charstring is a program - it moves, draws and
// calls subroutines, and the outline is only known once it has run.
//
// Nothing here trusts a length. Every INDEX, offset and operand is checked against the table before
// it is used, the interpreter is bounded in call depth and instruction count, and no coordinate is
// emitted unless it is finite, so a truncated or hostile table is rejected instead of read past.

#include <t2d/text/cff.h>

#include <cmath>
#include <string>
#include <vector>

namespace t2d {
namespace {

// ------------------------------------------------------------------ limits --

constexpr usize kMaxDictOperands = 48;   ///< CFF operand limit per DICT operator
constexpr usize kMaxStack = 96;          ///< Type 2 operand stack (the spec allows 48)
constexpr usize kMaxSubrDepth = 64;      ///< callsubr nesting (the spec allows 10)
constexpr u32 kMaxInstructions = 1u << 20;
constexpr i32 kMaxRealExponent = 300;

// Top DICT operators. Two byte operators are encoded as 0x0C00 | second_byte.
constexpr u32 kOpCharStrings = 17;
constexpr u32 kOpPrivate = 18;
constexpr u32 kOpRos = 0x0C1Eu;      // 12 30
constexpr u32 kOpFdArray = 0x0C24u;  // 12 36
constexpr u32 kOpFdSelect = 0x0C25u; // 12 37

// Private DICT operators.
constexpr u32 kOpSubrs = 19;
constexpr u32 kOpDefaultWidthX = 20;
constexpr u32 kOpNominalWidthX = 21;

// ---------------------------------------------------------------- reading --

[[nodiscard]] bool in_range(ConstSpan<const u8> data, usize position, usize count) {
    return position <= data.size() && count <= data.size() - position;
}

[[nodiscard]] bool read_u16(ConstSpan<const u8> data, usize position, u32& out) {
    if (!in_range(data, position, 2)) return false;
    out = (u32(data[position]) << 8) | u32(data[position + 1]);
    return true;
}

[[nodiscard]] bool read_u32(ConstSpan<const u8> data, usize position, u32& out) {
    if (!in_range(data, position, 4)) return false;
    out = (u32(data[position]) << 24) | (u32(data[position + 1]) << 16) | (u32(data[position + 2]) << 8) |
          u32(data[position + 3]);
    return true;
}

[[nodiscard]] bool read_offset_field(ConstSpan<const u8> data, usize position, u8 width, usize& out) {
    if (width < 1 || width > 4 || !in_range(data, position, width)) return false;
    usize value = 0;
    for (u8 i = 0; i < width; ++i) value = (value << 8) | usize(data[position + i]);
    out = value;
    return true;
}

/// A whole number that fits in the table: DICT operands are read as doubles, offsets must be exact.
[[nodiscard]] bool whole_value(f64 value, usize& out) {
    if (!std::isfinite(value) || value < 0.0) return false;
    const f64 rounded = std::floor(value);
    if (rounded != value || rounded > f64(0xFFFF'FFFFu)) return false;
    out = usize(rounded);
    return true;
}

[[nodiscard]] bool whole_offset(f64 value, usize limit, usize& out) {
    if (!whole_value(value, out)) return false;
    return out < limit;
}

// ------------------------------------------------------------------ INDEX --

/// One CFF INDEX: count items held between the offsets field (count + 1 big endian offsets of
/// off_size bytes, 1 based from the start of the data) and the end of the index. An empty INDEX
/// has no offSize byte at all.
struct Index {
    usize count = 0;
    usize offsets = 0;
    usize data = 0;
    usize data_size = 0;
    u8 off_size = 0;
};

[[nodiscard]] bool parse_index(ConstSpan<const u8> table, usize position, Index& out, usize& next) {
    out = Index{};
    next = position;
    u32 count = 0;
    if (!read_u16(table, position, count)) return false;
    if (count == 0) {
        next = position + 2;
        return true;
    }
    if (!in_range(table, position, 3)) return false;
    const u8 off_size = table[position + 2];
    if (off_size < 1 || off_size > 4) return false;
    const usize offsets = position + 3;
    const usize offsets_end = offsets + (usize(count) + 1) * usize(off_size);
    if (offsets_end > table.size() || offsets_end < offsets) return false;

    usize previous = 0;
    for (usize i = 0; i <= usize(count); ++i) {
        usize value = 0;
        if (!read_offset_field(table, offsets + i * usize(off_size), off_size, value)) return false;
        if (i == 0) {
            if (value < 1) return false;
        } else if (value < previous) {
            return false; // the offsets of a valid INDEX never go backwards
        }
        previous = value;
    }
    if (previous < 1 || previous - 1 > table.size() - offsets_end) return false;

    out.count = usize(count);
    out.offsets = offsets;
    out.off_size = off_size;
    out.data = offsets_end;
    out.data_size = previous - 1;
    next = offsets_end + out.data_size;
    return true;
}

[[nodiscard]] usize index_offset(ConstSpan<const u8> table, const Index& index, usize position) {
    usize value = 0;
    if (!read_offset_field(table, index.offsets + position * usize(index.off_size), index.off_size, value)) return 0;
    return value;
}

[[nodiscard]] ConstSpan<const u8> index_item(ConstSpan<const u8> table, const Index& index, usize position) {
    if (position >= index.count) return {};
    const usize begin = index_offset(table, index, position);
    const usize end = index_offset(table, index, position + 1);
    if (begin < 1 || end < begin || end - 1 > index.data_size) return {};
    return table.subspan(index.data + begin - 1, end - begin);
}

// ----------------------------------------------------------------- numbers --

/// The nibble encoded real of a DICT operand (byte 30). Only values this parser does not use are
/// encoded that way (FontMatrix, BlueScale), but they still have to be skipped over correctly.
[[nodiscard]] bool read_real(ConstSpan<const u8> data, usize& position, f64& out) {
    ++position; // the 30 introducer
    f64 mantissa = 0.0;
    i32 decimals = 0;
    i32 exponent = 0;
    bool negative = false;
    bool negative_exponent = false;
    bool after_point = false;
    bool in_exponent = false;
    for (;;) {
        if (position >= data.size()) return false;
        const u8 byte = data[position++];
        for (u8 half = 0; half < 2; ++half) {
            const u8 nibble = (half == 0) ? u8(byte >> 4) : u8(byte & 0x0Fu);
            if (nibble <= 9) {
                if (in_exponent) {
                    exponent = exponent * 10 + i32(nibble);
                    if (exponent > kMaxRealExponent) exponent = kMaxRealExponent;
                } else {
                    mantissa = mantissa * 10.0 + f64(nibble);
                    if (after_point) ++decimals;
                }
            } else if (nibble == 0x0A) {
                after_point = true;
            } else if (nibble == 0x0B) {
                in_exponent = true;
            } else if (nibble == 0x0C) {
                in_exponent = true;
                negative_exponent = true;
            } else if (nibble == 0x0E) {
                if (in_exponent) {
                    negative_exponent = true;
                } else {
                    negative = true;
                }
            } else if (nibble == 0x0F) {
                if (decimals != 0) mantissa /= std::pow(10.0, f64(decimals));
                if (exponent != 0) mantissa *= std::pow(10.0, f64(negative_exponent ? -exponent : exponent));
                out = negative ? -mantissa : mantissa;
                return true;
            } else {
                return false; // 0x0D is reserved
            }
        }
    }
}

[[nodiscard]] bool read_dict_number(ConstSpan<const u8> data, usize& position, f64& out) {
    const u8 b0 = data[position];
    if (b0 == 28) {
        if (!in_range(data, position, 3)) return false;
        i32 value = (i32(data[position + 1]) << 8) | i32(data[position + 2]);
        if (value >= 0x8000) value -= 0x10000;
        out = f64(value);
        position += 3;
        return true;
    }
    if (b0 == 29) {
        u32 raw = 0;
        if (!read_u32(data, position + 1, raw)) return false;
        out = f64(static_cast<i32>(raw));
        position += 5;
        return true;
    }
    if (b0 == 30) return read_real(data, position, out);
    if (b0 >= 32 && b0 <= 246) {
        out = f64(i32(b0) - 139);
        position += 1;
        return true;
    }
    if (b0 >= 247 && b0 <= 250) {
        if (!in_range(data, position, 2)) return false;
        out = f64((i32(b0) - 247) * 256 + i32(data[position + 1]) + 108);
        position += 2;
        return true;
    }
    if (b0 >= 251 && b0 <= 254) {
        if (!in_range(data, position, 2)) return false;
        out = f64(-(i32(b0) - 251) * 256 - i32(data[position + 1]) - 108);
        position += 2;
        return true;
    }
    return false;
}

/// Walks a DICT and hands every (operands, operator) pair to the handler. The handler returns false
/// to reject the DICT; a trailing operand list without an operator is malformed.
template <class Handler>
[[nodiscard]] bool scan_dict(ConstSpan<const u8> dict, Handler&& handler) {
    std::vector<f64> operands;
    operands.reserve(8);
    usize position = 0;
    while (position < dict.size()) {
        const u8 b0 = dict[position];
        if (b0 <= 21) {
            u32 op = b0;
            usize length = 1;
            if (b0 == 12) {
                if (!in_range(dict, position, 2)) return false;
                op = 0x0C00u | u32(dict[position + 1]);
                length = 2;
            }
            if (!handler(Span<const f64>(operands), op)) return false;
            operands.clear();
            position += length;
            continue;
        }
        if (operands.size() >= kMaxDictOperands) return false;
        f64 value = 0.0;
        if (!read_dict_number(dict, position, value)) return false;
        operands.push_back(value);
    }
    return operands.empty();
}

// -------------------------------------------------------------- structures --

/// Everything a charstring needs from its Private DICT.
struct PrivateInfo {
    f64 default_width = 0.0;
    f64 nominal_width = 0.0;
    Index subrs{};
};

/// The Top DICT entries this interpreter uses. The rest (charset, FontMatrix, names) is ignored.
struct TopInfo {
    bool has_charstrings = false;
    f64 charstrings = 0.0;
    bool has_private = false;
    f64 private_size = 0.0;
    f64 private_offset = 0.0;
    bool has_fdarray = false;
    f64 fdarray = 0.0;
    bool has_fdselect = false;
    f64 fdselect = 0.0;
    bool cid = false;
};

[[nodiscard]] bool read_top_entry(TopInfo& out, Span<const f64> operands, u32 op) {
    switch (op) {
    case kOpCharStrings:
        if (operands.size() != 1) return false;
        out.charstrings = operands[0];
        out.has_charstrings = true;
        return true;
    case kOpPrivate:
        if (operands.size() != 2) return false;
        out.private_size = operands[0];
        out.private_offset = operands[1];
        out.has_private = true;
        return true;
    case kOpFdArray:
        if (operands.size() != 1) return false;
        out.fdarray = operands[0];
        out.has_fdarray = true;
        return true;
    case kOpFdSelect:
        if (operands.size() != 1) return false;
        out.fdselect = operands[0];
        out.has_fdselect = true;
        return true;
    case kOpRos:
        if (operands.size() != 3) return false;
        out.cid = true;
        return true;
    default:
        return true;
    }
}

[[nodiscard]] bool read_private_dict(ConstSpan<const u8> table, f64 size_value, f64 offset_value, PrivateInfo& out) {
    usize size = 0;
    usize offset = 0;
    if (!whole_value(size_value, size) || !whole_value(offset_value, offset)) return false;
    if (offset > table.size() || size > table.size() - offset) return false;
    const ConstSpan<const u8> dict = table.subspan(offset, size);
    return scan_dict(dict, [&](Span<const f64> operands, u32 op) {
        if (op == kOpSubrs) {
            usize relative = 0;
            if (operands.size() != 1 || !whole_value(operands[0], relative)) return false;
            if (relative > table.size() - offset) return false;
            usize next = 0;
            return parse_index(table, offset + relative, out.subrs, next);
        }
        if (op == kOpDefaultWidthX) {
            if (operands.size() != 1) return false;
            out.default_width = operands[0];
            return true;
        }
        if (op == kOpNominalWidthX) {
            if (operands.size() != 1) return false;
            out.nominal_width = operands[0];
            return true;
        }
        return true;
    });
}

/// FDSelect format 0 (one byte per glyph) and format 3 (ranges). Anything else is not a CFF1 font.
[[nodiscard]] bool read_fd_select(ConstSpan<const u8> table, usize position, u32 glyph_count, usize fd_count,
                                  std::vector<u8>& out) {
    if (position >= table.size()) return false;
    const u8 format = table[position];
    out.assign(glyph_count, 0);
    if (format == 0) {
        if (!in_range(table, position + 1, glyph_count)) return false;
        for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
            const u8 fd = table[position + 1 + glyph];
            if (usize(fd) >= fd_count) return false;
            out[glyph] = fd;
        }
        return true;
    }
    if (format != 3) return false;

    u32 range_count = 0;
    if (!read_u16(table, position + 1, range_count)) return false;
    usize cursor = position + 3;
    std::vector<u32> starts(range_count, 0);
    std::vector<u8> indices(range_count, 0);
    for (u32 i = 0; i < range_count; ++i) {
        u32 first = 0;
        if (!read_u16(table, cursor, first) || !in_range(table, cursor + 2, 1)) return false;
        const u8 fd = table[cursor + 2];
        if (usize(fd) >= fd_count) return false;
        starts[i] = first;
        indices[i] = fd;
        cursor += 3;
    }
    u32 sentinel = 0;
    if (!read_u16(table, cursor, sentinel)) return false;
    if (!starts.empty() && starts[0] != 0) return false;
    if (sentinel > glyph_count) return false;
    for (u32 i = 0; i < range_count; ++i) {
        const u32 end = (i + 1 < range_count) ? starts[i + 1] : sentinel;
        if (end < starts[i] || end > glyph_count) return false;
        for (u32 glyph = starts[i]; glyph < end; ++glyph) out[glyph] = indices[i];
    }
    return true;
}

[[nodiscard]] i32 subr_bias(usize count) {
    if (count < 1240) return 107;
    if (count < 33900) return 1131;
    return 32768;
}

// ------------------------------------------------------------ interpreter --

/// Type 2 numbers: 28 is a 16 bit integer and 255 a 16.16 fixed point value.
[[nodiscard]] bool read_charstring_number(ConstSpan<const u8> code, usize& position, f64& out) {
    const u8 b0 = code[position];
    if (b0 == 28) {
        if (!in_range(code, position, 3)) return false;
        i32 value = (i32(code[position + 1]) << 8) | i32(code[position + 2]);
        if (value >= 0x8000) value -= 0x10000;
        out = f64(value);
        position += 3;
        return true;
    }
    if (b0 == 255) {
        u32 raw = 0;
        if (!read_u32(code, position + 1, raw)) return false;
        out = f64(static_cast<i32>(raw)) / 65536.0;
        position += 5;
        return true;
    }
    if (b0 >= 32 && b0 <= 246) {
        out = f64(i32(b0) - 139);
        position += 1;
        return true;
    }
    if (b0 >= 247 && b0 <= 250) {
        if (!in_range(code, position, 2)) return false;
        out = f64((i32(b0) - 247) * 256 + i32(code[position + 1]) + 108);
        position += 2;
        return true;
    }
    if (b0 >= 251 && b0 <= 254) {
        if (!in_range(code, position, 2)) return false;
        out = f64(-(i32(b0) - 251) * 256 - i32(code[position + 1]) - 108);
        position += 2;
        return true;
    }
    return false;
}

} // namespace

/// The parsed "CFF " table. It owns a copy of the bytes, so a CffFont stays valid after the caller's
/// buffer is gone; every span handed out below points into that copy. CffFont::Impl is exactly this
/// context: CffFont::Impl is private, so the interpreter below - which is a free class - cannot name
/// it, while the members of CffFont can.
struct CffContext {
    std::vector<u8> table;
    Index charstrings{};
    Index gsubrs{};
    std::vector<PrivateInfo> privates;
    std::vector<u8> fd_select;
    u32 glyph_count = 0;

    [[nodiscard]] ConstSpan<const u8> charstring(u32 glyph) const {
        if (glyph >= glyph_count) return {};
        return index_item(table, charstrings, usize(glyph));
    }

    /// The Private DICT that applies to a glyph: the FDSelect entry of a CID keyed font, or the
    /// single Private DICT every glyph of a plain font shares.
    [[nodiscard]] const PrivateInfo& private_for(u32 glyph) const {
        if (usize(glyph) < fd_select.size()) {
            const u8 fd = fd_select[usize(glyph)];
            if (usize(fd) < privates.size()) return privates[usize(fd)];
        }
        return privates.front();
    }

    [[nodiscard]] bool find_subr(const PrivateInfo& priv, bool global, f64 operand, ConstSpan<const u8>& out) const {
        const Index& index = global ? gsubrs : priv.subrs;
        if (index.count == 0) return false;
        const f64 biased = operand + f64(subr_bias(index.count));
        if (!(biased >= 0.0) || biased >= f64(index.count)) return false; // also rejects NaN
        out = index_item(table, index, usize(biased));
        return true;
    }
};

/// The private nested type the header declares, holding nothing but the context above.
struct CffFont::Impl : CffContext {};

namespace {

/// The Type 2 stack machine. One instance interprets one glyph: it walks the charstring (and any
/// subroutine it calls) and appends the outline to the path it was given.
class CharstringInterp {
public:
    CharstringInterp(const CffContext& impl, const PrivateInfo& priv, GlyphPath* path, bool width_only)
        : impl_(impl), priv_(priv), path_(path), width_only_(width_only) {}

    [[nodiscard]] bool run(ConstSpan<const u8> code);
    [[nodiscard]] f64 width() const { return width_; }

private:
    struct Frame {
        ConstSpan<const u8> code{};
        usize ip = 0;
    };

    [[nodiscard]] bool execute(u8 byte);
    [[nodiscard]] bool execute_escaped(u8 second);
    [[nodiscard]] bool finish();

    /// The first stack clearing operator may carry the glyph width in front of its arguments: odd
    /// counts for the stem and mask operators and rmoveto, even counts for hmoveto and vmoveto.
    [[nodiscard]] bool take_width(bool even_args);
    [[nodiscard]] bool count_stems();

    [[nodiscard]] bool finite() const { return std::isfinite(x_) && std::isfinite(y_); }
    [[nodiscard]] bool move_by(f64 dx, f64 dy);
    [[nodiscard]] bool line_by(f64 dx, f64 dy);
    [[nodiscard]] bool curve_by(f64 dx1, f64 dy1, f64 dx2, f64 dy2, f64 dx3, f64 dy3);

    const CffContext& impl_;
    const PrivateInfo& priv_;
    GlyphPath* path_ = nullptr;
    bool width_only_ = false;

    std::vector<Frame> frames_;
    std::vector<f64> stack_;
    f64 x_ = 0.0;
    f64 y_ = 0.0;
    bool open_ = false;
    bool finished_ = false;
    bool width_done_ = false;
    f64 width_ = 0.0;
    u32 stems_ = 0;
    u32 budget_ = kMaxInstructions;
};

[[nodiscard]] bool CharstringInterp::run(ConstSpan<const u8> code) {
    frames_.clear();
    frames_.push_back(Frame{code, 0});
    while (!frames_.empty() && !finished_) {
        if (budget_ == 0) return false;
        --budget_;
        const usize top = frames_.size() - 1;
        const ConstSpan<const u8> current = frames_[top].code;
        usize ip = frames_[top].ip;
        if (ip >= current.size()) {
            if (frames_.size() == 1) break; // the charstring simply ended
            frames_.pop_back();
            continue;
        }
        const u8 byte = current[ip];
        if (byte == 28 || byte == 255 || byte >= 32) {
            f64 value = 0.0;
            if (!read_charstring_number(current, ip, value)) return false;
            if (stack_.size() >= kMaxStack) return false;
            stack_.push_back(value);
            frames_[top].ip = ip;
            continue;
        }
        frames_[top].ip = ip + 1;
        if (!execute(byte)) return false;
        if (width_only_ && width_done_) return true;
    }
    return finish();
}

[[nodiscard]] bool CharstringInterp::finish() {
    if (open_ && path_ != nullptr) {
        path_->close();
        open_ = false;
    }
    return true;
}

[[nodiscard]] bool CharstringInterp::take_width(bool even_args) {
    if (width_done_) return true;
    const bool present = even_args ? (stack_.size() % 2 == 0) : (stack_.size() % 2 == 1);
    if (present && !stack_.empty()) {
        width_ = priv_.nominal_width + stack_.front();
        stack_.erase(stack_.begin());
    } else {
        width_ = priv_.default_width;
    }
    width_done_ = true;
    return true;
}

[[nodiscard]] bool CharstringInterp::count_stems() {
    if (!take_width(false)) return false;
    if (stack_.size() % 2 != 0) return false; // stem hints come in pairs
    stems_ += u32(stack_.size() / 2);
    return true;
}

[[nodiscard]] bool CharstringInterp::move_by(f64 dx, f64 dy) {
    x_ += dx;
    y_ += dy;
    if (!finite()) return false;
    if (path_ != nullptr) {
        if (open_) path_->close();
        path_->move_to(f32(x_), f32(y_));
    }
    open_ = true;
    return true;
}

[[nodiscard]] bool CharstringInterp::line_by(f64 dx, f64 dy) {
    if (path_ != nullptr && !open_) path_->move_to(f32(x_), f32(y_)); // a line starts the contour here
    open_ = true;
    x_ += dx;
    y_ += dy;
    if (!finite()) return false;
    if (path_ != nullptr) path_->line_to(f32(x_), f32(y_));
    return true;
}

[[nodiscard]] bool CharstringInterp::curve_by(f64 dx1, f64 dy1, f64 dx2, f64 dy2, f64 dx3, f64 dy3) {
    if (path_ != nullptr && !open_) path_->move_to(f32(x_), f32(y_));
    open_ = true;
    const f64 c1x = x_ + dx1;
    const f64 c1y = y_ + dy1;
    const f64 c2x = c1x + dx2;
    const f64 c2y = c1y + dy2;
    const f64 ex = c2x + dx3;
    const f64 ey = c2y + dy3;
    if (!std::isfinite(c1x) || !std::isfinite(c1y) || !std::isfinite(c2x) || !std::isfinite(c2y) ||
        !std::isfinite(ex) || !std::isfinite(ey)) {
        return false;
    }
    x_ = ex;
    y_ = ey;
    if (path_ != nullptr) path_->cubic_to(f32(c1x), f32(c1y), f32(c2x), f32(c2y), f32(ex), f32(ey));
    return true;
}

[[nodiscard]] bool CharstringInterp::execute(u8 byte) {
    switch (byte) {
    // hints
    case 1:  // hstem
    case 3:  // vstem
    case 18: // hstemhm
    case 23: // vstemhm
        if (!count_stems()) return false;
        stack_.clear();
        return true;
    case 19: // hintmask
    case 20: { // cntrmask
        if (!count_stems()) return false;
        stack_.clear();
        const usize mask = (usize(stems_) + 7) / 8;
        Frame& frame = frames_.back();
        if (mask > frame.code.size() - frame.ip) return false;
        frame.ip += mask;
        return true;
    }

    // moveto
    case 21: { // rmoveto: [width] dx dy
        if (!take_width(false)) return false;
        if (stack_.size() != 2) return false;
        const f64 dx = stack_[0];
        const f64 dy = stack_[1];
        stack_.clear();
        return move_by(dx, dy);
    }
    case 22: { // hmoveto: [width] dx
        if (!take_width(true)) return false;
        if (stack_.size() != 1) return false;
        const f64 dx = stack_[0];
        stack_.clear();
        return move_by(dx, 0.0);
    }
    case 4: { // vmoveto: [width] dy
        if (!take_width(true)) return false;
        if (stack_.size() != 1) return false;
        const f64 dy = stack_[0];
        stack_.clear();
        return move_by(0.0, dy);
    }

    // lines
    case 5: { // rlineto
        if (stack_.empty() || stack_.size() % 2 != 0) return false;
        for (usize i = 0; i < stack_.size(); i += 2) {
            if (!line_by(stack_[i], stack_[i + 1])) return false;
        }
        stack_.clear();
        return true;
    }
    case 6:  // hlineto: the direction alternates, starting horizontally
    case 7: { // vlineto: starting vertically
        if (stack_.empty()) return false;
        bool horizontal = (byte == 6);
        for (const f64 delta : stack_) {
            if (!(horizontal ? line_by(delta, 0.0) : line_by(0.0, delta))) return false;
            horizontal = !horizontal;
        }
        stack_.clear();
        return true;
    }

    // curves
    case 8: { // rrcurveto
        if (stack_.empty() || stack_.size() % 6 != 0) return false;
        for (usize i = 0; i < stack_.size(); i += 6) {
            if (!curve_by(stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], stack_[i + 4], stack_[i + 5])) {
                return false;
            }
        }
        stack_.clear();
        return true;
    }
    case 24: { // rcurveline: {dxa dya dxb dyb dxc dyc}+ dxd dyd
        const usize count = stack_.size();
        if (count < 8 || (count - 2) % 6 != 0) return false;
        usize i = 0;
        for (; i + 2 < count; i += 6) {
            if (!curve_by(stack_[i], stack_[i + 1], stack_[i + 2], stack_[i + 3], stack_[i + 4], stack_[i + 5])) {
                return false;
            }
        }
        const bool ok = line_by(stack_[count - 2], stack_[count - 1]);
        stack_.clear();
        return ok;
    }
    case 25: { // rlinecurve: {dxa dya}+ dxb dyb dxc dyc dxd dyd
        const usize count = stack_.size();
        if (count < 6 || (count - 6) % 2 != 0) return false;
        for (usize i = 0; i + 6 < count; i += 2) {
            if (!line_by(stack_[i], stack_[i + 1])) return false;
        }
        const usize base = count - 6;
        const bool ok = curve_by(stack_[base], stack_[base + 1], stack_[base + 2], stack_[base + 3], stack_[base + 4],
                                 stack_[base + 5]);
        stack_.clear();
        return ok;
    }
    case 26:  // vvcurveto: dx1? {dya dxb dyb dyc}+
    case 27: { // hhcurveto: dy1? {dxa dxb dyb dxc}+
        const usize count = stack_.size();
        if (count < 4) return false;
        const bool horizontal = (byte == 27);
        usize i = 0;
        f64 first = 0.0; // the optional first delta, present when the operand count is odd
        if (count % 2 != 0) {
            first = stack_[0];
            i = 1;
        }
        if ((count - i) % 4 != 0) return false;
        for (; i < count; i += 4) {
            const bool ok = horizontal ? curve_by(stack_[i], first, stack_[i + 1], stack_[i + 2], stack_[i + 3], 0.0)
                                       : curve_by(first, stack_[i], stack_[i + 1], stack_[i + 2], 0.0, stack_[i + 3]);
            if (!ok) return false;
            first = 0.0;
        }
        stack_.clear();
        return true;
    }
    case 30:  // vhcurveto: alternates, starting vertically
    case 31: { // hvcurveto: starting horizontally
        const usize count = stack_.size();
        if (count < 4) return false;
        bool horizontal = (byte == 31);
        usize i = 0;
        while (i < count) {
            const usize remaining = count - i;
            if (remaining < 4) return false;
            const bool extra = (remaining == 5); // the last curve may carry the other axis
            const f64 a0 = stack_[i];
            const f64 a1 = stack_[i + 1];
            const f64 a2 = stack_[i + 2];
            const f64 a3 = stack_[i + 3];
            const f64 a4 = extra ? stack_[i + 4] : 0.0;
            const bool ok = horizontal ? curve_by(a0, 0.0, a1, a2, a4, a3) : curve_by(0.0, a0, a1, a2, a3, a4);
            if (!ok) return false;
            i += extra ? 5 : 4;
            horizontal = !horizontal;
        }
        stack_.clear();
        return true;
    }

    // subroutines
    case 10:  // callsubr
    case 29: { // callgsubr
        if (stack_.empty()) return false;
        const f64 operand = stack_.back();
        stack_.pop_back();
        ConstSpan<const u8> subr{};
        if (!impl_.find_subr(priv_, byte == 29, operand, subr)) return false;
        if (frames_.size() >= kMaxSubrDepth) return false;
        frames_.push_back(Frame{subr, 0});
        return true;
    }
    case 11: // return
        if (frames_.size() > 1) {
            frames_.pop_back();
        } else {
            finished_ = true; // a top level return ends the charstring, as FreeType reads it
        }
        return true;
    case 14: { // endchar
        if (!take_width(false)) return false;
        if (stack_.size() == 4) return false; // deprecated seac accent composition: not supported
        if (!stack_.empty()) return false;
        finished_ = true;
        return true;
    }
    case 12: { // the two byte (escaped) operators
        Frame& frame = frames_.back();
        if (frame.ip >= frame.code.size()) return false;
        const u8 second = frame.code[frame.ip];
        frame.ip += 1;
        return execute_escaped(second);
    }
    default:
        return false; // reserved opcode
    }
}

[[nodiscard]] bool CharstringInterp::execute_escaped(u8 second) {
    switch (second) {
    case 34: { // hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6
        if (stack_.size() != 7) return false;
        const f64 dy2 = stack_[2];
        const bool first = curve_by(stack_[0], 0.0, stack_[1], dy2, stack_[3], 0.0);
        const bool second_curve = first && curve_by(stack_[4], 0.0, stack_[5], -dy2, stack_[6], 0.0);
        stack_.clear();
        return second_curve;
    }
    case 33: { // hflex1: dx1 dy1 dx2 dy2 dx3 dx4 dx5 dy5 dx6
        if (stack_.size() != 9) return false;
        const f64 dy6 = -(stack_[1] + stack_[3] + stack_[7]);
        const bool first = curve_by(stack_[0], stack_[1], stack_[2], stack_[3], stack_[4], 0.0);
        const bool second_curve = first && curve_by(stack_[5], 0.0, stack_[6], stack_[7], stack_[8], dy6);
        stack_.clear();
        return second_curve;
    }
    case 35: { // flex: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 dx6 dy6 fd
        if (stack_.size() != 13) return false;
        const bool first = curve_by(stack_[0], stack_[1], stack_[2], stack_[3], stack_[4], stack_[5]);
        const bool second_curve = first && curve_by(stack_[6], stack_[7], stack_[8], stack_[9], stack_[10], stack_[11]);
        stack_.clear();
        return second_curve;
    }
    case 36: { // flex1: dx1 dy1 dx2 dy2 dx3 dy3 dx4 dy4 dx5 dy5 d6
        if (stack_.size() != 11) return false;
        const f64 dx = stack_[0] + stack_[2] + stack_[4] + stack_[6] + stack_[8];
        const f64 dy = stack_[1] + stack_[3] + stack_[5] + stack_[7] + stack_[9];
        const f64 last = stack_[10];
        const bool horizontal = std::fabs(dx) > std::fabs(dy);
        const f64 dx6 = horizontal ? last : -dx;
        const f64 dy6 = horizontal ? -dy : last;
        const bool first = curve_by(stack_[0], stack_[1], stack_[2], stack_[3], stack_[4], stack_[5]);
        const bool second_curve = first && curve_by(stack_[6], stack_[7], stack_[8], stack_[9], dx6, dy6);
        stack_.clear();
        return second_curve;
    }
    default:
        // The Type 1 arithmetic, storage and random operators (12 3, 4, 5, 8, 9, 10, 11, 12, 13, 14,
        // 15, 18, 20, 21, 22, 23, 24, 26, 27, 28, 29, 30) are not implemented: they are extinct in
        // CFF fonts (fontTools' outline extractor raises NotImplementedError for all of them) and no
        // glyph of the fonts this engine targets uses one. They are reported, not guessed at.
        return false;
    }
}

[[nodiscard]] bool interpret(const CffContext& impl, u32 glyph, GlyphPath* path, bool width_only, f64& width) {
    const PrivateInfo& priv = impl.private_for(glyph);
    CharstringInterp interp(impl, priv, path, width_only);
    if (!interp.run(impl.charstring(glyph))) {
        width = priv.default_width;
        return false;
    }
    width = interp.width();
    return true;
}

} // namespace

// ---------------------------------------------------------------- the API --

std::shared_ptr<const CffFont> CffFont::parse(ConstSpan<const u8> cff_table, std::string* error) {
    const auto fail = [error](const char* reason) -> std::shared_ptr<const CffFont> {
        if (error != nullptr) *error = reason;
        return nullptr;
    };

    if (cff_table.size() < 4) return fail("the CFF table is too small to hold a header");
    if (cff_table[0] != 1) return fail("unsupported CFF major version");
    const usize header_size = usize(cff_table[2]);
    if (header_size < 4 || header_size > cff_table.size()) return fail("bad CFF header size");

    auto impl = std::make_shared<CffFont::Impl>();
    impl->table.assign(cff_table.begin(), cff_table.end());
    const ConstSpan<const u8> table(impl->table);

    // The four INDEXes that follow the header, in order.
    usize position = header_size;
    Index name_index{};
    Index top_index{};
    Index string_index{};
    if (!parse_index(table, position, name_index, position)) return fail("bad Name INDEX");
    if (!parse_index(table, position, top_index, position)) return fail("bad Top DICT INDEX");
    if (!parse_index(table, position, string_index, position)) return fail("bad String INDEX");
    if (!parse_index(table, position, impl->gsubrs, position)) return fail("bad Global Subr INDEX");
    if (top_index.count != 1) return fail("the Top DICT INDEX must hold exactly one DICT");

    TopInfo top{};
    if (!scan_dict(index_item(table, top_index, 0), [&top](Span<const f64> operands, u32 op) {
            return read_top_entry(top, operands, op);
        })) {
        return fail("malformed Top DICT");
    }
    if (!top.has_charstrings) return fail("the Top DICT has no CharStrings entry");

    usize charstrings_position = 0;
    if (!whole_offset(top.charstrings, table.size(), charstrings_position)) return fail("bad CharStrings offset");
    usize after = 0;
    if (!parse_index(table, charstrings_position, impl->charstrings, after)) return fail("bad CharStrings INDEX");
    impl->glyph_count = u32(impl->charstrings.count);

    // A CID keyed font keeps its charstrings in the Private DICT of the Font DICT that the FDSelect
    // points at; without FDArray and FDSelect the Top DICT Private DICT covers every glyph.
    Index fdarray_index{};
    usize fdselect_position = 0;
    bool have_fdarray = false;
    if (top.has_fdarray) {
        usize fdarray_position = 0;
        if (!whole_offset(top.fdarray, table.size(), fdarray_position)) return fail("bad FDArray offset");
        if (!parse_index(table, fdarray_position, fdarray_index, after)) return fail("bad FDArray INDEX");
        if (fdarray_index.count == 0) return fail("empty FDArray");
        have_fdarray = true;
    }
    bool have_fdselect = false;
    if (top.has_fdselect) {
        if (!whole_offset(top.fdselect, table.size(), fdselect_position)) return fail("bad FDSelect offset");
        have_fdselect = true;
    }

    if (have_fdarray && have_fdselect) {
        impl->privates.reserve(fdarray_index.count);
        for (usize i = 0; i < fdarray_index.count; ++i) {
            PrivateInfo priv{};
            bool ok = true;
            ok = scan_dict(index_item(table, fdarray_index, i), [&](Span<const f64> operands, u32 op) {
                if (op != kOpPrivate) return true;
                if (operands.size() != 2) return false;
                return read_private_dict(table, operands[0], operands[1], priv);
            });
            if (!ok) return fail("malformed Font DICT");
            impl->privates.push_back(priv);
        }
        if (!read_fd_select(table, fdselect_position, impl->glyph_count, impl->privates.size(), impl->fd_select)) {
            return fail("bad FDSelect");
        }
    } else {
        PrivateInfo priv{};
        if (top.has_private && !read_private_dict(table, top.private_size, top.private_offset, priv)) {
            return fail("bad Private DICT");
        }
        impl->privates.push_back(priv);
    }

    auto font = std::shared_ptr<CffFont>(new CffFont());
    font->impl_ = std::move(impl);
    font->glyph_count_ = font->impl_->glyph_count;
    font->is_cid_keyed_ = top.cid;
    return font;
}

bool CffFont::glyph_path(u32 glyph, GlyphPath& out) const {
    out.clear();
    if (!impl_ || glyph >= glyph_count_) return false;
    f64 width = 0.0;
    if (!interpret(*impl_, glyph, &out, false, width)) {
        out.clear(); // half an outline is worse than none: the caller was told it failed
        return false;
    }
    return true;
}

f32 CffFont::advance(u32 glyph) const {
    if (!impl_ || glyph >= glyph_count_) return 0.0f;
    f64 width = 0.0;
    // The width is settled by the first stack clearing operator, so the interpretation stops there.
    (void)interpret(*impl_, glyph, nullptr, true, width);
    if (!std::isfinite(width)) return 0.0f;
    return f32(width);
}

} // namespace t2d
