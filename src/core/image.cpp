#include <ore/core/image.h>

#include <ore/core/file.h>
#include <ore/core/log.h>

#include <algorithm>
#include <array>
#include <cstring>

#if ORE_ENABLE_ZLIB
#include <zlib.h>
#endif

namespace ore {
namespace {

constexpr std::array<u8, 8> kPngSignature = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

std::array<u32, 256> make_crc_table() {
    std::array<u32, 256> table{};
    for (u32 n = 0; n < 256; ++n) {
        u32 c = n;
        for (u32 k = 0; k < 8; ++k) {
            c = (c & 1u) != 0u ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[n] = c;
    }
    return table;
}

const std::array<u32, 256>& crc_table() {
    static const std::array<u32, 256> table = make_crc_table();
    return table;
}

u32 crc32_bytes(u32 seed, ConstSpan<u8> data) {
    const auto& table = crc_table();
    u32 c = seed ^ 0xFFFFFFFFu;
    for (u8 byte : data) c = table[(c ^ byte) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

u32 adler32_bytes(ConstSpan<u8> data) {
    u32 a = 1, b = 0;
    for (u8 byte : data) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void push_u32_be(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>((value >> 24) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 16) & 0xFFu));
    out.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    out.push_back(static_cast<u8>(value & 0xFFu));
}

[[nodiscard]] u32 read_u32_be(const u8* data) {
    return (static_cast<u32>(data[0]) << 24) | (static_cast<u32>(data[1]) << 16) |
           (static_cast<u32>(data[2]) << 8) | static_cast<u32>(data[3]);
}

void write_chunk(std::vector<u8>& out, const char type[5], ConstSpan<u8> payload) {
    push_u32_be(out, static_cast<u32>(payload.size()));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    const u32 crc = crc32_bytes(0, ConstSpan<u8>(out.data() + start, out.size() - start));
    push_u32_be(out, crc);
}

/// zlib stream: real deflate when zlib is available, stored blocks otherwise.
[[nodiscard]] std::vector<u8> zlib_compress(ConstSpan<u8> raw) {
    std::vector<u8> out;
#if ORE_ENABLE_ZLIB
    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    out.resize(bound);
    if (compress2(out.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
        ORE_WARN("zlib compression failed, writing stored deflate blocks");
        out.clear();
    } else {
        out.resize(bound);
        return out;
    }
#endif
    // Stored (BTYPE=00) deflate blocks, max 65535 payload bytes each.
    out.push_back(0x78); // zlib header: CM=8, CINFO=7
    out.push_back(0x01); // FCHECK/FLEVEL/FDICT, no preset dictionary
    usize offset = 0;
    constexpr usize kMaxBlock = 65535;
    while (offset < raw.size()) {
        const usize chunk = std::min(kMaxBlock, raw.size() - offset);
        const bool last = offset + chunk >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<u8>(chunk & 0xFFu));
        out.push_back(static_cast<u8>((chunk >> 8) & 0xFFu));
        out.push_back(static_cast<u8>(~chunk & 0xFFu));
        out.push_back(static_cast<u8>((~chunk >> 8) & 0xFFu));
        out.insert(out.end(), raw.begin() + static_cast<isize>(offset), raw.begin() + static_cast<isize>(offset + chunk));
        offset += chunk;
    }
    if (raw.empty()) {
        out.push_back(1);
        out.insert(out.end(), {0, 0, 0xFF, 0xFF});
    }
    push_u32_be(out, adler32_bytes(raw));
    return out;
}

struct PngHeader {
    u32 width = 0;
    u32 height = 0;
    u8 bit_depth = 0;
    u8 color_type = 0;
    u8 interlace = 0;
};

[[maybe_unused, nodiscard]] u8 paeth(u8 a, u8 b, u8 c) {
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

} // namespace

Image Image::create(u32 width, u32 height, u32 rgba_value) {
    Image image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<usize>(width) * height * 4u);
    image.fill(rgba_value);
    return image;
}

void Image::fill(u32 rgba_value) {
    u32* words = reinterpret_cast<u32*>(pixels.data());
    const usize count = pixels.size() / 4u;
    std::fill(words, words + static_cast<isize>(count), rgba_value);
}

void Image::fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgba_value) {
    for (u32 row_index = y; row_index < std::min(y + h, height); ++row_index) {
        u32* line = row(row_index);
        for (u32 col = x; col < std::min(x + w, width); ++col) line[col] = rgba_value;
    }
}

void Image::flip_vertical() {
    for (u32 y = 0; y < height / 2; ++y) {
        std::swap_ranges(row(y), row(y) + width, row(height - 1 - y));
    }
}

std::optional<std::vector<u8>> Image::encode_png() const {
    if (empty() || pixels.size() != byte_size()) {
        ORE_ERROR("encode_png: invalid image ({}x{})", width, height);
        return std::nullopt;
    }

    // Raw scanlines with filter byte 0 (None) in front of every row.
    std::vector<u8> raw;
    raw.reserve(static_cast<usize>(height) * (static_cast<usize>(width) * 4u + 1u));
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        const u8* src = pixels.data() + static_cast<usize>(y) * width * 4u;
        raw.insert(raw.end(), src, src + static_cast<usize>(width) * 4u);
    }

    std::vector<u8> out(kPngSignature.begin(), kPngSignature.end());

    std::vector<u8> ihdr;
    push_u32_be(ihdr, width);
    push_u32_be(ihdr, height);
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // colour type: truecolour with alpha
    ihdr.push_back(0); // compression
    ihdr.push_back(0); // filter method
    ihdr.push_back(0); // interlace
    write_chunk(out, "IHDR", ihdr);

    const std::vector<u8> compressed = zlib_compress(raw);
    write_chunk(out, "IDAT", compressed);
    write_chunk(out, "IEND", {});
    return out;
}

bool Image::save_png(std::string_view path) const {
    const auto encoded = encode_png();
    if (!encoded) return false;
    return write_binary_file(path, *encoded);
}

std::optional<Image> Image::decode_png(ConstSpan<u8> bytes) {
    if (bytes.size() < kPngSignature.size() ||
        !std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin())) {
        ORE_ERROR("decode_png: not a PNG file");
        return std::nullopt;
    }

    PngHeader header;
    std::vector<u8> idat;
    std::vector<u8> palette;
    std::vector<u8> palette_alpha;
    bool seen_header = false;

    usize offset = kPngSignature.size();
    while (offset + 12 <= bytes.size()) {
        const u32 length = read_u32_be(bytes.data() + offset);
        const char* type = reinterpret_cast<const char*>(bytes.data() + offset + 4);
        if (offset + 12u + length > bytes.size()) {
            ORE_ERROR("decode_png: truncated chunk '{}'", std::string_view(type, 4));
            return std::nullopt;
        }
        const u8* payload = bytes.data() + offset + 8;
        const std::string_view kind(type, 4);
        if (kind == "IHDR") {
            if (length < 13) return std::nullopt;
            header.width = read_u32_be(payload);
            header.height = read_u32_be(payload + 4);
            header.bit_depth = payload[8];
            header.color_type = payload[9];
            header.interlace = payload[12];
            seen_header = true;
        } else if (kind == "PLTE") {
            palette.assign(payload, payload + length);
        } else if (kind == "tRNS") {
            palette_alpha.assign(payload, payload + length);
        } else if (kind == "IDAT") {
            idat.insert(idat.end(), payload, payload + length);
        } else if (kind == "IEND") {
            break;
        }
        offset += 12u + length;
    }

    if (!seen_header || header.width == 0 || header.height == 0) {
        ORE_ERROR("decode_png: missing or invalid IHDR");
        return std::nullopt;
    }
    if (header.bit_depth != 8 || header.interlace != 0) {
        ORE_ERROR("decode_png: only 8-bit non-interlaced PNGs are supported (depth={}, interlace={})",
                  header.bit_depth, header.interlace);
        return std::nullopt;
    }
    if (idat.empty()) {
        ORE_ERROR("decode_png: no IDAT payload");
        return std::nullopt;
    }

    [[maybe_unused]] u32 channels = 0;
    switch (header.color_type) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default:
            ORE_ERROR("decode_png: unsupported colour type {}", header.color_type);
            return std::nullopt;
    }
    if (header.color_type == 3 && palette.empty()) {
        ORE_ERROR("decode_png: palette image without PLTE chunk");
        return std::nullopt;
    }

#if !ORE_ENABLE_ZLIB
    ORE_ERROR("decode_png: this build has no zlib support");
    return std::nullopt;
#else
    const usize stride = static_cast<usize>(header.width) * channels;
    std::vector<u8> raw(static_cast<usize>(header.height) * (stride + 1u));
    uLongf raw_size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &raw_size, idat.data(), static_cast<uLong>(idat.size())) != Z_OK ||
        raw_size != raw.size()) {
        ORE_ERROR("decode_png: inflate failed");
        return std::nullopt;
    }

    // Undo the per-scanline filters.
    std::vector<u8> pixels(stride * header.height);
    for (u32 y = 0; y < header.height; ++y) {
        const u8 filter = raw[static_cast<usize>(y) * (stride + 1u)];
        const u8* src = raw.data() + static_cast<usize>(y) * (stride + 1u) + 1u;
        u8* dst = pixels.data() + static_cast<usize>(y) * stride;
        const u8* prior = y > 0 ? pixels.data() + static_cast<usize>(y - 1) * stride : nullptr;
        for (usize x = 0; x < stride; ++x) {
            const u8 left = x >= channels ? dst[x - channels] : 0;
            const u8 up = prior != nullptr ? prior[x] : 0;
            const u8 up_left = (prior != nullptr && x >= channels) ? prior[x - channels] : 0;
            switch (filter) {
                case 0: dst[x] = src[x]; break;
                case 1: dst[x] = static_cast<u8>(src[x] + left); break;
                case 2: dst[x] = static_cast<u8>(src[x] + up); break;
                case 3: dst[x] = static_cast<u8>(src[x] + ((static_cast<u32>(left) + up) / 2u)); break;
                case 4: dst[x] = static_cast<u8>(src[x] + paeth(left, up, up_left)); break;
                default:
                    ORE_ERROR("decode_png: unknown filter type {}", filter);
                    return std::nullopt;
            }
        }
    }

    Image image = Image::create(header.width, header.height);
    for (u32 y = 0; y < header.height; ++y) {
        const u8* src = pixels.data() + static_cast<usize>(y) * stride;
        u32* dst = image.row(y);
        for (u32 x = 0; x < header.width; ++x) {
            u8 r = 0, g = 0, b = 0, a = 255;
            switch (header.color_type) {
                case 0:
                    r = g = b = src[x];
                    break;
                case 2:
                    r = src[x * 3 + 0];
                    g = src[x * 3 + 1];
                    b = src[x * 3 + 2];
                    break;
                case 3: {
                    const u8 index = src[x];
                    if ((static_cast<usize>(index) * 3u + 2u) < palette.size()) {
                        r = palette[index * 3 + 0];
                        g = palette[index * 3 + 1];
                        b = palette[index * 3 + 2];
                    }
                    if (index < palette_alpha.size()) a = palette_alpha[index];
                    break;
                }
                case 4:
                    r = g = b = src[x * 2 + 0];
                    a = src[x * 2 + 1];
                    break;
                case 6:
                    r = src[x * 4 + 0];
                    g = src[x * 4 + 1];
                    b = src[x * 4 + 2];
                    a = src[x * 4 + 3];
                    break;
                default:
                    break;
            }
            dst[x] = make_rgba(r, g, b, a);
        }
    }
    return image;
#endif
}

std::optional<Image> Image::load_png(std::string_view path) {
    const auto bytes = read_binary_file(path);
    if (!bytes) {
        ORE_ERROR("load_png: cannot read '{}'", path);
        return std::nullopt;
    }
    return decode_png(*bytes);
}

Image make_checkerboard(u32 size, u32 cells, u32 color_a, u32 color_b) {
    Image image = Image::create(size, size);
    if (cells == 0) cells = 1;
    const u32 cell_size = std::max(1u, size / cells);
    for (u32 y = 0; y < size; ++y) {
        u32* line = image.row(y);
        for (u32 x = 0; x < size; ++x) {
            const bool even = ((x / cell_size) + (y / cell_size)) % 2u == 0u;
            line[x] = even ? color_a : color_b;
        }
    }
    return image;
}

Image make_color_gradient(u32 width, u32 height, u32 top_left, u32 bottom_right) {
    Image image = Image::create(width, height);
    const Color a = Color::from_packed(top_left);
    const Color b = Color::from_packed(bottom_right);
    for (u32 y = 0; y < height; ++y) {
        u32* line = image.row(y);
        for (u32 x = 0; x < width; ++x) {
            const f32 tx = width > 1 ? static_cast<f32>(x) / static_cast<f32>(width - 1) : 0.0f;
            const f32 ty = height > 1 ? static_cast<f32>(y) / static_cast<f32>(height - 1) : 0.0f;
            const f32 t = std::clamp((tx + ty) * 0.5f, 0.0f, 1.0f);
            const auto mix = [t](u8 lo, u8 hi) {
                return static_cast<u8>(static_cast<f32>(lo) + (static_cast<f32>(hi) - static_cast<f32>(lo)) * t);
            };
            line[x] = make_rgba(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), 255);
        }
    }
    return image;
}

} // namespace ore
