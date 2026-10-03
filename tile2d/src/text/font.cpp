#include <t2d/text/font.h>

#include <t2d/core/cli.h>
#include <t2d/core/log.h>
#include <t2d/text/cff.h>
#include <t2d/text/sfnt.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace t2d {
namespace {

constexpr u32 tag_of(const char (&text)[5]) {
    return (static_cast<u32>(text[0]) << 24) | (static_cast<u32>(text[1]) << 16) | (static_cast<u32>(text[2]) << 8) |
           static_cast<u32>(text[3]);
}

constexpr u32 kTagHead = tag_of("head");
constexpr u32 kTagHhea = tag_of("hhea");
constexpr u32 kTagHmtx = tag_of("hmtx");
constexpr u32 kTagMaxp = tag_of("maxp");
constexpr u32 kTagCmap = tag_of("cmap");
constexpr u32 kTagGlyf = tag_of("glyf");
constexpr u32 kTagLoca = tag_of("loca");
constexpr u32 kTagCff = tag_of("CFF ");
constexpr u32 kTagName = tag_of("name");
constexpr u32 kTagPost = tag_of("post");

constexpr u32 kSfntTrueType = 0x00010000u;
constexpr u32 kSfntTrue = 0x74727565u;
constexpr u32 kSfntOtto = 0x4F54544Fu;
constexpr u32 kTtcTag = 0x74746366u;

/// TrueType point flags.
constexpr u8 kOnCurve = 1u << 0;
constexpr u8 kXShort = 1u << 1;
constexpr u8 kYShort = 1u << 2;
constexpr u8 kRepeat = 1u << 3;
constexpr u8 kXSame = 1u << 4;
constexpr u8 kYSame = 1u << 5;

/// Composite glyph flags.
constexpr u16 kArgWords = 0x0001;
constexpr u16 kArgsAreXy = 0x0002;
constexpr u16 kHaveScale = 0x0008;
constexpr u16 kMoreComponents = 0x0020;
constexpr u16 kHaveXyScale = 0x0040;
constexpr u16 kHaveTwoByTwo = 0x0080;

/// Composites are placed by a 2x2 matrix plus a translation: x' = a*x + c*y + dx.
struct Placement {
    f32 a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, dx = 0.0f, dy = 0.0f;
};

/// Appends one TrueType contour. Points alternate between on-curve and off-curve; two off-curve points
/// in a row imply an on-curve point halfway between them, and a contour may start on an off-curve
/// point, in which case it starts at the implied midpoint.
void append_contour(GlyphPath& path, ConstSpan<const FontPoint> points, ConstSpan<const u8> on_curve,
                    usize first, usize last) {
    const usize count = last - first + 1;
    if (count == 0) return;

    FontPoint start_point = points[first];
    usize offset = 0;
    if (!on_curve[first]) {
        if (on_curve[last]) {
            start_point = points[last];
        } else {
            start_point = FontPoint{(points[last].x + points[first].x) * 0.5f,
                                    (points[last].y + points[first].y) * 0.5f};
            offset = 1;
        }
    }

    path.move_to(start_point.x, start_point.y);
    FontPoint previous = start_point;
    bool previous_on = true;
    for (usize index = 0; index < count; ++index) {
        const usize point_index = first + ((index + offset) % count);
        const FontPoint point = points[point_index];
        const bool is_on = on_curve[point_index];
        if (is_on) {
            if (previous_on) path.line_to(point.x, point.y);
            else path.quad_to(previous.x, previous.y, point.x, point.y);
        } else if (!previous_on) {
            const FontPoint middle{(previous.x + point.x) * 0.5f, (previous.y + point.y) * 0.5f};
            path.quad_to(previous.x, previous.y, middle.x, middle.y);
        }
        previous = point;
        previous_on = is_on;
    }
    if (!previous_on) path.quad_to(previous.x, previous.y, start_point.x, start_point.y);
    path.close();
}

[[nodiscard]] std::string utf16be_to_utf8(ConstSpan<const u8> bytes) {
    std::string out;
    for (usize index = 0; index + 1 < bytes.size(); index += 2) {
        u32 code = static_cast<u32>((static_cast<u16>(bytes[index]) << 8) | bytes[index + 1]);
        if (code >= 0xD800 && code <= 0xDBFF && index + 3 < bytes.size()) {
            const u32 low = static_cast<u32>((static_cast<u16>(bytes[index + 2]) << 8) | bytes[index + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                index += 2;
            }
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0u | (code >> 6)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0u | (code >> 12)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (code >> 18)));
            out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
    }
    return out;
}

/// Picks the most capable Unicode subtable of a cmap: format 12 over format 4, and a Unicode platform
/// over the legacy Mac one.
[[nodiscard]] int cmap_subtable_score(u16 platform, u16 encoding, u16 format) {
    if (format != 12 && format != 4 && format != 6 && format != 0) return -1;
    int score = format == 12 ? 30 : (format == 4 ? 20 : 10);
    if (platform == 3 && encoding == 10) score += 8;
    else if (platform == 3 && encoding == 1) score += 6;
    else if (platform == 0) score += 5;
    else if (platform == 1 && encoding == 0) score += 1;
    else score -= 5;
    return score;
}

} // namespace

// ------------------------------------------------------------------ GlyphPath --

void GlyphPath::move_to(f32 x, f32 y) {
    PathCommand command;
    command.verb = PathVerb::Move;
    command.points[0] = FontPoint{x, y};
    command.count = 1;
    commands_.push_back(command);
}

void GlyphPath::line_to(f32 x, f32 y) {
    PathCommand command;
    command.verb = PathVerb::Line;
    command.points[0] = FontPoint{x, y};
    command.count = 1;
    commands_.push_back(command);
}

void GlyphPath::quad_to(f32 cx, f32 cy, f32 x, f32 y) {
    PathCommand command;
    command.verb = PathVerb::Quad;
    command.points[0] = FontPoint{cx, cy};
    command.points[1] = FontPoint{x, y};
    command.count = 2;
    commands_.push_back(command);
}

void GlyphPath::cubic_to(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y) {
    PathCommand command;
    command.verb = PathVerb::Cubic;
    command.points[0] = FontPoint{c1x, c1y};
    command.points[1] = FontPoint{c2x, c2y};
    command.points[2] = FontPoint{x, y};
    command.count = 3;
    commands_.push_back(command);
}

void GlyphPath::close() {
    PathCommand command;
    command.verb = PathVerb::Close;
    command.count = 0;
    commands_.push_back(command);
}

bool GlyphPath::bounds(FontPoint& min, FontPoint& max) const {
    bool any = false;
    for (const PathCommand& command : commands_) {
        for (u8 index = 0; index < command.count; ++index) {
            const FontPoint point = command.points[index];
            if (!any) {
                min = point;
                max = point;
                any = true;
                continue;
            }
            min.x = std::min(min.x, point.x);
            min.y = std::min(min.y, point.y);
            max.x = std::max(max.x, point.x);
            max.y = std::max(max.y, point.y);
        }
    }
    return any;
}

// ---------------------------------------------------------------------- Font --

const Font::Table& Font::table(u32 tag) const {
    static const Table missing{};
    for (u32 index = 0; index < table_count_; ++index) {
        if (tables_[index].present && tables_[index].tag == tag) return tables_[index];
    }
    return missing;
}

ConstSpan<const u8> Font::table_bytes(u32 tag) const {
    const Table& found = table(tag);
    if (!found.present || found.offset > data_.size() || found.length > data_.size() - found.offset) return {};
    return ConstSpan<const u8>(data_.data() + found.offset, found.length);
}

std::vector<std::pair<std::string, std::string>> Font::face_names(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> names;
    const auto bytes = read_binary(path);
    if (!bytes.has_value()) return names;
    const ConstSpan<const u8> data(bytes->data(), bytes->size());
    BigEndianReader reader(data);
    const u32 sfnt = reader.read_u32();
    std::vector<usize> faces;
    if (sfnt == kTtcTag) {
        reader.read_u32(); // version
        const u32 count = reader.read_u32();
        if (!reader.ok() || count > 64) return names;
        for (u32 index = 0; index < count; ++index) faces.push_back(reader.read_u32());
    } else {
        faces.push_back(0);
    }

    for (const usize offset : faces) {
        std::string family;
        std::string style;
        // Only the table directory and the name table are read: the outlines of a CJK face are tens of
        // megabytes, which would make picking a face absurdly expensive.
        BigEndianReader header = reader.window(offset, 12);
        header.read_u32(); // sfnt version
        const u16 table_count = header.read_u16();
        header.skip(6);
        for (u16 index = 0; index < table_count; ++index) {
            BigEndianReader entry = reader.window(offset + 12 + static_cast<usize>(index) * 16, 16);
            const u32 tag = entry.read_u32();
            entry.read_u32();
            const u32 table_offset = entry.read_u32();
            const u32 table_length = entry.read_u32();
            if (tag != kTagName || !entry.ok()) continue;
            if (table_offset > data.size() || table_length > data.size() - table_offset) break;
            const ConstSpan<const u8> table = data.subspan(table_offset, table_length);
            BigEndianReader name_reader(table);
            name_reader.read_u16(); // format
            const u16 records = name_reader.read_u16();
            const u16 strings = name_reader.read_u16();
            int best_family = -1;
            int best_style = -1;
            for (u16 record_index = 0; record_index < records; ++record_index) {
                BigEndianReader record = name_reader.window(6 + static_cast<usize>(record_index) * 12, 12);
                const u16 platform = record.read_u16();
                record.read_u16();
                record.read_u16();
                const u16 name_id = record.read_u16();
                const u16 length = record.read_u16();
                const u16 string_offset = record.read_u16();
                if (!record.ok() || (name_id != 1 && name_id != 2)) continue;
                const int score = platform == 3 ? 3 : (platform == 0 ? 2 : 1);
                int& best = name_id == 1 ? best_family : best_style;
                if (score <= best) continue;
                const usize start = static_cast<usize>(strings) + string_offset;
                if (start > table.size() || length > table.size() - start) continue;
                const std::string text = utf16be_to_utf8(table.subspan(start, length));
                if (text.empty()) continue;
                best = score;
                if (name_id == 1) family = text;
                else style = text;
            }
            break;
        }
        names.emplace_back(family, style);
    }
    return names;
}

std::optional<Font> Font::load(const std::string& path, u32 face_index, std::string* error) {
    const auto bytes = read_binary(path);
    if (!bytes.has_value()) {
        if (error != nullptr) *error = std::format("cannot read '{}'", path);
        return std::nullopt;
    }
    return load_from_memory(*bytes, face_index, error);
}

std::optional<Font> Font::load_from_memory(std::vector<u8> data, u32 face_index, std::string* error) {
    const auto fail = [error](std::string message) -> std::optional<Font> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    if (data.size() < 12) return fail("the file is too short to be a font");

    Font font;
    font.data_ = std::move(data);

    BigEndianReader reader(ConstSpan<const u8>(font.data_.data(), font.data_.size()));
    u32 sfnt = reader.read_u32();
    usize directory = 0;
    if (sfnt == kTtcTag) {
        reader.read_u32(); // version
        const u32 faces = reader.read_u32();
        font.metrics_.face_count = faces;
        if (faces == 0) return fail("the collection contains no fonts");
        if (face_index >= faces) return fail(std::format("face {} does not exist ({} in the file)", face_index, faces));
        reader.skip(static_cast<usize>(face_index) * 4);
        directory = reader.read_u32();
        if (!reader.ok()) return fail("the collection header is truncated");
        sfnt = reader.window(directory, 4).read_u32();
    } else if (face_index != 0) {
        return fail("a single font file has only face 0");
    }
    font.metrics_.face_index = face_index;
    if (sfnt != kSfntTrueType && sfnt != kSfntTrue && sfnt != kSfntOtto) {
        return fail(std::format("unsupported sfnt version 0x{:08x}", sfnt));
    }

    // Table directory.
    BigEndianReader header = reader.window(directory, 12);
    header.read_u32(); // sfnt version
    const u16 table_count = header.read_u16();
    header.skip(6); // searchRange, entrySelector, rangeShift
    if (!header.ok()) return fail("the table directory is truncated");
    // The cap keeps the font object small, and a font that exceeds it is reported rather than
    // silently losing its later tables - "name" and "post" are alphabetically last.
    if (table_count > kMaxFontTables) {
        T2D_WARN("font: {} tables, only the first {} are kept", table_count, kMaxFontTables);
    }
    for (u16 index = 0; index < table_count && font.table_count_ < kMaxFontTables; ++index) {
        BigEndianReader entry = reader.window(directory + 12 + static_cast<usize>(index) * 16, 16);
        const u32 tag = entry.read_u32();
        entry.read_u32(); // checksum
        const u32 offset = entry.read_u32();
        const u32 length = entry.read_u32();
        if (!entry.ok()) return fail("the table directory is truncated");
        if (offset > font.data_.size() || length > font.data_.size() - offset) continue; // skip a broken table
        font.tables_[font.table_count_++] = Table{tag, offset, length, true};
    }
    if (font.table_count_ == 0) return fail("the font has no usable table");

    // head: units per em and the loca format.
    {
        const ConstSpan<const u8> head = font.table_bytes(kTagHead);
        BigEndianReader head_reader(head);
        head_reader.skip(18);
        const u16 units = head_reader.read_u16();
        head_reader.skip(30); // through the bounding box and the style fields
        const i16 loca_format = head_reader.read_i16();
        if (!head_reader.ok() || units == 0) return fail("the head table is missing or unusable");
        font.metrics_.units_per_em = static_cast<f32>(units);
        font.long_loca_ = loca_format != 0;
    }

    // maxp: glyph count.
    {
        BigEndianReader maxp(font.table_bytes(kTagMaxp));
        maxp.skip(4);
        font.metrics_.glyph_count = maxp.read_u16();
        if (!maxp.ok()) return fail("the maxp table is missing or unusable");
    }

    // hhea: vertical metrics.
    {
        BigEndianReader hhea(font.table_bytes(kTagHhea));
        hhea.skip(4);
        font.metrics_.ascender = static_cast<f32>(hhea.read_i16());
        font.metrics_.descender = static_cast<f32>(hhea.read_i16());
        font.metrics_.line_gap = static_cast<f32>(hhea.read_i16());
        if (!hhea.ok()) return fail("the hhea table is missing or unusable");
    }

    // post: underline metrics (the values live in the header, so version 3 still has them).
    {
        const ConstSpan<const u8> post = font.table_bytes(kTagPost);
        if (post.size() >= 12) {
            BigEndianReader post_reader(post);
            post_reader.skip(8);
            font.metrics_.underline_position = static_cast<f32>(post_reader.read_i16());
            font.metrics_.underline_thickness = static_cast<f32>(post_reader.read_i16());
        }
    }

    // cmap: choose one subtable and remember where it is.
    {
        const ConstSpan<const u8> cmap = font.table_bytes(kTagCmap);
        BigEndianReader cmap_reader(cmap);
        cmap_reader.read_u16(); // version
        const u16 records = cmap_reader.read_u16();
        int best_score = -1;
        for (u16 index = 0; index < records; ++index) {
            BigEndianReader record = cmap_reader.window(4 + static_cast<usize>(index) * 8, 8);
            const u16 platform = record.read_u16();
            const u16 encoding = record.read_u16();
            const u32 offset = record.read_u32();
            if (!record.ok()) break;
            const u16 format = cmap_reader.window(offset, 2).read_u16();
            const int score = cmap_subtable_score(platform, encoding, format);
            if (score <= best_score) continue;
            best_score = score;
            // Subtable offsets are relative to the cmap table, everything below reads the file: store
            // an absolute offset or every lookup lands somewhere else entirely.
            font.cmap_offset_ = font.table(kTagCmap).offset + offset;
            font.cmap_format_ = format;
        }
        if (best_score < 0) return fail("the font has no Unicode character map");
        if (font.cmap_offset_ >= font.data_.size()) return fail("the character map is out of bounds");
        font.cmap_length_ = font.data_.size();
    }

    // Outlines: "glyf" for TrueType, the CFF table for "OTTO".
    if (sfnt == kSfntOtto) {
        std::string cff_error;
        font.cff_ = CffFont::parse(font.table_bytes(kTagCff), &cff_error);
        if (font.cff_ == nullptr) return fail(std::format("the CFF table could not be read: {}", cff_error));
    } else {
        if (!font.table(kTagGlyf).present || !font.table(kTagLoca).present) {
            return fail("a TrueType font without glyf/loca");
        }
        font.glyf_offset_ = font.table(kTagGlyf).offset;
        font.loca_offset_ = font.table(kTagLoca).offset;
    }

    // name: family and style, for logs and for picking a fallback.
    {
        const ConstSpan<const u8> name = font.table_bytes(kTagName);
        BigEndianReader name_reader(name);
        name_reader.read_u16(); // format
        const u16 count = name_reader.read_u16();
        const u16 strings = name_reader.read_u16();
        // Family and style are scored independently: sharing one "best" lets whichever record comes
        // first in the table win and silently drops the other.
        int best_family = -1;
        int best_style = -1;
        for (u16 index = 0; index < count; ++index) {
            BigEndianReader record = name_reader.window(6 + static_cast<usize>(index) * 12, 12);
            const u16 platform = record.read_u16();
            record.read_u16(); // encoding
            record.read_u16(); // language
            const u16 name_id = record.read_u16();
            const u16 length = record.read_u16();
            const u16 offset = record.read_u16();
            if (!record.ok() || (name_id != 1 && name_id != 2)) continue;
            const int score = platform == 3 ? 3 : (platform == 0 ? 2 : 1);
            int& best = name_id == 1 ? best_family : best_style;
            if (score <= best) continue;
            const usize start = static_cast<usize>(strings) + offset;
            if (start > name.size() || length > name.size() - start) continue;
            const std::string text = utf16be_to_utf8(name.subspan(start, length));
            if (text.empty()) continue;
            best = score;
            if (name_id == 1) font.family_name_ = text;
            else font.style_name_ = text;
        }
    }

    T2D_DEBUG("font: '{} {}' face {}/{}, {} glyphs, {} units/em, {} outlines", font.family_name_,
              font.style_name_, face_index, font.metrics_.face_count, font.metrics_.glyph_count,
              static_cast<u32>(font.metrics_.units_per_em), font.cff_ != nullptr ? "CFF" : "TrueType");
    return font;
}

u32 Font::glyph_index(u32 codepoint) const {
    if (cmap_format_ == 12) {
        BigEndianReader reader(ConstSpan<const u8>(data_.data() + cmap_offset_, cmap_length_ - cmap_offset_));
        reader.skip(12); // format, reserved, length, language
        const u32 groups = reader.read_u32();
        if (!reader.ok()) return 0;
        u32 low = 0;
        u32 high = groups;
        while (low < high) {
            const u32 middle = low + (high - low) / 2;
            BigEndianReader group = reader.window(16 + static_cast<usize>(middle) * 12, 12);
            const u32 start = group.read_u32();
            const u32 end = group.read_u32();
            const u32 start_glyph = group.read_u32();
            if (!group.ok()) return 0;
            if (codepoint < start) {
                high = middle;
            } else if (codepoint > end) {
                low = middle + 1;
            } else {
                return start_glyph + (codepoint - start);
            }
        }
        return 0;
    }
    if (cmap_format_ == 4) {
        BigEndianReader reader(ConstSpan<const u8>(data_.data() + cmap_offset_, cmap_length_ - cmap_offset_));
        reader.skip(6); // format, length, language
        const u16 segments = static_cast<u16>(reader.read_u16() / 2);
        if (!reader.ok() || segments == 0) return 0;
        const usize end_codes = cmap_offset_ + 14;
        const usize start_codes = end_codes + static_cast<usize>(segments) * 2 + 2;
        const usize deltas = start_codes + static_cast<usize>(segments) * 2;
        const usize ranges = deltas + static_cast<usize>(segments) * 2;
        for (u16 segment = 0; segment < segments; ++segment) {
            BigEndianReader end_reader(ConstSpan<const u8>(data_.data() + end_codes + static_cast<usize>(segment) * 2, 2));
            const u16 end = end_reader.read_u16();
            if (codepoint > end) continue;
            BigEndianReader start_reader(
                ConstSpan<const u8>(data_.data() + start_codes + static_cast<usize>(segment) * 2, 2));
            const u16 start = start_reader.read_u16();
            if (codepoint < start) return 0;
            BigEndianReader delta_reader(
                ConstSpan<const u8>(data_.data() + deltas + static_cast<usize>(segment) * 2, 2));
            const i16 delta = delta_reader.read_i16();
            BigEndianReader range_reader(
                ConstSpan<const u8>(data_.data() + ranges + static_cast<usize>(segment) * 2, 2));
            const u16 range_offset = range_reader.read_u16();
            if (range_offset == 0) {
                return static_cast<u32>((static_cast<i32>(codepoint) + delta) & 0xFFFF);
            }
            const usize glyph_address =
                ranges + static_cast<usize>(segment) * 2 + range_offset + (codepoint - start) * 2;
            if (glyph_address + 2 > data_.size()) return 0;
            BigEndianReader glyph_reader(ConstSpan<const u8>(data_.data() + glyph_address, 2));
            const u16 glyph = glyph_reader.read_u16();
            if (glyph == 0) return 0;
            return static_cast<u32>((static_cast<i32>(glyph) + delta) & 0xFFFF);
        }
        return 0;
    }
    if (cmap_format_ == 6) {
        BigEndianReader reader(ConstSpan<const u8>(data_.data() + cmap_offset_, cmap_length_ - cmap_offset_));
        reader.skip(6);
        const u16 first = reader.read_u16();
        const u16 count = reader.read_u16();
        if (!reader.ok() || codepoint < first || codepoint >= static_cast<u32>(first) + count) return 0;
        reader.skip(static_cast<usize>(codepoint - first) * 2);
        return reader.read_u16();
    }
    if (cmap_format_ == 0) {
        if (codepoint > 255) return 0;
        const usize address = cmap_offset_ + 6 + codepoint;
        if (address >= data_.size()) return 0;
        return data_[address];
    }
    return 0;
}

f32 Font::advance(u32 glyph) const {
    const ConstSpan<const u8> hmtx = table_bytes(kTagHmtx);
    if (hmtx.size() >= 4) {
        BigEndianReader hhea(table_bytes(kTagHhea));
        hhea.skip(34);
        const u16 metrics = hhea.read_u16();
        if (hhea.ok() && metrics > 0) {
            const u32 index = std::min<u32>(glyph, metrics - 1u);
            const usize address = static_cast<usize>(index) * 4;
            if (address + 2 <= hmtx.size()) {
                BigEndianReader reader(hmtx);
                reader.skip(address);
                return static_cast<f32>(reader.read_u16());
            }
        }
    }
    if (cff_ != nullptr) return cff_->advance(glyph);
    return 0.0f;
}

f32 Font::bearing(u32 glyph) const {
    if (cff_ != nullptr) {
        GlyphPath path;
        if (!cff_->glyph_path(glyph, path)) return 0.0f;
        FontPoint min{};
        FontPoint max{};
        return path.bounds(min, max) ? min.x : 0.0f;
    }
    const usize address = [&] {
        const ConstSpan<const u8> loca = table_bytes(kTagLoca);
        BigEndianReader reader(loca);
        if (long_loca_) {
            reader.skip(static_cast<usize>(glyph) * 4);
            return static_cast<usize>(reader.read_u32());
        }
        reader.skip(static_cast<usize>(glyph) * 2);
        return static_cast<usize>(reader.read_u16()) * 2u;
    }();
    const usize glyph_address = glyf_offset_ + address;
    if (glyph_address + 10 > data_.size()) return 0.0f;
    BigEndianReader reader(ConstSpan<const u8>(data_.data() + glyph_address, 10));
    reader.read_i16(); // numberOfContours
    return static_cast<f32>(reader.read_i16()); // xMin
}

bool Font::parse_glyf_glyph(u32 glyph, GlyphPath& out, u32 depth) const {
    if (depth > 5) return false; // a composite chain this deep is a broken font
    const ConstSpan<const u8> loca = table_bytes(kTagLoca);
    if (loca.empty()) return false;
    usize start = 0;
    usize end = 0;
    {
        BigEndianReader reader(loca);
        if (long_loca_) {
            reader.skip(static_cast<usize>(glyph) * 4);
            start = reader.read_u32();
            end = reader.read_u32();
        } else {
            reader.skip(static_cast<usize>(glyph) * 2);
            start = static_cast<usize>(reader.read_u16()) * 2u;
            end = static_cast<usize>(reader.read_u16()) * 2u;
        }
        if (!reader.ok()) return false;
    }
    if (end <= start) return true; // a blank glyph: no outline, but perfectly valid

    const usize address = glyf_offset_ + start;
    const usize length = end - start;
    if (address + length > data_.size()) return false;
    BigEndianReader reader(ConstSpan<const u8>(data_.data() + address, length));
    const i16 contours = reader.read_i16();
    reader.skip(8); // bounding box
    if (!reader.ok()) return false;

    if (contours < 0) {
        // Composite: place other glyphs with a matrix and a translation.
        bool first = true;
        for (;;) {
            const u16 flags = reader.read_u16();
            const u32 component = reader.read_u16();
            if (!reader.ok()) return false;
            i32 dx = 0;
            i32 dy = 0;
            if ((flags & kArgWords) != 0u) {
                dx = reader.read_i16();
                dy = reader.read_i16();
            } else {
                dx = static_cast<i8>(reader.read_u8());
                dy = static_cast<i8>(reader.read_u8());
            }
            if ((flags & kArgsAreXy) == 0u) {
                // Point matching: the component is attached to a point of the parent outline. Rare
                // enough that it is reported instead of guessed.
                T2D_WARN("font: composite glyph {} uses point matching, which is not supported", glyph);
                return false;
            }
            Placement placement;
            placement.dx = static_cast<f32>(dx);
            placement.dy = static_cast<f32>(dy);
            if ((flags & kHaveScale) != 0u) {
                BigEndianReader scale(reader.bytes(2));
                const f32 value = static_cast<f32>(scale.read_i16()) / 16384.0f;
                placement.a = value;
                placement.d = value;
            } else if ((flags & kHaveXyScale) != 0u) {
                BigEndianReader scale(reader.bytes(4));
                placement.a = static_cast<f32>(scale.read_i16()) / 16384.0f;
                placement.d = static_cast<f32>(scale.read_i16()) / 16384.0f;
            } else if ((flags & kHaveTwoByTwo) != 0u) {
                BigEndianReader scale(reader.bytes(8));
                placement.a = static_cast<f32>(scale.read_i16()) / 16384.0f;
                placement.b = static_cast<f32>(scale.read_i16()) / 16384.0f;
                placement.c = static_cast<f32>(scale.read_i16()) / 16384.0f;
                placement.d = static_cast<f32>(scale.read_i16()) / 16384.0f;
            }
            if (!reader.ok()) return false;

            GlyphPath component_path;
            if (!parse_glyf_glyph(component, component_path, depth + 1)) return false;
            for (const PathCommand& command : component_path.commands()) {
                PathCommand transformed = command;
                for (u8 index = 0; index < command.count; ++index) {
                    const FontPoint point = command.points[index];
                    transformed.points[index] = FontPoint{placement.a * point.x + placement.c * point.y + placement.dx,
                                                          placement.b * point.x + placement.d * point.y + placement.dy};
                }
                if (first && transformed.verb == PathVerb::Move) first = false;
                out.commands_.push_back(transformed);
            }
            first = false;
            if ((flags & kMoreComponents) == 0u) break;
        }
        return true;
    }

    // Simple glyph.
    std::vector<u16> ends(static_cast<usize>(contours));
    for (i16 index = 0; index < contours; ++index) ends[static_cast<usize>(index)] = reader.read_u16();
    const u16 instructions = reader.read_u16();
    reader.skip(instructions);
    if (!reader.ok()) return false;
    const usize points = contours == 0 ? 0 : static_cast<usize>(ends.back()) + 1;
    if (points == 0) return true;
    if (points > 10000) return false; // a sanity bound: real glyphs are far below this

    std::vector<u8> flags;
    flags.reserve(points);
    while (flags.size() < points) {
        const u8 flag = reader.read_u8();
        if (!reader.ok()) return false;
        flags.push_back(flag);
        if ((flag & kRepeat) == 0u) continue;
        u8 repeat = reader.read_u8();
        if (!reader.ok()) return false;
        while (repeat-- > 0 && flags.size() < points) flags.push_back(flag);
    }

    std::vector<FontPoint> coordinates(points);
    i32 value = 0;
    for (usize index = 0; index < points; ++index) {
        const u8 flag = flags[index];
        if ((flag & kXShort) != 0u) {
            const u8 delta = reader.read_u8();
            value += (flag & kXSame) != 0u ? static_cast<i32>(delta) : -static_cast<i32>(delta);
        } else if ((flag & kXSame) == 0u) {
            value += reader.read_i16();
        }
        coordinates[index].x = static_cast<f32>(value);
    }
    value = 0;
    for (usize index = 0; index < points; ++index) {
        const u8 flag = flags[index];
        if ((flag & kYShort) != 0u) {
            const u8 delta = reader.read_u8();
            value += (flag & kYSame) != 0u ? static_cast<i32>(delta) : -static_cast<i32>(delta);
        } else if ((flag & kYSame) == 0u) {
            value += reader.read_i16();
        }
        coordinates[index].y = static_cast<f32>(value);
    }
    if (!reader.ok()) return false;

    // std::vector<bool> cannot hand out a span, so the flags live in bytes.
    std::vector<u8> on_curve(points);
    for (usize index = 0; index < points; ++index) on_curve[index] = (flags[index] & kOnCurve) != 0u ? 1u : 0u;

    usize first = 0;
    for (const u16 contour_end : ends) {
        const usize last = contour_end;
        if (last >= points || last < first) return false;
        append_contour(out, ConstSpan<const FontPoint>(coordinates.data(), coordinates.size()),
                       ConstSpan<const u8>(on_curve.data(), on_curve.size()), first, last);
        first = last + 1;
    }
    return true;
}

bool Font::glyph_path(u32 glyph, GlyphPath& out) const {
    out.clear();
    if (glyph >= metrics_.glyph_count) return false;
    if (cff_ != nullptr) return cff_->glyph_path(glyph, out);
    return parse_glyf_glyph(glyph, out, 0);
}

} // namespace t2d
