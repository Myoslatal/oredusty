#include <mine/sandbox.h>

#include <mine/content_loader.h>

#include <t2d/core/bitstream.h>
#include <t2d/core/cli.h>
#include <t2d/core/ecfg.h>
#include <t2d/core/rng.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace mine {
namespace {

/// "MSB2": the layout format grew tile layers, and a blob from before that is refused rather than
/// guessed at (it is a debug file, not a save).
constexpr u32 kLayoutMagic = 0x3242534Du;
constexpr u8 kLayoutVersion = 2;
/// A kind byte of 0xFF means "this cell is empty" - a value no ContentKind can take.
constexpr u8 kEmptyKindByte = 0xFFu;

/// The cell an out of range lookup returns. Empty, so a caller that forgot inside() draws nothing
/// instead of reading past the grid.
const SandboxCell kNoCell{};

[[nodiscard]] i32 floor_to_i32(f32 value) { return static_cast<i32>(std::floor(value)); }

/// FNV-1a: stable across runs and platforms, which is the whole point (a cell must not change colour
/// because the process restarted).
[[nodiscard]] u64 hash_name(std::string_view name) {
    u64 hash = 1469598103934665603ull;
    for (const char character : name) {
        hash ^= static_cast<u8>(character);
        hash *= 1099511628211ull;
    }
    return hash;
}

struct Rgb {
    u8 r = 0, g = 0, b = 0;
};

/// Hue from the hash, saturation and value from two of its bits: every name gets its own hue, and the
/// two extra bits keep neighbours on the palette apart without ever going near black or white.
[[nodiscard]] Rgb hsv_to_rgb(f32 hue_degrees, f32 saturation, f32 value) {
    const f32 c = value * saturation;
    const f32 h = hue_degrees / 60.0f;
    const f32 x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    f32 r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 1.0f) { r = c; g = x; }
    else if (h < 2.0f) { r = x; g = c; }
    else if (h < 3.0f) { g = c; b = x; }
    else if (h < 4.0f) { g = x; b = c; }
    else if (h < 5.0f) { r = x; b = c; }
    else { r = c; b = x; }
    const f32 m = value - c;
    const auto to_byte = [](f32 channel) {
        return static_cast<u8>(std::lround(std::clamp(channel, 0.0f, 1.0f) * 255.0f));
    };
    return Rgb{to_byte(r + m), to_byte(g + m), to_byte(b + m)};
}

} // namespace

bool is_placeable_kind(ContentKind kind) {
    // The kinds whose instances occupy a tile. Extending this is a one line change the moment the
    // designer says what else sits on the map (a submit channel, for instance).
    return kind == ContentKind::Structure || kind == ContentKind::Machine;
}

u32 debug_color_for(std::string_view name) {
    if (name.empty()) return 0xFF3C3C3Cu; // neutral grey, never pure black
    const u64 hash = hash_name(name);
    const f32 hue = static_cast<f32>(hash % 360u);
    const f32 saturation = 0.45f + 0.25f * static_cast<f32>((hash >> 9) & 1u);
    const f32 value = 0.70f + 0.25f * static_cast<f32>((hash >> 10) & 1u);
    const Rgb rgb = hsv_to_rgb(hue, saturation, value);
    // 0xAABBGGRR, the packing ore::make_rgba uses.
    return 0xFF000000u | (static_cast<u32>(rgb.b) << 16) | (static_cast<u32>(rgb.g) << 8) |
           static_cast<u32>(rgb.r);
}

SandboxModel::SandboxModel(u32 width, u32 height, i32 layers) {
    width_ = std::clamp(width, 1u, kMaxDimension);
    height_ = std::clamp(height, 1u, kMaxDimension);
    layers_ = std::clamp(layers, 1, kMaxLayers);
    active_layer_ = 0;
    cells_.assign(static_cast<usize>(width_) * height_ * static_cast<usize>(layers_), SandboxCell{});
}

bool SandboxModel::inside(GridPos pos) const {
    return pos.x >= 0 && pos.y >= 0 && static_cast<u32>(pos.x) < width_ && static_cast<u32>(pos.y) < height_;
}

const SandboxCell& SandboxModel::cell(GridPos pos) const {
    if (!inside(pos)) return kNoCell;
    // Topmost first: what the screen shows at this cell is what the inspector has to report.
    for (i32 layer = layers_ - 1; layer >= 0; --layer) {
        const SandboxCell& value = cells_[index(layer, pos)];
        if (!value.empty()) return value;
    }
    return cells_[index(active_layer_, pos)];
}

const SandboxCell& SandboxModel::cell(i32 layer, GridPos pos) const {
    if (!inside(pos) || layer < 0 || layer >= layers_) return kNoCell;
    return cells_[index(layer, pos)];
}

void SandboxModel::set_cell(GridPos pos, const SandboxCell& value) { set_cell(active_layer_, pos, value); }

void SandboxModel::set_cell(i32 layer, GridPos pos, const SandboxCell& value) {
    if (!inside(pos) || layer < 0 || layer >= layers_) return;
    cells_[index(layer, pos)] = value;
}

void SandboxModel::clear_cells() { cells_.assign(cells_.size(), SandboxCell{}); }

void SandboxModel::clear_layer(i32 layer) {
    if (layer < 0 || layer >= layers_) return;
    for (u32 y = 0; y < height_; ++y) {
        for (u32 x = 0; x < width_; ++x) {
            cells_[index(layer, GridPos{static_cast<i32>(x), static_cast<i32>(y)})] = SandboxCell{};
        }
    }
}

usize SandboxModel::filled_cells() const {
    usize filled = 0;
    for (const SandboxCell& value : cells_) {
        if (!value.empty()) ++filled;
    }
    return filled;
}

usize SandboxModel::filled_cells(i32 layer) const {
    if (layer < 0 || layer >= layers_) return 0;
    usize filled = 0;
    for (u32 y = 0; y < height_; ++y) {
        for (u32 x = 0; x < width_; ++x) {
            if (!cells_[index(layer, GridPos{static_cast<i32>(x), static_cast<i32>(y)})].empty()) ++filled;
        }
    }
    return filled;
}

void SandboxModel::set_active_layer(i32 layer) {
    if (layers_ <= 0) {
        active_layer_ = 0;
        return;
    }
    active_layer_ = ((layer % layers_) + layers_) % layers_;   // wrap: a key press walks the stack
}

void SandboxModel::cycle_layer(i32 delta) { set_active_layer(active_layer_ + delta); }

void SandboxModel::resize(u32 width, u32 height) {
    const u32 new_width = std::clamp(width, 1u, kMaxDimension);
    const u32 new_height = std::clamp(height, 1u, kMaxDimension);
    std::vector<SandboxCell> resized(static_cast<usize>(new_width) * new_height * static_cast<usize>(layers_),
                                     SandboxCell{});
    const u32 keep_x = std::min(new_width, width_);
    const u32 keep_y = std::min(new_height, height_);
    for (i32 layer = 0; layer < layers_; ++layer) {
        for (u32 y = 0; y < keep_y; ++y) {
            for (u32 x = 0; x < keep_x; ++x) {
                const GridPos pos{static_cast<i32>(x), static_cast<i32>(y)};
                resized[static_cast<usize>(layer) * new_width * new_height + static_cast<usize>(y) * new_width + x] =
                    cells_[index(layer, pos)];
            }
        }
    }
    width_ = new_width;
    height_ = new_height;
    cells_ = std::move(resized);
    set_cursor(cursor_);
}

usize SandboxModel::rebuild_palette(const ContentRegistry& registry) {
    // The selection survives a reload when it still points at the same content: re-registering a file
    // can move every id, and losing the brush on every reload would make the tool unusable.
    PaletteEntry previous;
    const bool had_selection = selected_ < palette_.size();
    if (had_selection) previous = palette_[selected_];

    palette_.clear();
    for (usize kind_index = 0; kind_index < kContentKindCount; ++kind_index) {
        const ContentKind kind = static_cast<ContentKind>(kind_index);
        if (!is_placeable_kind(kind)) continue;
        for (const ContentEntry& entry : registry.entries(kind)) {
            palette_.push_back(PaletteEntry{kind, entry.id, entry.name});
        }
    }

    selected_ = 0;
    if (had_selection) {
        for (usize index = 0; index < palette_.size(); ++index) {
            if (palette_[index].kind == previous.kind && palette_[index].name == previous.name) {
                selected_ = index;
                break;
            }
        }
    }
    return palette_.size();
}

const PaletteEntry& SandboxModel::palette(usize index) const {
    static const PaletteEntry kNone{};
    if (palette_.empty()) return kNone;
    return palette_[std::min(index, palette_.size() - 1)];
}

void SandboxModel::select_palette(usize index) {
    if (palette_.empty()) {
        selected_ = 0;
        return;
    }
    selected_ = std::min(index, palette_.size() - 1);
}

void SandboxModel::cycle_palette(i32 delta) {
    if (palette_.empty()) return;
    const i32 count = static_cast<i32>(palette_.size());
    i32 index = static_cast<i32>(selected_) + delta;
    index = ((index % count) + count) % count;
    selected_ = static_cast<usize>(index);
}

usize SandboxModel::palette_window_start(usize rows) const {
    if (rows == 0 || palette_.size() <= rows) return 0;
    if (selected_ < rows) return 0;
    return selected_ - rows + 1;
}

const PaletteEntry* SandboxModel::selected_entry() const {
    if (palette_.empty()) return nullptr;
    return &palette_[selected_];
}

void SandboxModel::set_cursor(GridPos pos) {
    cursor_.x = t2d::clamp_i32(pos.x, 0, static_cast<i32>(width_) - 1);
    cursor_.y = t2d::clamp_i32(pos.y, 0, static_cast<i32>(height_) - 1);
}

void SandboxModel::move_cursor(i32 dx, i32 dy) { set_cursor(GridPos{cursor_.x + dx, cursor_.y + dy}); }

bool SandboxModel::paint() {
    const PaletteEntry* entry = selected_entry();
    if (entry == nullptr) return false;
    const SandboxCell& existing = cell(active_layer_, cursor_);
    if (!existing.empty() && existing.kind == entry->kind && existing.id == entry->id &&
        existing.name == entry->name) {
        return false;
    }
    set_cell(active_layer_, cursor_, SandboxCell{entry->kind, entry->id, entry->name});
    return true;
}

bool SandboxModel::erase() {
    // Topmost first: what is visible at the cursor is what gets deleted, whichever layer holds it.
    for (i32 layer = layers_ - 1; layer >= 0; --layer) {
        if (cells_[index(layer, cursor_)].empty()) continue;
        cells_[index(layer, cursor_)] = SandboxCell{};
        return true;
    }
    return false;
}

void SandboxModel::set_cell_px(f32 pixels) { cell_px_ = std::clamp(pixels, 2.0f, 512.0f); }

void SandboxModel::zoom_at(Vec2 anchor, f32 factor) {
    const f32 before = cell_px_;
    set_cell_px(cell_px_ * factor);
    if (before == cell_px_) return;
    // The point under the cursor stays where it is: solve origin from anchor = origin + cells * cell_px.
    const Vec2 cells{(anchor.x - origin_.x) / before, (anchor.y - origin_.y) / before};
    origin_ = Vec2{anchor.x - cells.x * cell_px_, anchor.y - cells.y * cell_px_};
}

GridPos SandboxModel::cell_at_screen(Vec2 point) const {
    return GridPos{floor_to_i32((point.x - origin_.x) / cell_px_),
                   floor_to_i32((point.y - origin_.y) / cell_px_)};
}

Vec2 SandboxModel::screen_of_cell(GridPos cell_position) const {
    return Vec2{origin_.x + static_cast<f32>(cell_position.x) * cell_px_,
                origin_.y + static_cast<f32>(cell_position.y) * cell_px_};
}

void SandboxModel::center_view(Vec2 viewport) {
    origin_ = Vec2{(viewport.x - static_cast<f32>(width_) * cell_px_) * 0.5f,
                   (viewport.y - static_cast<f32>(height_) * cell_px_) * 0.5f};
}

void SandboxModel::scroll_to_show(GridPos cell_position, Vec2 viewport) {
    const f32 margin = cell_px_;
    const Vec2 top_left = screen_of_cell(cell_position);
    const Vec2 bottom_right{top_left.x + cell_px_, top_left.y + cell_px_};
    if (top_left.x < margin) origin_.x += margin - top_left.x;
    else if (bottom_right.x > viewport.x - margin) origin_.x -= bottom_right.x - (viewport.x - margin);
    if (top_left.y < margin) origin_.y += margin - top_left.y;
    else if (bottom_right.y > viewport.y - margin) origin_.y -= bottom_right.y - (viewport.y - margin);
}

void SandboxModel::fill_bands() {
    clear_layer(active_layer_);
    if (palette_.empty()) return;
    const usize count = palette_.size();
    for (u32 y = 0; y < height_; ++y) {
        const usize band = std::min(count - 1, static_cast<usize>(y) * count / height_);
        for (u32 x = 0; x < width_; ++x) {
            const PaletteEntry& entry = palette_[band];
            set_cell(GridPos{static_cast<i32>(x), static_cast<i32>(y)},
                     SandboxCell{entry.kind, entry.id, entry.name});
        }
    }
}

void SandboxModel::fill_scatter(u64 seed, u32 percent) {
    clear_layer(active_layer_);
    if (palette_.empty()) return;
    t2d::Rng rng(seed);
    const u32 chance = std::min(percent, 100u);
    for (u32 y = 0; y < height_; ++y) {
        for (u32 x = 0; x < width_; ++x) {
            if (rng.next_bounded(100) >= chance) continue;
            const PaletteEntry& entry = palette_[rng.next_bounded(static_cast<u32>(palette_.size()))];
            set_cell(GridPos{static_cast<i32>(x), static_cast<i32>(y)},
                     SandboxCell{entry.kind, entry.id, entry.name});
        }
    }
}

SandboxReloadReport SandboxModel::reload(ContentRegistry& registry, const std::vector<std::string>& paths) {
    std::vector<std::string> texts;
    texts.reserve(paths.size());
    SandboxReloadReport report;
    for (const std::string& path : paths) {
        const std::optional<std::string> text = t2d::read_text(path);
        if (!text.has_value()) {
            report.parsed = false;
            report.errors.push_back(std::format("{}: cannot be read", path));
            continue;
        }
        texts.push_back(*text);
    }
    if (!report.parsed) return report;
    report = reload_texts(registry, texts, paths);
    if (report.parsed) content_paths_ = paths;
    return report;
}

SandboxReloadReport SandboxModel::reload_texts(ContentRegistry& registry,
                                               const std::vector<std::string>& texts,
                                               const std::vector<std::string>& names) {
    SandboxReloadReport report;

    // Parse everything before touching anything: a half applied reload would leave the layer pointing
    // at a registry that no file describes.
    std::vector<t2d::EcfgDocument> documents;
    documents.reserve(texts.size());
    for (usize index = 0; index < texts.size(); ++index) {
        const std::string& name = index < names.size() ? names[index] : std::string("<text>");
        t2d::EcfgError error;
        std::optional<t2d::EcfgDocument> document = t2d::EcfgDocument::parse(texts[index], &error);
        if (!document.has_value()) {
            report.parsed = false;
            report.errors.push_back(error.describe(name));
            continue;
        }
        documents.push_back(std::move(*document));
    }
    if (!report.parsed) return report;

    const ContentTable before = registry.table();
    registry.clear();
    for (const t2d::EcfgDocument& document : documents) {
        const ContentLoadReport loaded = register_content_from_ecfg(registry, document);
        for (const std::string& table : loaded.unknown_tables) report.unknown_tables.push_back(table);
    }

    // What the files gained and lost, by name: an id that moved is not a change of content, and a
    // designer reading this line cares about content.
    for (const ContentEntry& entry : before.entries) {
        if (registry.find(entry.kind, entry.name) != kNoContent) ++report.kept;
        else ++report.removed;
    }
    const ContentTable after = registry.table();
    for (const ContentEntry& entry : after.entries) {
        if (before.find_id(entry.kind, entry.name) == kNoContent) ++report.added;
    }

    // Re-point every placed cell at its content by name. This is the step that makes reloading safe:
    // inserting an entry in the middle of a file shifts every id after it, and without this the layer
    // would quietly turn one structure into another.
    for (SandboxCell& value : cells_) {
        if (value.empty()) continue;
        const ContentId current = registry.find(value.kind, value.name);
        if (current == kNoContent) {
            value.id = kNoContent;
            ++report.lost_cells;
            continue;
        }
        if (current != value.id) ++report.remapped_cells;
        value.id = current;
    }

    report.palette_size = rebuild_palette(registry);
    return report;
}

std::vector<u8> SandboxModel::serialize(const ContentRegistry& registry) const {
    std::vector<u8> out;
    t2d::ByteWriter writer(out);
    writer.write_u32(kLayoutMagic);
    writer.write_u8(kLayoutVersion);
    writer.write_varint(width_);
    writer.write_varint(height_);
    writer.write_varint(static_cast<u32>(layers_));
    writer.write_varint(width_ * height_);
    // cells_ is layer major, so this writes every layer bottom to top in one pass.
    for (const SandboxCell& value : cells_) {
        if (value.empty()) {
            writer.write_u8(kEmptyKindByte);
            continue;
        }
        writer.write_u8(static_cast<u8>(value.kind));
        writer.write_varint(value.id);
    }
    const std::vector<u8> table = registry.table().serialize();
    writer.write_varint(static_cast<u32>(table.size()));
    writer.write_bytes(t2d::ConstSpan<const u8>(table.data(), table.size()));
    return out;
}

SandboxLoadReport SandboxModel::deserialize(t2d::ConstSpan<const u8> data, const ContentRegistry& registry) {
    SandboxLoadReport report;
    t2d::ByteReader reader(data);

    const u32 magic = reader.read_u32();
    const u8 version = reader.read_u8();
    if (!reader.ok() || magic != kLayoutMagic) {
        report.error = "not a sandbox layout (wrong magic)";
        return report;
    }
    if (version != kLayoutVersion) {
        report.error = std::format("layout version {} is not supported (expected {})", version, kLayoutVersion);
        return report;
    }
    const u32 width = reader.read_varint();
    const u32 height = reader.read_varint();
    const u32 layers = reader.read_varint();
    const u32 count = reader.read_varint();
    if (!reader.ok() || width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        report.error = "the map size is missing or out of range";
        return report;
    }
    if (layers == 0 || layers > static_cast<u32>(kMaxLayers)) {
        report.error = std::format("the layer count {} is out of range", layers);
        return report;
    }
    if (count != width * height) {
        report.error = std::format("the cell count {} does not match {}x{}", count, width, height);
        return report;
    }

    // Read everything into locals: a file that turns out to be corrupt half way through must leave the
    // map exactly as it was.
    const u64 total = static_cast<u64>(count) * layers;
    std::vector<SandboxCell> loaded(static_cast<usize>(total));
    usize unknown_kind = 0;
    for (u64 cell_index = 0; cell_index < total; ++cell_index) {
        const u32 layer = static_cast<u32>(cell_index / count);
        const u32 within = static_cast<u32>(cell_index % count);
        const u8 kind_byte = reader.read_u8();
        if (kind_byte == kEmptyKindByte) continue;
        if (!reader.ok() || kind_byte >= static_cast<u8>(ContentKind::Count)) {
            report.error = std::format("layer {} cell {} has an unknown kind {}", layer, within, kind_byte);
            return report;
        }
        const ContentId id = reader.read_varint();
        if (!reader.ok()) {
            report.error = std::format("layer {} cell {} is truncated", layer, within);
            return report;
        }
        SandboxCell value;
        value.kind = static_cast<ContentKind>(kind_byte);
        if (!is_placeable_kind(value.kind)) ++unknown_kind;
        value.id = id;
        loaded[static_cast<usize>(cell_index)] = value;
    }

    const u32 table_size = reader.read_varint();
    if (!reader.ok() || table_size > reader.remaining()) {
        report.error = "the name -> id table is missing";
        return report;
    }
    const t2d::ConstSpan<const u8> table_bytes = reader.read_span(table_size);
    ContentTable saved;
    if (!ContentTable::deserialize(table_bytes, saved)) {
        report.error = "the name -> id table is unreadable";
        return report;
    }
    if (!reader.empty()) {
        report.error = std::format("{} trailing bytes after the table", reader.remaining());
        return report;
    }

    // Translate the saved ids into the running registry's ids, by name (registry.h). Content that is
    // gone keeps its name and gets id 0: the screen can then say what used to be there.
    const ContentRemap remap = ContentRemap::build(saved, registry);
    usize translated = 0;
    usize missing = 0;
    for (SandboxCell& value : loaded) {
        if (value.empty()) continue;
        const ContentEntry* entry = saved.find(value.kind, value.id);
        if (entry != nullptr) value.name = entry->name;
        value.id = remap.to_current(value.kind, value.id);
        if (value.id == kNoContent) ++missing;
        else ++translated;
    }

    width_ = width;
    height_ = height;
    layers_ = static_cast<i32>(layers);
    active_layer_ = 0;
    cells_ = std::move(loaded);
    set_cursor(cursor_);
    // A loaded layer is ready to paint on: the palette comes from the registry that translated it.
    rebuild_palette(registry);

    report.ok = true;
    report.translated = translated;
    report.missing = missing;
    report.unknown_kind = unknown_kind;
    return report;
}

std::string SandboxModel::dump_text() const {
    const usize cells_per_layer = static_cast<usize>(width_) * height_;
    std::string text = std::format("sandbox map {}x{}, {} tile layer(s), {} of {} cells filled\n", width_,
                                   height_, layers_, filled_cells(), cells_per_layer * layers_);
    if (content_paths_.empty()) {
        text += "content: none\n";
    } else {
        text += "content:";
        for (const std::string& path : content_paths_) text += std::format(" {}", path);
        text += "\n";
    }
    text += std::format("palette: {} entries\n", palette_.size());
    for (usize index = 0; index < palette_.size(); ++index) {
        text += std::format("  [{}] {} #{} {}\n", index, content_kind_name(palette_[index].kind),
                            palette_[index].id, palette_[index].name);
    }
    for (i32 layer = 0; layer < layers_; ++layer) {
        for (u32 y = 0; y < height_; ++y) {
            for (u32 x = 0; x < width_; ++x) {
                const SandboxCell& value =
                    cells_[index(layer, GridPos{static_cast<i32>(x), static_cast<i32>(y)})];
                if (value.empty()) continue;
                text += std::format("  L{} {},{} {} #{} {}{}\n", layer, x, y, content_kind_name(value.kind),
                                    value.id, value.name, value.missing() ? " (missing)" : "");
            }
        }
    }
    return text;
}

} // namespace mine
