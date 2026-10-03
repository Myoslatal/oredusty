// Tile2D - the CFF (Type 2 charstring) interpreter.
//
// Two kinds of test live here.
//
// The synthetic ones build a whole CFF table byte by byte, so every operator can be checked against
// coordinates worked out on paper: the width rule of the first stack clearing operator, the
// alternating hlineto/vlineto and hvcurveto/vhcurveto runs, the optional leading delta of
// hhcurveto/vvcurveto, the flex family, subroutine bias and the CID keyed FDSelect/FDArray path.
//
// The real ones read the Noto CJK collections that ship with this machine: face 0 of each is a CID
// keyed CFF font with 65535 glyphs, and every one of them is extracted and checked for a well formed
// outline. Those fonts are where the awkward cases come from - hint masks with hundreds of stems,
// nested subroutines, 16.16 fixed point operands - and where the blank glyphs (the space and
// friends) are found.
#include <t2d/text/cff.h>
#include <t2d/text/font.h>

#include <support/test_support.h>

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace t2d;

namespace {

// ------------------------------------------------------------- reading files --

[[nodiscard]] std::vector<u8> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

[[nodiscard]] u32 be_u16(ConstSpan<const u8> data, usize position) {
    if (position + 2 > data.size()) return 0;
    return (u32(data[position]) << 8) | u32(data[position + 1]);
}

[[nodiscard]] u32 be_u32(ConstSpan<const u8> data, usize position) {
    if (position + 4 > data.size()) return 0;
    return (u32(data[position]) << 24) | (u32(data[position + 1]) << 16) | (u32(data[position + 2]) << 8) |
           u32(data[position + 3]);
}

constexpr u32 kTagCff = 0x43464620u;  // 'CFF '
constexpr u32 kTagHhea = 0x68686561u; // 'hhea'
constexpr u32 kTagHmtx = 0x686D7478u; // 'hmtx'

/// The offset of one face's sfnt table directory inside a .ttc. The TTC header is the tag 'ttcf', a
/// version, the number of faces and then one offset per face.
[[nodiscard]] bool ttc_directory(ConstSpan<const u8> file, u32 face, usize& out, std::string& error) {
    if (file.size() < 12 || be_u32(file, 0) != 0x74746366u) {
        error = "not a TrueType collection";
        return false;
    }
    if (face >= be_u32(file, 8)) {
        error = "face index out of range";
        return false;
    }
    out = be_u32(file, 12 + 4 * usize(face));
    if (out + 12 > file.size()) {
        error = "table directory out of range";
        return false;
    }
    return true;
}

/// The bytes of one table of one face: the sfnt directory holds the tag, checksum, offset and length
/// of every table it has.
[[nodiscard]] std::vector<u8> face_table(ConstSpan<const u8> file, usize directory, u32 tag) {
    const u32 table_count = be_u16(file, directory + 4);
    for (u32 i = 0; i < table_count; ++i) {
        const usize record = directory + 12 + 16 * usize(i);
        if (record + 16 > file.size()) break;
        if (be_u32(file, record) != tag) continue;
        const usize offset = be_u32(file, record + 8);
        const usize length = be_u32(file, record + 12);
        if (offset > file.size() || length > file.size() - offset) break;
        return std::vector<u8>(file.begin() + offset, file.begin() + offset + length);
    }
    return {};
}

// ------------------------------------------------------------- the real fonts --

constexpr const char* kSansPath = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc";
constexpr const char* kSerifPath = "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc";
constexpr f32 kUnitsPerEm = 1000.0f; // Noto CJK is a 1000 unit em
constexpr f32 kCoordinateLimit = 4.0f * kUnitsPerEm;
constexpr f32 kAdvanceLimit = 3.0f * kUnitsPerEm;

struct RealFont {
    bool available = false;
    std::string error;
    std::vector<u8> file;  ///< the whole collection, kept for the hmtx check
    usize directory = 0;   ///< the sfnt table directory of face 0
    std::vector<u8> table; ///< the "CFF " table of face 0
    std::shared_ptr<const CffFont> font;
};

[[nodiscard]] RealFont load_real_font(const char* path) {
    RealFont out;
    out.file = read_file(path);
    if (out.file.empty()) {
        out.error = std::string("cannot read ") + path;
        return out;
    }
    if (!ttc_directory(ConstSpan<const u8>(out.file), 0, out.directory, out.error)) return out;
    out.table = face_table(ConstSpan<const u8>(out.file), out.directory, kTagCff);
    if (out.table.empty()) {
        out.error = "the face has no CFF table";
        return out;
    }
    out.font = CffFont::parse(ConstSpan<const u8>(out.table), &out.error);
    out.available = out.font != nullptr;
    return out;
}

/// Parsed once: the Sans collection is 15 MB and the tests that use it all want the same context.
[[nodiscard]] const RealFont& sans_font() {
    static const RealFont font = load_real_font(kSansPath);
    return font;
}
[[nodiscard]] const RealFont& serif_font() {
    static const RealFont font = load_real_font(kSerifPath);
    return font;
}

/// The advance widths of the hmtx table: numberOfHMetrics entries of four bytes, the last one
/// repeated for the glyphs beyond it.
[[nodiscard]] bool read_hmtx(ConstSpan<const u8> file, usize directory, u32 glyph_count, std::vector<u16>& out) {
    const std::vector<u8> hhea = face_table(file, directory, kTagHhea);
    const std::vector<u8> hmtx = face_table(file, directory, kTagHmtx);
    if (hhea.size() < 36 || hmtx.empty()) return false;
    const u32 long_metrics = be_u16(ConstSpan<const u8>(hhea), 34);
    if (long_metrics == 0) return false;
    out.assign(glyph_count, 0);
    for (u32 glyph = 0; glyph < glyph_count; ++glyph) {
        const usize index = glyph < long_metrics ? usize(glyph) : usize(long_metrics - 1);
        if (index * 4 + 2 > hmtx.size()) return false;
        out[glyph] = u16(be_u16(ConstSpan<const u8>(hmtx), index * 4));
    }
    return true;
}

// ------------------------------------------------------------ path checking --

/// What a well formed outline must look like: every contour opens with a Move, holds lines and
/// curves and ends with a Close, no command carries the wrong number of points, and every
/// coordinate is a finite number inside the limit.
struct PathReport {
    u32 commands = 0;
    u32 contours = 0;
    u32 lines = 0;
    u32 curves = 0;
    bool well_formed = true;
    bool finite = true;
    bool within_limit = true;
    bool have_bounds = false;
    FontPoint min{};
    FontPoint max{};
    std::string_view problem{}; ///< the first thing that was wrong
};

void note_bad(PathReport& report, std::string_view problem) {
    if (report.well_formed) {
        report.well_formed = false;
        report.problem = problem;
    }
}

[[nodiscard]] PathReport inspect_path(ConstSpan<PathCommand> commands, f32 limit) {
    PathReport report;
    bool open = false;
    for (const PathCommand& command : commands) {
        ++report.commands;
        u8 expected = 0;
        switch (command.verb) {
        case PathVerb::Move:
        case PathVerb::Line:
            expected = 1;
            break;
        case PathVerb::Quad:
            expected = 2;
            break;
        case PathVerb::Cubic:
            expected = 3;
            break;
        case PathVerb::Close:
            expected = 0;
            break;
        }
        if (command.count != expected) note_bad(report, "wrong point count");
        if (command.verb == PathVerb::Move) {
            if (open) note_bad(report, "a contour opens before the previous one is closed");
            open = true;
            ++report.contours;
        } else if (command.verb == PathVerb::Close) {
            if (!open) note_bad(report, "a contour closes without a Move");
            open = false;
        } else {
            if (!open) note_bad(report, "a line or curve is drawn outside a contour");
            if (command.verb == PathVerb::Line) ++report.lines; else ++report.curves;
        }
        for (u8 i = 0; i < command.count && i < 3; ++i) {
            const FontPoint point = command.points[i];
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) report.finite = false;
            if (std::fabs(point.x) > limit || std::fabs(point.y) > limit) report.within_limit = false;
            if (!report.have_bounds) {
                report.min = point;
                report.max = point;
                report.have_bounds = true;
            } else {
                if (point.x < report.min.x) report.min.x = point.x;
                if (point.y < report.min.y) report.min.y = point.y;
                if (point.x > report.max.x) report.max.x = point.x;
                if (point.y > report.max.y) report.max.y = point.y;
            }
        }
    }
    if (open) note_bad(report, "the last contour is not closed");
    if (!commands.empty() && commands.front().verb != PathVerb::Move) note_bad(report, "no Move first");
    return report;
}

/// The tally of one sweep over a font. Every number here is checked by a test.
struct Extraction {
    u32 glyphs = 0;
    u32 failures = 0;
    u32 first_failure = kInvalidId;
    u32 ill_formed = 0;
    u32 non_finite = 0;
    u32 out_of_bounds = 0;
    u32 blank = 0;
    u32 non_empty = 0;
    u32 contours = 0;
    u32 lines = 0;
    u32 curves = 0;
    u32 zero_advance = 0;
    u32 advance_out_of_range = 0;
    u32 widest_glyph = kInvalidId;
    f32 widest_outline = 0.0f;
};

[[nodiscard]] Extraction extract(const CffFont& font, u32 step, f32 coordinate_limit) {
    Extraction out;
    GlyphPath path;
    for (u32 glyph = 0; glyph < font.glyph_count(); glyph += step) {
        ++out.glyphs;
        if (!font.glyph_path(glyph, path)) {
            if (out.failures == 0) out.first_failure = glyph;
            ++out.failures;
            continue;
        }
        const PathReport report = inspect_path(path.commands(), coordinate_limit);
        out.contours += report.contours;
        out.lines += report.lines;
        out.curves += report.curves;
        if (!report.well_formed) ++out.ill_formed;
        if (!report.finite) ++out.non_finite;
        if (!report.within_limit) ++out.out_of_bounds;
        if (path.empty()) ++out.blank; else ++out.non_empty;
        if (report.have_bounds && report.max.x - report.min.x > out.widest_outline) {
            out.widest_outline = report.max.x - report.min.x;
            out.widest_glyph = glyph;
        }
        const f32 advance = font.advance(glyph);
        if (!(advance > 0.0f)) ++out.zero_advance;
        if (!(advance >= 0.0f) || advance > kAdvanceLimit) ++out.advance_out_of_range;
    }
    return out;
}

// -------------------------------------------------------------- path compare --

/// One expected command, so a synthetic test can spell out the outline it drew on paper.
struct Step {
    PathVerb verb = PathVerb::Move;
    FontPoint points[3]{};
    u8 count = 0;
};

[[nodiscard]] Step step(PathVerb verb, std::initializer_list<FontPoint> points) {
    Step out;
    out.verb = verb;
    for (const FontPoint& point : points) {
        if (out.count < 3) out.points[out.count] = point;
        ++out.count;
    }
    return out;
}
[[nodiscard]] Step m(f32 x, f32 y) { return step(PathVerb::Move, {{x, y}}); }
[[nodiscard]] Step l(f32 x, f32 y) { return step(PathVerb::Line, {{x, y}}); }
[[nodiscard]] Step c(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y) {
    return step(PathVerb::Cubic, {{c1x, c1y}, {c2x, c2y}, {x, y}});
}
[[nodiscard]] Step close_contour() { return step(PathVerb::Close, {}); }

void check_path(const GlyphPath& path, std::initializer_list<Step> expected, std::string_view what) {
    const ConstSpan<PathCommand> commands = path.commands();
    const bool same_size = commands.size() == expected.size();
    T2D_CHECK_MSG(same_size, "{}: expected {} commands, got {}", what, expected.size(), commands.size());
    if (!same_size) return;
    usize index = 0;
    for (const Step& want : expected) {
        const PathCommand& have = commands[index];
        bool ok = have.verb == want.verb && have.count == want.count;
        for (u8 i = 0; ok && i < want.count; ++i) {
            ok = std::fabs(have.points[i].x - want.points[i].x) < 0.01f &&
                 std::fabs(have.points[i].y - want.points[i].y) < 0.01f;
        }
        T2D_CHECK_MSG(ok, "{}: command {} is not the expected one", what, index);
        ++index;
    }
}

// ---------------------------------------------------------- synthetic tables --

/// The operand encoding shared by DICTs and Type 2 charstrings. The short forms are used for
/// charstrings; DICT operands are always written as 32 bit integers, so a DICT has the same size
/// whatever its offsets turn out to be and the layout can be computed before the offsets are known.
class Code {
public:
    Code& compact(i32 value) {
        if (value >= -107 && value <= 107) {
            bytes_.push_back(u8(value + 139));
        } else if (value >= 108 && value <= 1131) {
            const i32 biased = value - 108;
            bytes_.push_back(u8(247 + (biased >> 8)));
            bytes_.push_back(u8(biased & 0xFF));
        } else if (value <= -108 && value >= -1131) {
            const i32 biased = -value - 108;
            bytes_.push_back(u8(251 + (biased >> 8)));
            bytes_.push_back(u8(biased & 0xFF));
        } else {
            bytes_.push_back(28);
            bytes_.push_back(u8((value >> 8) & 0xFF));
            bytes_.push_back(u8(value & 0xFF));
        }
        return *this;
    }
    Code& fixed(i32 value) {
        bytes_.push_back(29);
        bytes_.push_back(u8((u32(value) >> 24) & 0xFFu));
        bytes_.push_back(u8((u32(value) >> 16) & 0xFFu));
        bytes_.push_back(u8((u32(value) >> 8) & 0xFFu));
        bytes_.push_back(u8(u32(value) & 0xFFu));
        return *this;
    }
    Code& numbers(std::initializer_list<i32> values) {
        for (const i32 value : values) compact(value);
        return *this;
    }
    Code& op(u8 byte) {
        bytes_.push_back(byte);
        return *this;
    }
    Code& escaped(u8 second) {
        bytes_.push_back(12);
        bytes_.push_back(second);
        return *this;
    }
    Code& raw(const std::vector<u8>& bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        return *this;
    }
    [[nodiscard]] const std::vector<u8>& bytes() const { return bytes_; }
    [[nodiscard]] std::vector<u8> take() { return std::move(bytes_); }

private:
    std::vector<u8> bytes_;
};

void append(std::vector<u8>& out, const std::vector<u8>& bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Where a table built by SynthCff keeps its CharStrings INDEX: the Top DICT spells the offset as a
/// 32 bit operand followed by the CharStrings operator. The Top DICT sits near the front, so the
/// first match is the real one.
[[nodiscard]] usize charstrings_index_of(const std::vector<u8>& table) {
    for (usize i = 0; i + 6 <= table.size(); ++i) {
        if (table[i] == 29 && table[i + 5] == 17) return be_u32(ConstSpan<const u8>(table), i + 1);
    }
    return 0;
}

/// A CFF INDEX: count, offSize, count + 1 offsets and the data. An empty INDEX is just a zero count.
[[nodiscard]] std::vector<u8> make_index(const std::vector<std::vector<u8>>& items) {
    std::vector<u8> out;
    const usize count = items.size();
    out.push_back(u8((count >> 8) & 0xFFu));
    out.push_back(u8(count & 0xFFu));
    if (count == 0) return out;

    usize total = 1;
    for (const std::vector<u8>& item : items) total += item.size();
    u8 off_size = 1;
    while (off_size < 4 && total > ((usize(1) << (8 * usize(off_size))) - 1)) ++off_size;
    out.push_back(off_size);
    const auto push_offset = [&](usize value) {
        for (u8 i = 0; i < off_size; ++i) {
            out.push_back(u8((value >> (8 * usize(off_size - 1 - i))) & 0xFFu));
        }
    };
    usize running = 1;
    push_offset(running);
    for (const std::vector<u8>& item : items) {
        running += item.size();
        push_offset(running);
    }
    for (const std::vector<u8>& item : items) out.insert(out.end(), item.begin(), item.end());
    return out;
}

/// A hand built CFF table: the four INDEXes the header is followed by, a Top DICT, one CharStrings
/// INDEX, and either a single Private DICT (a plain font) or an FDArray with one Private DICT per
/// font DICT plus an FDSelect (a CID keyed font).
struct SynthCff {
    std::vector<std::vector<u8>> glyphs;
    std::vector<u8> extra_private; ///< entries written in front of the width entries
    std::vector<std::vector<u8>> local_subrs;
    std::vector<std::vector<u8>> global_subrs;
    bool include_widths = true;
    i32 default_width = 500;
    i32 nominal_width = 400;

    bool cid = false;
    bool range_select = false; ///< write FDSelect format 3 instead of format 0
    std::vector<std::vector<u8>> fd_extra_private;
    std::vector<std::vector<std::vector<u8>>> fd_subrs;
    std::vector<i32> fd_default_width;
    std::vector<i32> fd_nominal_width;
    std::vector<u8> fd_select;

    [[nodiscard]] std::vector<u8> build() const {
        const usize fd_count = cid ? fd_extra_private.size() : 1;
        const auto subrs_of = [&](usize fd) -> const std::vector<std::vector<u8>>& {
            return cid ? fd_subrs[fd] : local_subrs;
        };

        const std::vector<u8> name_index = make_index({{u8('T'), u8('e'), u8('s'), u8('t')}});
        const std::vector<u8> string_index = make_index({});
        const std::vector<u8> gsubr_index = make_index(global_subrs);
        const std::vector<u8> charstrings_index = make_index(glyphs);

        // Sizes first: every DICT operand is five bytes, so nothing changes size once the offsets
        // are known and the layout can be computed from these probes.
        const usize top_size = top_dict(0, 0, 0, 0, 0).bytes().size();
        const usize head = 4 + name_index.size() + make_index({std::vector<u8>(top_size, 0)}).size() +
                           string_index.size() + gsubr_index.size();
        std::vector<usize> private_size(fd_count, 0);
        std::vector<usize> subrs_size(fd_count, 0);
        for (usize fd = 0; fd < fd_count; ++fd) {
            private_size[fd] = private_dict(fd, 0).bytes().size();
            subrs_size[fd] = make_index(subrs_of(fd)).size();
        }
        const std::vector<u8> fdarray_probe =
            cid ? fd_array(private_size, std::vector<usize>(fd_count, 0)).bytes() : std::vector<u8>{};
        const std::vector<u8> fdselect_probe = cid ? fd_select_blob().take() : std::vector<u8>{};

        usize cursor = head;
        const usize charstrings_offset = cursor;
        cursor += charstrings_index.size();
        usize fdarray_offset = 0;
        if (cid) {
            fdarray_offset = cursor;
            cursor += fdarray_probe.size();
        }
        std::vector<usize> private_offset(fd_count, 0);
        for (usize fd = 0; fd < fd_count; ++fd) {
            private_offset[fd] = cursor;
            cursor += private_size[fd];
        }
        std::vector<usize> subrs_offset(fd_count, 0);
        for (usize fd = 0; fd < fd_count; ++fd) {
            subrs_offset[fd] = cursor;
            cursor += subrs_size[fd];
        }
        usize fdselect_offset = 0;
        if (cid) {
            fdselect_offset = cursor;
            cursor += fdselect_probe.size();
        }

        std::vector<u8> out;
        out.push_back(1); // major
        out.push_back(0); // minor
        out.push_back(4); // hdrSize
        out.push_back(1); // offSize
        append(out, name_index);
        append(out, make_index({top_dict(charstrings_offset, private_offset[0], private_size[0], fdarray_offset,
                                        fdselect_offset)
                                    .take()}));
        append(out, string_index);
        append(out, gsubr_index);
        append(out, charstrings_index);
        if (cid) append(out, fd_array(private_size, private_offset).take());
        for (usize fd = 0; fd < fd_count; ++fd) {
            append(out, private_dict(fd, i32(subrs_offset[fd] - private_offset[fd])).take());
        }
        for (usize fd = 0; fd < fd_count; ++fd) append(out, make_index(subrs_of(fd)));
        if (cid) append(out, fd_select_blob().take());
        return out;
    }

    [[nodiscard]] Code top_dict(usize charstrings, usize private_offset, usize private_size, usize fdarray,
                                usize fdselect) const {
        Code dict;
        dict.fixed(i32(charstrings)).op(17);
        if (cid) {
            dict.fixed(0).fixed(0).fixed(0).escaped(30); // ROS, as a CID keyed font must have
            dict.fixed(i32(fd_count())).escaped(34);     // CIDCount
            dict.fixed(i32(fdarray)).escaped(36);        // FDArray
            dict.fixed(i32(fdselect)).escaped(37);       // FDSelect
        } else {
            dict.fixed(i32(private_size)).fixed(i32(private_offset)).op(18);
        }
        return dict;
    }

    [[nodiscard]] usize fd_count() const { return cid ? fd_extra_private.size() : 1; }

    [[nodiscard]] Code private_dict(usize fd, i32 subrs_relative) const {
        Code dict;
        dict.raw(cid ? fd_extra_private[fd] : extra_private);
        if (!(cid ? fd_subrs[fd] : local_subrs).empty()) dict.fixed(subrs_relative).op(19);
        if (include_widths) {
            dict.fixed(cid ? fd_default_width[fd] : default_width).op(20);
            dict.fixed(cid ? fd_nominal_width[fd] : nominal_width).op(21);
        }
        return dict;
    }

    [[nodiscard]] Code fd_array(const std::vector<usize>& sizes, const std::vector<usize>& offsets) const {
        std::vector<std::vector<u8>> dicts;
        for (usize fd = 0; fd < sizes.size(); ++fd) {
            dicts.push_back(Code().fixed(i32(sizes[fd])).fixed(i32(offsets[fd])).op(18).take());
        }
        return Code().raw(make_index(dicts));
    }

    /// FDSelect format 0 is one byte per glyph, format 3 is a run length encoded range list ending
    /// in the glyph count.
    [[nodiscard]] Code fd_select_blob() const {
        Code out;
        if (!range_select) {
            out.op(0);
            for (const u8 fd : fd_select) out.op(fd);
            return out;
        }
        std::vector<std::pair<u16, u8>> ranges;
        for (usize i = 0; i < fd_select.size(); ++i) {
            if (ranges.empty() || ranges.back().second != fd_select[i]) {
                ranges.emplace_back(u16(i), fd_select[i]);
            }
        }
        const auto push_u16 = [&](usize value) {
            out.op(u8((value >> 8) & 0xFFu));
            out.op(u8(value & 0xFFu));
        };
        out.op(3);
        push_u16(ranges.size());
        for (const std::pair<u16, u8>& range : ranges) {
            push_u16(range.first);
            out.op(range.second);
        }
        push_u16(fd_select.size());
        return out;
    }
};

} // namespace

// ---------------------------------------------------------------- the tests --

T2D_TEST(cff_parse_rejects_garbage) {
    std::string error;
    T2D_CHECK(CffFont::parse({}, &error) == nullptr);
    T2D_CHECK_FALSE(error.empty());

    const std::vector<u8> tiny{1, 0, 4};
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(tiny), &error) == nullptr);

    // A header that lies about its own size, and one with a major version this parser cannot know.
    const std::vector<u8> bad_header{2, 0, 4, 1, 0, 0};
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(bad_header), &error) == nullptr);
    const std::vector<u8> short_header{1, 0, 0, 1, 0, 0};
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(short_header), &error) == nullptr);
    std::vector<u8> huge_header{1, 0, 200, 1, 0, 0};
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(huge_header), &error) == nullptr);

    // A valid header with an empty Top DICT INDEX: there is no font in there.
    const std::vector<u8> no_top_dict{1, 0, 4, 1, 0, 0, 0, 0, 0, 0};
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(no_top_dict), &error) == nullptr);

    // A Top DICT with a Private DICT but no CharStrings entry.
    std::vector<u8> no_charstrings{1, 0, 4, 1};
    append(no_charstrings, make_index({{u8('T')}}));
    const std::vector<u8> private_entry = Code().fixed(2).fixed(60).op(18).take();
    append(no_charstrings, make_index({private_entry}));
    append(no_charstrings, make_index({}));
    append(no_charstrings, make_index({}));
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(no_charstrings), &error) == nullptr);
    T2D_CHECK_MSG(error.find("CharStrings") != std::string::npos, "rejected for the wrong reason: {}", error);

    // A CharStrings offset past the end of the table, and an INDEX whose offsets do.
    SynthCff one_glyph;
    one_glyph.glyphs = {{14}};
    const std::vector<u8> table = one_glyph.build();
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(table), &error) != nullptr);

    // The same table with the CharStrings operand - the first entry of the Top DICT - pointing
    // past the end of the table.
    std::vector<u8> bogus = table;
    const usize name_index_size = make_index({{u8('T'), u8('e'), u8('s'), u8('t')}}).size();
    const usize top_dict_data = 4 + name_index_size + 5; // count, offSize and two offsets
    T2D_CHECK_EQ(bogus[top_dict_data], 29u);
    bogus[top_dict_data + 1] = 0xFF;
    bogus[top_dict_data + 2] = 0xFF;
    bogus[top_dict_data + 3] = 0xFF;
    bogus[top_dict_data + 4] = 0xFF;
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(bogus), &error) == nullptr);

    // An INDEX whose last offset reaches past the end of the table.
    std::vector<u8> clipped = table;
    const usize charstrings = charstrings_index_of(clipped);
    T2D_REQUIRE(charstrings != 0);
    const u8 off_size = clipped[charstrings + 2];
    T2D_REQUIRE(off_size >= 1 && off_size <= 4);
    const usize last_offset = charstrings + 3 + usize(off_size); // the second of the two offsets
    for (u8 i = 0; i < off_size; ++i) clipped[last_offset + i] = 0xFF;
    T2D_CHECK(CffFont::parse(ConstSpan<const u8>(clipped), &error) == nullptr);
}

T2D_TEST(cff_parse_rejects_truncated_real_table) {
    const RealFont& sans = sans_font();
    T2D_REQUIRE(sans.available);
    std::string error;

    // Every prefix short enough to cut into the header or the INDEXes must be rejected outright.
    for (usize length = 0; length <= 64; ++length) {
        const ConstSpan<const u8> prefix(sans.table.data(), length);
        T2D_CHECK_MSG(CffFont::parse(prefix, &error) == nullptr, "a {} byte prefix was accepted", length);
    }
    // Longer prefixes must not read past their end either; the CharStrings INDEX of these fonts
    // reaches the end of the table, so all of them are incomplete.
    for (const usize length : {usize(128), usize(1024), usize(65536), sans.table.size() / 4, sans.table.size() / 2,
                               sans.table.size() - 1}) {
        const ConstSpan<const u8> prefix(sans.table.data(), length);
        T2D_CHECK_MSG(CffFont::parse(prefix, &error) == nullptr, "a {} byte prefix was accepted", length);
    }
}

T2D_TEST(cff_synthetic_plain_font_parses) {
    SynthCff synth;
    synth.default_width = 500;
    synth.nominal_width = 400;
    // Entries in front of the ones the parser reads: a 16 bit operand pair (BlueValues), a real
    // operand (BlueScale, nibble encoded as 30 0A 00 1F) and a 32 bit operand (defaultWidthX).
    synth.extra_private = Code().numbers({-2000, 2000}).op(6).raw({30, 0x0A, 0x00, 0x1F}).escaped(9).fixed(500).op(20).take();
    synth.glyphs = {
        Code().numbers({0, 0}).op(21).op(14).take(),  // rmoveto, endchar
        Code().numbers({10, 20}).op(5).op(14).take(), // rlineto, endchar
    };
    const std::vector<u8> table = synth.build();

    std::string error;
    const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(table), &error);
    T2D_REQUIRE(font != nullptr);
    T2D_CHECK_MSG(error.empty(), "parse reported {}", error);
    T2D_CHECK_EQ(font->glyph_count(), 2u);
    T2D_CHECK_FALSE(font->is_cid_keyed());

    GlyphPath path;
    T2D_CHECK(font->glyph_path(0, path));
    check_path(path, {m(0.0f, 0.0f), close_contour()}, "plain glyph 0");
    T2D_CHECK(font->glyph_path(1, path));
    check_path(path, {m(0.0f, 0.0f), l(10.0f, 20.0f), close_contour()}, "plain glyph 1");
    T2D_CHECK_NEAR(font->advance(0), 500.0f, 0.01f);

    // Out of range glyphs are refused, and an advance is still a number.
    T2D_CHECK_FALSE(font->glyph_path(2, path));
    T2D_CHECK_NEAR(font->advance(2), 0.0f, 0.01f);
}

T2D_TEST(cff_synthetic_width_rules) {
    SynthCff synth;
    synth.default_width = 500;
    synth.nominal_width = 400;
    synth.glyphs = {
        Code().numbers({0, 0}).op(21).op(14).take(),         // rmoveto, two operands: no width
        Code().numbers({150, 100, 200}).op(21).op(14).take(), // rmoveto, three: width, dx, dy
        Code().numbers({50}).op(22).op(14).take(),            // hmoveto, one: no width
        Code().numbers({70, 60}).op(22).op(14).take(),        // hmoveto, two: width, dx
        Code().numbers({30}).op(4).op(14).take(),             // vmoveto, one: no width
        Code().numbers({40, 25}).op(4).op(14).take(),         // vmoveto, two: width, dy
        Code().numbers({120, 10}).op(18).numbers({0, 0}).op(21).op(14).take(),      // hstemhm even: no width
        Code().numbers({130, 10, 20}).op(18).numbers({0, 0}).op(21).op(14).take(),  // hstemhm odd: width
        Code().numbers({90}).op(14).take(),                   // endchar with a width
        Code().op(14).take(),                                 // endchar alone: default width
    };
    const std::vector<u8> table = synth.build();
    std::string error;
    const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(table), &error);
    T2D_REQUIRE(font != nullptr);
    T2D_CHECK_EQ(font->glyph_count(), 10u);

    const f32 expected[10] = {500.0f, 550.0f, 500.0f, 470.0f, 500.0f, 440.0f, 500.0f, 530.0f, 490.0f, 500.0f};
    for (u32 glyph = 0; glyph < 10; ++glyph) {
        T2D_CHECK_MSG(std::fabs(font->advance(glyph) - expected[glyph]) < 0.01f, "glyph {}: advance {} instead of {}",
                      glyph, font->advance(glyph), expected[glyph]);
    }

    GlyphPath path;
    T2D_CHECK(font->glyph_path(1, path));
    check_path(path, {m(100.0f, 200.0f), close_contour()}, "rmoveto with a width");
    T2D_CHECK(font->glyph_path(3, path));
    check_path(path, {m(60.0f, 0.0f), close_contour()}, "hmoveto with a width");
    T2D_CHECK(font->glyph_path(5, path));
    check_path(path, {m(0.0f, 25.0f), close_contour()}, "vmoveto with a width");
    // A blank glyph is not a failure: endchar on its own leaves an empty path.
    T2D_CHECK(font->glyph_path(9, path));
    T2D_CHECK(path.empty());
    T2D_CHECK(font->glyph_path(8, path));
    T2D_CHECK(path.empty());
}

T2D_TEST(cff_synthetic_lines_and_hint_masks) {
    SynthCff synth;
    synth.glyphs = {
        // hlineto alternates horizontal first: +10 x, +20 y, +30 x.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30}).op(6).op(14).take(),
        // vlineto alternates vertical first.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30}).op(7).op(14).take(),
        // rlineto takes pairs.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40}).op(5).op(14).take(),
        // Nine stems need two mask bytes; if the interpreter skipped one, the moveto would be read
        // from the middle of the mask.
        Code()
            .numbers({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18})
            .op(18)
            .op(19)
            .op(0xAA)
            .op(0x55)
            .numbers({0, 0})
            .op(21)
            .numbers({5, 5})
            .op(5)
            .op(14)
            .take(),
        // hintmask carries implicit vstem hints: eight operands are four stems, one mask byte.
        Code().numbers({1, 2, 3, 4, 5, 6, 7, 8}).op(19).op(0x0F).numbers({0, 0}).op(21).numbers({7, 7}).op(5).op(14).take(),
        // Mixing the stem operators counts them all.
        Code().numbers({1, 2}).op(1).numbers({3, 4}).op(3).numbers({5, 6}).op(18).op(19).op(0xFF).numbers({0, 0}).op(21).op(14).take(),
    };
    const std::vector<u8> table = synth.build();
    std::string error;
    const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(table), &error);
    T2D_REQUIRE(font != nullptr);
    T2D_CHECK_EQ(font->glyph_count(), 6u);

    GlyphPath path;
    T2D_CHECK(font->glyph_path(0, path));
    check_path(path, {m(0.0f, 0.0f), l(10.0f, 0.0f), l(10.0f, 20.0f), l(40.0f, 20.0f), close_contour()}, "hlineto");
    T2D_CHECK(font->glyph_path(1, path));
    check_path(path, {m(0.0f, 0.0f), l(0.0f, 10.0f), l(20.0f, 10.0f), l(20.0f, 40.0f), close_contour()}, "vlineto");
    T2D_CHECK(font->glyph_path(2, path));
    check_path(path, {m(0.0f, 0.0f), l(10.0f, 20.0f), l(40.0f, 60.0f), close_contour()}, "rlineto");
    T2D_CHECK(font->glyph_path(3, path));
    check_path(path, {m(0.0f, 0.0f), l(5.0f, 5.0f), close_contour()}, "nine stems, two mask bytes");
    T2D_CHECK(font->glyph_path(4, path));
    check_path(path, {m(0.0f, 0.0f), l(7.0f, 7.0f), close_contour()}, "implicit vstems at hintmask");
    T2D_CHECK(font->glyph_path(5, path));
    check_path(path, {m(0.0f, 0.0f), close_contour()}, "mixed stem operators");
    T2D_CHECK_NEAR(font->advance(5), 500.0f, 0.01f);
}

T2D_TEST(cff_synthetic_curves) {
    SynthCff synth;
    synth.glyphs = {
        // rrcurveto: two control deltas and an end delta.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60}).op(8).op(14).take(),
        // hhcurveto without the optional first delta: dy1 is zero.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40}).op(27).op(14).take(),
        // hhcurveto with it: five operands, so the first is dy1.
        Code().numbers({0, 0}).op(21).numbers({5, 10, 20, 30, 40}).op(27).op(14).take(),
        // vvcurveto without and with the optional dx1.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40}).op(26).op(14).take(),
        Code().numbers({0, 0}).op(21).numbers({5, 10, 20, 30, 40}).op(26).op(14).take(),
        // hvcurveto: horizontal first, and the fifth operand is the end dx of the last curve.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40}).op(31).op(14).take(),
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50}).op(31).op(14).take(),
        // hvcurveto with nine operands: a horizontal curve, then a vertical one with the extra.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60, 70, 80, 90}).op(31).op(14).take(),
        // vhcurveto: vertical first, the extra lands on the vertical curve.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50}).op(30).op(14).take(),
        // rcurveline: curves while more than two operands are left, then a line.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60, 70, 80}).op(24).op(14).take(),
        // rlinecurve: lines until six operands are left, then a curve.
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60, 70, 80, 90, 100}).op(25).op(14).take(),
    };
    const std::vector<u8> table = synth.build();
    std::string error;
    const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(table), &error);
    T2D_REQUIRE(font != nullptr);

    GlyphPath path;
    T2D_CHECK(font->glyph_path(0, path));
    check_path(path, {m(0.0f, 0.0f), c(10.0f, 20.0f, 40.0f, 60.0f, 90.0f, 120.0f), close_contour()}, "rrcurveto");
    T2D_CHECK(font->glyph_path(1, path));
    check_path(path, {m(0.0f, 0.0f), c(10.0f, 0.0f, 30.0f, 30.0f, 70.0f, 30.0f), close_contour()}, "hhcurveto");
    T2D_CHECK(font->glyph_path(2, path));
    check_path(path, {m(0.0f, 0.0f), c(10.0f, 5.0f, 30.0f, 35.0f, 70.0f, 35.0f), close_contour()}, "hhcurveto dy1");
    T2D_CHECK(font->glyph_path(3, path));
    check_path(path, {m(0.0f, 0.0f), c(0.0f, 10.0f, 20.0f, 40.0f, 20.0f, 80.0f), close_contour()}, "vvcurveto");
    T2D_CHECK(font->glyph_path(4, path));
    check_path(path, {m(0.0f, 0.0f), c(5.0f, 10.0f, 25.0f, 40.0f, 25.0f, 80.0f), close_contour()}, "vvcurveto dx1");
    T2D_CHECK(font->glyph_path(5, path));
    check_path(path, {m(0.0f, 0.0f), c(10.0f, 0.0f, 30.0f, 30.0f, 30.0f, 70.0f), close_contour()}, "hvcurveto");
    T2D_CHECK(font->glyph_path(6, path));
    check_path(path, {m(0.0f, 0.0f), c(10.0f, 0.0f, 30.0f, 30.0f, 80.0f, 70.0f), close_contour()}, "hvcurveto 5");
    T2D_CHECK(font->glyph_path(7, path));
    check_path(path,
               {m(0.0f, 0.0f), c(10.0f, 0.0f, 30.0f, 30.0f, 30.0f, 70.0f), c(30.0f, 120.0f, 90.0f, 190.0f, 170.0f, 280.0f),
                close_contour()},
               "hvcurveto 9");
    T2D_CHECK(font->glyph_path(8, path));
    check_path(path, {m(0.0f, 0.0f), c(0.0f, 10.0f, 20.0f, 40.0f, 60.0f, 90.0f), close_contour()}, "vhcurveto 5");
    T2D_CHECK(font->glyph_path(9, path));
    check_path(path,
               {m(0.0f, 0.0f), c(10.0f, 20.0f, 40.0f, 60.0f, 90.0f, 120.0f), l(160.0f, 200.0f), close_contour()},
               "rcurveline");
    T2D_CHECK(font->glyph_path(10, path));
    check_path(path,
               {m(0.0f, 0.0f), l(10.0f, 20.0f), l(40.0f, 60.0f), c(90.0f, 120.0f, 160.0f, 200.0f, 250.0f, 300.0f),
                close_contour()},
               "rlinecurve");
}

T2D_TEST(cff_synthetic_flex_operators) {
    SynthCff synth;
    synth.glyphs = {
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60, 70}).escaped(34).op(14).take(),  // hflex
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60, 70, 80, 90}).escaped(33).op(14).take(), // hflex1
        Code()
            .numbers({0, 0})
            .op(21)
            .numbers({10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 50})
            .escaped(35)
            .op(14)
            .take(), // flex
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110}).escaped(36).op(14).take(), // flex1, |dy| wins
        Code().numbers({0, 0}).op(21).numbers({100, 10, 100, 10, 100, 10, 100, 10, 100, 10, 55}).escaped(36).op(14).take(), // flex1, |dx| wins
    };
    const std::vector<u8> table = synth.build();
    std::string error;
    const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(table), &error);
    T2D_REQUIRE(font != nullptr);

    GlyphPath path;
    T2D_CHECK(font->glyph_path(0, path));
    check_path(path,
               {m(0.0f, 0.0f), c(10.0f, 0.0f, 30.0f, 30.0f, 70.0f, 30.0f), c(120.0f, 30.0f, 180.0f, 0.0f, 250.0f, 0.0f),
                close_contour()},
               "hflex");
    T2D_CHECK(font->glyph_path(1, path));
    check_path(path,
               {m(0.0f, 0.0f), c(10.0f, 20.0f, 40.0f, 60.0f, 90.0f, 60.0f), c(150.0f, 60.0f, 220.0f, 140.0f, 310.0f, 0.0f),
                close_contour()},
               "hflex1");
    T2D_CHECK(font->glyph_path(2, path));
    check_path(path,
               {m(0.0f, 0.0f), c(10.0f, 20.0f, 40.0f, 60.0f, 90.0f, 120.0f), c(160.0f, 200.0f, 250.0f, 300.0f, 360.0f, 420.0f),
                close_contour()},
               "flex");
    T2D_CHECK(font->glyph_path(3, path));
    check_path(path,
               {m(0.0f, 0.0f), c(10.0f, 20.0f, 40.0f, 60.0f, 90.0f, 120.0f), c(160.0f, 200.0f, 250.0f, 300.0f, 0.0f, 410.0f),
                close_contour()},
               "flex1, dy wins");
    T2D_CHECK(font->glyph_path(4, path));
    check_path(path,
               {m(0.0f, 0.0f), c(100.0f, 10.0f, 200.0f, 20.0f, 300.0f, 30.0f), c(400.0f, 40.0f, 500.0f, 50.0f, 555.0f, 0.0f),
                close_contour()},
               "flex1, dx wins");
}

T2D_TEST(cff_synthetic_subroutines) {
    SynthCff synth;
    synth.local_subrs = {
        Code().numbers({10, 20}).op(5).op(11).take(),                    // ends with an explicit return
        Code().numbers({5, 5}).op(5).take(),                             // no return: the end of the subr is one
        Code().numbers({-107}).op(10).numbers({2, 3}).op(5).op(11).take(), // calls subr 0 (bias 107)
    };
    synth.global_subrs = {Code().numbers({1, 1}).op(5).op(11).take()};
    synth.glyphs = {
        Code().numbers({0, 0}).op(21).numbers({-107}).op(10).numbers({-106}).op(10).op(14).take(),
        Code().numbers({0, 0}).op(21).numbers({-107}).op(29).op(14).take(),
        Code().numbers({0, 0}).op(21).numbers({-105}).op(10).op(14).take(), // nested: subr 2 calls subr 0
        Code().numbers({0, 0}).op(21).numbers({-104}).op(10).op(14).take(),
        Code().numbers({0, 0}).op(21).numbers({-108}).op(10).op(14).take(), // one below the first subr
        Code().numbers({0, 0}).op(21).numbers({500}).op(10).op(14).take(),   // and one far past the last
    };
    // A return in the middle of a subr stops it.
    synth.local_subrs.push_back(Code().numbers({4, 4}).op(5).op(11).numbers({9, 9}).op(5).take());

    const std::vector<u8> table = synth.build();
    std::string error;
    const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(table), &error);
    T2D_REQUIRE(font != nullptr);

    GlyphPath path;
    T2D_CHECK(font->glyph_path(0, path));
    check_path(path, {m(0.0f, 0.0f), l(10.0f, 20.0f), l(15.0f, 25.0f), close_contour()}, "two local subrs");
    T2D_CHECK(font->glyph_path(1, path));
    check_path(path, {m(0.0f, 0.0f), l(1.0f, 1.0f), close_contour()}, "global subr");
    T2D_CHECK(font->glyph_path(2, path));
    check_path(path, {m(0.0f, 0.0f), l(10.0f, 20.0f), l(12.0f, 23.0f), close_contour()}, "nested subr");
    T2D_CHECK(font->glyph_path(3, path));
    check_path(path, {m(0.0f, 0.0f), l(4.0f, 4.0f), close_contour()}, "return in the middle");
    T2D_CHECK_FALSE(font->glyph_path(4, path)); // -108 + 107 is before the first subr
    T2D_CHECK_FALSE(font->glyph_path(5, path)); // 500 + 107 is past the last subr

    // The bias changes at 1240 subrs, and at exactly 1240 it is already 1131.
    SynthCff small_bias;
    small_bias.local_subrs.assign(1239, Code().op(11).take());
    small_bias.glyphs = {Code().numbers({0, 0}).op(21).numbers({-107}).op(10).op(14).take(),
                         Code().numbers({0, 0}).op(21).numbers({-1131}).op(10).op(14).take()};
    const std::shared_ptr<const CffFont> small = CffFont::parse(ConstSpan<const u8>(small_bias.build()), &error);
    T2D_REQUIRE(small != nullptr);
    T2D_CHECK(small->glyph_path(0, path));  // index 0 with bias 107
    T2D_CHECK_FALSE(small->glyph_path(1, path)); // -1131 + 107 is out of range

    SynthCff big_bias;
    big_bias.local_subrs.assign(1240, Code().op(11).take());
    big_bias.glyphs = {Code().numbers({0, 0}).op(21).numbers({-1131}).op(10).op(14).take(),
                       Code().numbers({0, 0}).op(21).numbers({109}).op(10).op(14).take()};
    const std::shared_ptr<const CffFont> big = CffFont::parse(ConstSpan<const u8>(big_bias.build()), &error);
    T2D_REQUIRE(big != nullptr);
    T2D_CHECK(big->glyph_path(0, path));       // index 0 with bias 1131
    T2D_CHECK_FALSE(big->glyph_path(1, path)); // 109 + 1131 is one past the last subr
}

T2D_TEST(cff_synthetic_cid_keyed_fonts) {
    for (int variant = 0; variant < 2; ++variant) {
        SynthCff synth;
        synth.cid = true;
        synth.range_select = (variant == 1);
        synth.fd_extra_private = {{}, {}};
        synth.fd_default_width = {1000, 500};
        synth.fd_nominal_width = {0, 100};
        synth.fd_subrs = {
            {Code().numbers({1, 1}).op(5).op(11).take()},
            {Code().numbers({2, 2}).op(5).op(11).take()},
        };
        synth.fd_select = variant == 0 ? std::vector<u8>{0, 1, 1, 0} : std::vector<u8>{0, 0, 1, 1};
        synth.glyphs = {
            Code().numbers({0, 0}).op(21).op(14).take(),
            Code().numbers({0, 0}).op(21).op(14).take(),
            Code().numbers({50, 0, 0}).op(21).numbers({-107}).op(10).op(14).take(),
            Code().numbers({0, 0}).op(21).numbers({-107}).op(10).op(14).take(),
        };
        std::string error;
        const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(synth.build()), &error);
        T2D_REQUIRE(font != nullptr);
        T2D_CHECK_MSG(font->is_cid_keyed(), "variant {}: not reported as CID keyed", variant);
        T2D_CHECK_EQ(font->glyph_count(), 4u);

        // The FDSelect decides which Private DICT - and so which widths and local subrs - applies.
        // Variant 0 maps the glyphs to the FDs as {0, 1, 1, 0}, variant 1 as {0, 0, 1, 1}.
        const bool glyph_one_in_fd_one = variant == 0;
        T2D_CHECK_NEAR(font->advance(0), 1000.0f, 0.01f);
        T2D_CHECK_NEAR(font->advance(1), glyph_one_in_fd_one ? 500.0f : 1000.0f, 0.01f);
        T2D_CHECK_NEAR(font->advance(2), 150.0f, 0.01f); // FD 1 in both variants: 100 + 50
        T2D_CHECK_NEAR(font->advance(3), glyph_one_in_fd_one ? 1000.0f : 500.0f, 0.01f);

        GlyphPath path;
        T2D_CHECK(font->glyph_path(2, path));
        check_path(path, {m(0.0f, 0.0f), l(2.0f, 2.0f), close_contour()},
                   variant == 0 ? "FD 1 local subr" : "FD 0 local subr");
        T2D_CHECK(font->glyph_path(3, path));
        if (variant == 0) {
            check_path(path, {m(0.0f, 0.0f), l(1.0f, 1.0f), close_contour()}, "FD 0 local subr");
        } else {
            check_path(path, {m(0.0f, 0.0f), l(2.0f, 2.0f), close_contour()}, "FD 1 local subr");
        }
    }

    // Without FDArray and FDSelect the Top DICT Private DICT covers every glyph, even with a ROS.
    SynthCff plain;
    plain.default_width = 700;
    plain.nominal_width = 0;
    plain.glyphs = {Code().numbers({0, 0}).op(21).op(14).take()};
    const std::shared_ptr<const CffFont> plain_font = CffFont::parse(ConstSpan<const u8>(plain.build()));
    T2D_REQUIRE(plain_font != nullptr);
    T2D_CHECK_FALSE(plain_font->is_cid_keyed());
    T2D_CHECK_NEAR(plain_font->advance(0), 700.0f, 0.01f);
}

T2D_TEST(cff_synthetic_broken_charstrings_are_reported) {
    SynthCff synth;
    synth.glyphs = {
        Code().numbers({0, 0}).op(21).op(0).take(),                     // reserved opcode 0
        Code().numbers({0, 0}).op(21).op(13).take(),                    // reserved opcode 13
        Code().numbers({0, 0}).op(21).op(15).take(),                    // reserved opcode 15
        Code().numbers({0, 0}).op(21).op(28).take(),                    // a 16 bit number with no bytes
        Code().numbers({0, 0}).op(21).op(255).take(),                   // a fixed point number with no bytes
        Code().numbers({10}).op(21).op(14).take(),                      // rmoveto with one operand
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30}).op(5).op(14).take(),  // rlineto with three
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40}).op(8).op(14).take(), // rrcurveto with four
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30}).op(27).op(14).take(), // hhcurveto with three
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30, 40, 50, 60, 70, 80, 90}).escaped(35).op(14).take(), // flex, 9 of 13
        Code().numbers({100, 100, 0, 0}).op(14).take(),                 // the deprecated seac endchar
        Code().op(12).take(),                                           // an escaped operator with no second byte
        Code().numbers({0, 0}).op(21).numbers({11}).op(10).op(14).take(), // callsubr with no subrs at all
        Code().numbers({0, 0}).op(21).numbers({10, 20, 30}).escaped(9).op(14).take(), // not an operator we know
        Code().numbers({0, 0, 0, 0, 0, 0, 0}).op(30).take(),            // 30 is not a charstring operator
        Code().numbers({0, 0}).op(21).op(14).take(),                    // the one good glyph
    };
    // A hundred operands overflow the stack long before the operator arrives.
    Code flood;
    for (i32 i = 0; i < 100; ++i) flood.compact(i);
    flood.numbers({0, 0}).op(21).op(14);
    synth.glyphs.push_back(flood.take());

    std::string error;
    const std::shared_ptr<const CffFont> font = CffFont::parse(ConstSpan<const u8>(synth.build()), &error);
    T2D_REQUIRE(font != nullptr);
    T2D_CHECK_EQ(font->glyph_count(), 17u);

    GlyphPath path;
    for (u32 glyph = 0; glyph < 15; ++glyph) {
        T2D_CHECK_MSG(!font->glyph_path(glyph, path), "glyph {} should not be interpretable", glyph);
    }
    T2D_CHECK(font->glyph_path(15, path));
    check_path(path, {m(0.0f, 0.0f), close_contour()}, "the good glyph still decodes");
    T2D_CHECK_FALSE(font->glyph_path(16, path)); // a hundred operands overflow the stack

    // A charstring that simply stops is accepted: the end of the code is the end of the glyph.
    SynthCff no_endchar;
    no_endchar.glyphs = {Code().numbers({0, 0}).op(21).numbers({10, 20}).op(5).take()};
    const std::shared_ptr<const CffFont> loose = CffFont::parse(ConstSpan<const u8>(no_endchar.build()), &error);
    T2D_REQUIRE(loose != nullptr);
    T2D_CHECK(loose->glyph_path(0, path));
    check_path(path, {m(0.0f, 0.0f), l(10.0f, 20.0f), close_contour()}, "no endchar");

    // A subroutine that calls itself runs into the depth limit instead of the stack.
    SynthCff recursive;
    recursive.local_subrs = {Code().numbers({-107}).op(10).op(11).take()};
    recursive.glyphs = {Code().numbers({0, 0}).op(21).numbers({-107}).op(10).op(14).take()};
    const std::shared_ptr<const CffFont> loop = CffFont::parse(ConstSpan<const u8>(recursive.build()), &error);
    T2D_REQUIRE(loop != nullptr);
    T2D_CHECK_FALSE(loop->glyph_path(0, path));
}

T2D_TEST(cff_real_fonts_are_cid_keyed) {
    const RealFont& sans = sans_font();
    T2D_REQUIRE(sans.available);
    T2D_CHECK(sans.font->is_cid_keyed());
    T2D_CHECK_GT(sans.font->glyph_count(), 10000u);

    const RealFont& serif = serif_font();
    T2D_REQUIRE(serif.available);
    T2D_CHECK(serif.font->is_cid_keyed());
    T2D_CHECK_GT(serif.font->glyph_count(), 10000u);
}

T2D_TEST(cff_real_every_glyph_of_noto_sans_cjk) {
    const RealFont& sans = sans_font();
    T2D_REQUIRE(sans.available);
    const Extraction all = extract(*sans.font, 1, kCoordinateLimit);

    T2D_CHECK_MSG(all.failures == 0, "{} of {} glyphs could not be interpreted (first: {})", all.failures, all.glyphs,
                  all.first_failure);
    T2D_CHECK_MSG(all.ill_formed == 0, "{} outlines are not well formed", all.ill_formed);
    T2D_CHECK_MSG(all.non_finite == 0, "{} outlines hold a coordinate that is not a number", all.non_finite);
    T2D_CHECK_MSG(all.out_of_bounds == 0, "{} outlines leave the {} unit limit", all.out_of_bounds, kCoordinateLimit);
    T2D_CHECK_MSG(all.advance_out_of_range == 0, "{} advances are outside 0..{}", all.advance_out_of_range,
                  kAdvanceLimit);
    T2D_CHECK_GT(all.non_empty, 60000u);
    T2D_CHECK_GT(all.contours, 100000u);
    T2D_CHECK_GT(all.curves, 100000u);
    T2D_CHECK_GT(all.lines, 100000u);
    T2D_CHECK_MSG(all.widest_outline > 500.0f, "the widest outline is only {} units", all.widest_outline);
    T2D_CHECK_EQ(all.glyphs, sans.font->glyph_count());
}

T2D_TEST(cff_real_serif_collection_extracts) {
    const RealFont& serif = serif_font();
    T2D_REQUIRE(serif.available);
    const Extraction all = extract(*serif.font, 1, kCoordinateLimit);
    T2D_CHECK_MSG(all.failures == 0, "{} of {} glyphs could not be interpreted (first: {})", all.failures, all.glyphs,
                  all.first_failure);
    T2D_CHECK_EQ(all.ill_formed, 0u);
    T2D_CHECK_EQ(all.non_finite, 0u);
    T2D_CHECK_EQ(all.out_of_bounds, 0u);
    T2D_CHECK_EQ(all.advance_out_of_range, 0u);
    T2D_CHECK_GT(all.curves, 100000u);
}

T2D_TEST(cff_real_advances_match_hmtx) {
    const RealFont& sans = sans_font();
    T2D_REQUIRE(sans.available);
    std::vector<u16> hmtx;
    T2D_REQUIRE(read_hmtx(ConstSpan<const u8>(sans.file), sans.directory, sans.font->glyph_count(), hmtx));

    // The width a charstring declares must be the width the font publishes. Noto CJK marks 754 of
    // its glyphs as zero width (hmtx says 0 as well), which is why this checks against hmtx instead
    // of demanding a positive advance from every glyph.
    u32 sampled = 0;
    u32 mismatches = 0;
    u32 zero = 0;
    u32 positive = 0;
    u32 out_of_range = 0;
    for (u32 glyph = 0; glyph < sans.font->glyph_count(); glyph += 7) {
        ++sampled;
        const f32 advance = sans.font->advance(glyph);
        if (std::fabs(advance - f32(hmtx[glyph])) > 0.01f) ++mismatches;
        if (advance > 0.0f) ++positive; else ++zero;
        if (!(advance >= 0.0f) || advance > kAdvanceLimit) ++out_of_range;
    }
    T2D_CHECK_EQ(mismatches, 0u);
    T2D_CHECK_EQ(out_of_range, 0u);
    T2D_CHECK_MSG(positive * 100 > sampled * 98, "only {} of {} sampled glyphs have a positive advance", positive,
                  sampled);
    T2D_CHECK_MSG(zero * 50 < sampled, "{} of {} sampled glyphs declare a zero advance", zero, sampled);
}

T2D_TEST(cff_real_blank_glyphs_are_empty_not_failures) {
    const RealFont& sans = sans_font();
    T2D_REQUIRE(sans.available);

    u32 blank = 0;
    u32 blank_with_advance = 0;
    u32 first_blank = kInvalidId;
    GlyphPath path;
    for (u32 glyph = 0; glyph < 4000 && glyph < sans.font->glyph_count(); ++glyph) {
        if (!sans.font->glyph_path(glyph, path)) continue; // covered by the stress test
        if (!path.empty()) continue;
        if (first_blank == kInvalidId) first_blank = glyph;
        ++blank;
        if (sans.font->advance(glyph) > 0.0f) ++blank_with_advance;
    }
    T2D_CHECK_MSG(blank > 0, "no blank glyph in the first 4000");
    T2D_CHECK_NE(first_blank, kInvalidId);
    T2D_CHECK_MSG(blank_with_advance > 0, "every blank glyph declares a zero advance");

    // The blank glyph stays blank and still reports success when asked again.
    T2D_CHECK(sans.font->glyph_path(first_blank, path));
    T2D_CHECK(path.empty());
    T2D_CHECK_FALSE(sans.font->glyph_path(sans.font->glyph_count(), path));
}

T2D_TEST(cff_real_ideographs_have_wide_outlines) {
    const RealFont& sans = sans_font();
    T2D_REQUIRE(sans.available);
    // Glyphs 1000 to 1500 are CJK ideographs in this collection: a real outline, wider than half an
    // em, with a sane advance.
    u32 wide = 0;
    u32 checked = 0;
    u32 positive = 0;
    GlyphPath path;
    for (u32 glyph = 1000; glyph < 1500; ++glyph) {
        T2D_REQUIRE(sans.font->glyph_path(glyph, path));
        ++checked;
        FontPoint min{};
        FontPoint max{};
        if (path.bounds(min, max) && max.x - min.x > 500.0f) ++wide;
        if (max.x - min.x > kCoordinateLimit) {
            T2D_CHECK_MSG(false, "glyph {} is wider than the limit", glyph);
        }
        if (sans.font->advance(glyph) > 0.0f) ++positive;
    }
    T2D_CHECK_EQ(checked, 500u);
    // The sweep holds ideographs as well as the narrow kana and punctuation forms that sit among
    // them, and four of the glyphs are zero width in the font itself (hmtx agrees, see the advance
    // test), so the counts are what matters, not a blanket demand on every glyph.
    T2D_CHECK_MSG(wide > 400u, "only {} of {} glyphs are wider than 500 units", wide, checked);
    T2D_CHECK_MSG(positive > 490u, "only {} of {} glyphs have a positive advance", positive, checked);
}

T2D_TEST_MAIN
