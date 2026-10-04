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

/// "MSB3": the layout format grew packed cells with a "gone" flag, and a blob from before that is
/// refused rather than guessed at (it is a debug file, not a save).
constexpr u32 kLayoutMagic = 0x3242534Du;
constexpr u8 kLayoutVersion = 3;
/// A kind byte of 0xFF means "this cell is empty" - a value no ContentKind can take.
constexpr u8 kEmptyKindByte = 0xFFu;
/// A cell whose content is gone is written with this bit set on its kind byte.
constexpr u8 kMissingKindBit = 0x80u;

/// The cell an out of range lookup returns. Empty, so a caller that forgot inside() draws nothing
/// instead of reading past the grid.
const CellView kNoCell{};

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

/// Adds \p entry to \p table unless the table already names that id or that name. A table that
/// repeats either one cannot be translated unambiguously, and ContentTable::deserialize refuses it.
void add_unique(ContentTable& table, const ContentEntry& entry) {
    if (entry.id == kNoContent || entry.name.empty()) return;
    if (table.find(entry.kind, entry.id) != nullptr) return;
    if (table.find_id(entry.kind, entry.name) != kNoContent) return;
    table.entries.push_back(entry);
}

} // namespace

bool is_placeable_kind(ContentKind kind) {
    // The kinds whose instances occupy a tile. Extending this is a one line change the moment the
    // designer says what else sits on the map (a submit channel, for instance).
    return kind == ContentKind::Structure || kind == ContentKind::Machine;
}

i32 draw_step_for(usize visible_cells, i32 layers, usize max_quads) {
    const usize layer_count = static_cast<usize>(std::max(layers, 1));
    if (max_quads == 0) return 1;
    const usize quads = visible_cells * layer_count;
    if (quads <= max_quads) return 1;
    const auto step = static_cast<i32>(std::ceil(std::sqrt(static_cast<t2d::f64>(quads) /
                                                          static_cast<t2d::f64>(max_quads))));
    return std::max(step, 1);
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

SandboxModel::SandboxModel(u32 width, u32 height, i32 layers)
    : cells_(std::clamp(width, 1u, kMaxDimension), std::clamp(height, 1u, kMaxDimension),
             std::clamp(layers, 1, kMaxLayers)) {
    // The zoom range of the camera, in pixels per cell: close enough to read a name on a cell, far
    // enough out that a 1024 cell map fits a small window.
    camera_.set_zoom_limits(kMinCellPx, kMaxCellPx);
    camera_.set_zoom(24.0f);
}

// ------------------------------------------------------------------- the map ---

CellView SandboxModel::cell(GridPos pos) const {
    if (!inside(pos)) return kNoCell;
    // Topmost first: what the screen shows at this cell is what the inspector has to report.
    for (i32 layer = layer_count() - 1; layer >= 0; --layer) {
        const ContentRef ref = cells_.at(layer, pos);
        if (!ref.empty()) return CellView{ref.kind, ref.id, ref.stale, name_of(ref)};
    }
    return kNoCell;
}

CellView SandboxModel::cell(i32 layer, GridPos pos) const {
    if (!inside(pos) || layer < 0 || layer >= layer_count()) return kNoCell;
    const ContentRef ref = cells_.at(layer, pos);
    return CellView{ref.kind, ref.id, ref.stale, name_of(ref)};
}

std::string_view SandboxModel::name_of(ContentRef ref) const {
    if (ref.empty()) return {};
    // A cell that is still there is named by the table in force; a cell whose content went missing by
    // the table it went missing from. The two are never mixed: the id of a missing cell may well have
    // been handed to something else since, and naming it after that would be a lie.
    const ContentTable& table = ref.stale ? retired_ : table_;
    if (const ContentEntry* entry = table.find(ref.kind, ref.id); entry != nullptr) return entry->name;
    return {};
}

void SandboxModel::set_cell(GridPos pos, ContentRef value) { set_cell(active_layer_, pos, value); }

void SandboxModel::set_cell(i32 layer, GridPos pos, ContentRef value) { cells_.set(layer, pos, value); }

void SandboxModel::clear_cells() { cells_.clear(); }

void SandboxModel::clear_layer(i32 layer) { cells_.clear_layer(layer); }

usize SandboxModel::filled_cells() const { return cells_.filled(); }

usize SandboxModel::filled_cells(i32 layer) const { return cells_.filled(layer); }

void SandboxModel::resize(u32 width, u32 height) {
    cells_.resize(width, height);
    set_cursor(cursor_);
}

void SandboxModel::set_active_layer(i32 layer) {
    const i32 layers = layer_count();
    if (layers <= 0) {
        active_layer_ = 0;
        return;
    }
    active_layer_ = ((layer % layers) + layers) % layers;   // wrap: a key press walks the stack
}

void SandboxModel::cycle_layer(i32 delta) { set_active_layer(active_layer_ + delta); }

// ------------------------------------------------------------------- palette ---

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

// ------------------------------------------------------------ cursor and brush ---

void SandboxModel::set_cursor(GridPos pos) {
    cursor_.x = t2d::clamp_i32(pos.x, 0, static_cast<i32>(width()) - 1);
    cursor_.y = t2d::clamp_i32(pos.y, 0, static_cast<i32>(height()) - 1);
}

void SandboxModel::move_cursor(i32 dx, i32 dy) { set_cursor(GridPos{cursor_.x + dx, cursor_.y + dy}); }

bool SandboxModel::paint() {
    const PaletteEntry* entry = selected_entry();
    if (entry == nullptr) return false;
    const ContentRef existing = cells_.at(active_layer_, cursor_);
    if (existing.kind == entry->kind && existing.id == entry->id && !existing.stale) return false;
    set_cell(active_layer_, cursor_, ContentRef{entry->kind, entry->id, false});
    return true;
}

bool SandboxModel::erase() {
    // Topmost first: what is visible at the cursor is what gets deleted, whichever layer holds it.
    for (i32 layer = layer_count() - 1; layer >= 0; --layer) {
        if (cells_.at(layer, cursor_).empty()) continue;
        cells_.set(layer, cursor_, ContentRef{});
        return true;
    }
    return false;
}

// -------------------------------------------------------------------- camera ---

void SandboxModel::set_cell_px(f32 pixels) { camera_.set_zoom(pixels); }

void SandboxModel::set_origin(Vec2 origin) {
    // Solved from screen_of(): the screen position of cell (0, 0) is what a caller wants to place.
    camera_.set_centre((camera_.viewport_centre() - origin) / camera_.zoom());
}

GridPos SandboxModel::cell_at_screen(Vec2 point) const {
    const Vec2 world = camera_.world_of(point);
    return GridPos{t2d::floor_to_i32(world.x), t2d::floor_to_i32(world.y)};
}

Vec2 SandboxModel::screen_of_cell(GridPos cell_position) const {
    return camera_.screen_of(Vec2{static_cast<f32>(cell_position.x), static_cast<f32>(cell_position.y)});
}

void SandboxModel::center_view(Vec2 viewport) {
    camera_.set_viewport(viewport);
    camera_.look_at(Vec2{static_cast<f32>(width()) * 0.5f, static_cast<f32>(height()) * 0.5f});
}

void SandboxModel::look_at(GridPos cell, f32 pixels_per_cell) {
    if (pixels_per_cell > 0.0f) set_cell_px(pixels_per_cell);
    // The middle of the cell, not its corner: the cell is what the caller wants to look at.
    camera_.look_at(Vec2{static_cast<f32>(cell.x) + 0.5f, static_cast<f32>(cell.y) + 0.5f});
}

// ---------------------------------------------------------- the playtest pointer ---

void SandboxModel::point_at(Vec2 screen) {
    const GridPos under = cell_at_screen(screen);
    // Off the map is not a cell: the pointer remembers nothing rather than clamping to the nearest
    // edge, or hovering the void beside a small map would select its last column.
    hovered_ = inside(under) ? std::optional<GridPos>{under} : std::nullopt;
}

void SandboxModel::point_at_cell(GridPos cell) { hovered_ = inside(cell) ? std::optional<GridPos>{cell} : std::nullopt; }

void SandboxModel::scroll_to_show(GridPos cell_position, Vec2 viewport) {
    camera_.set_viewport(viewport);
    const f32 x = static_cast<f32>(cell_position.x);
    const f32 y = static_cast<f32>(cell_position.y);
    // One cell of margin, so a cursor that is walked with the arrow keys does not end up glued to the
    // edge of the screen.
    camera_.scroll_to_show(t2d::Aabb2{Vec2{x, y}, Vec2{x + 1.0f, y + 1.0f}}, camera_.zoom());
}

// --------------------------------------------------------------------- fills ---

void SandboxModel::fill_bands() {
    clear_layer(active_layer_);
    if (palette_.empty()) return;
    const usize count = palette_.size();
    for (u32 y = 0; y < height(); ++y) {
        const usize band = std::min(count - 1, static_cast<usize>(y) * count / height());
        const PaletteEntry& entry = palette_[band];
        for (u32 x = 0; x < width(); ++x) {
            set_cell(GridPos{static_cast<i32>(x), static_cast<i32>(y)},
                     ContentRef{entry.kind, entry.id, false});
        }
    }
}

void SandboxModel::fill_scatter(u64 seed, u32 percent) {
    clear_layer(active_layer_);
    if (palette_.empty()) return;
    t2d::Rng rng(seed);
    const u32 chance = std::min(percent, 100u);
    for (u32 y = 0; y < height(); ++y) {
        for (u32 x = 0; x < width(); ++x) {
            if (rng.next_bounded(100) >= chance) continue;
            const PaletteEntry& entry = palette_[rng.next_bounded(static_cast<u32>(palette_.size()))];
            set_cell(GridPos{static_cast<i32>(x), static_cast<i32>(y)},
                     ContentRef{entry.kind, entry.id, false});
        }
    }
}

// ------------------------------------------------------- reloading by name ---

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

    // The remap is the same pass a caller that filled the registry from several sources runs later;
    // here it happens once, right after this file's content was registered.
    const SandboxReloadReport remapped = rebind(registry);
    report.remapped_cells = remapped.remapped_cells;
    report.lost_cells = remapped.lost_cells;
    report.palette_size = remapped.palette_size;
    return report;
}

void SandboxModel::remember_missing(ContentKind kind, ContentId id, std::string_view name) {
    add_unique(retired_, ContentEntry{kind, id, std::string(name)});
}

SandboxReloadReport SandboxModel::rebind(const ContentRegistry& registry) {
    SandboxReloadReport report;
    // Re-point every placed cell at its content by name. This is the step that makes reloading safe:
    // inserting an entry in the middle of a file shifts every id after it, and without this the map
    // would quietly turn one structure into another.
    const ContentRemap remap = ContentRemap::build(table_, registry);
    for (i32 layer = 0; layer < layer_count(); ++layer) {
        cells_.for_each(layer, [&](GridPos pos, ContentRef& ref) {
            (void)pos;
            if (ref.empty()) return;
            if (ref.stale) {
                // A missing cell is matched by the name it was placed under, not by its number: that
                // number may have been handed to something else since, and turning one structure into
                // another is the one thing this pass exists to prevent.
                const ContentEntry* retired = retired_.find(ref.kind, ref.id);
                const ContentId current =
                    retired != nullptr ? registry.find(ref.kind, retired->name) : kNoContent;
                if (current == kNoContent) return;   // still gone, and still reported as missing
                ref.id = current;
                ref.stale = false;
                ++report.remapped_cells;
                return;
            }
            const ContentId current = remap.to_current(ref.kind, ref.id);
            if (current == kNoContent) {
                // The name comes from the table the cell was placed under, so it survives the registry
                // that dropped it.
                remember_missing(ref.kind, ref.id, name_of(ref));
                ref.stale = true;
                ++report.lost_cells;
                return;
            }
            if (current != ref.id) ++report.remapped_cells;
            ref.id = current;
        });
    }
    table_ = registry.table();
    report.palette_size = rebuild_palette(registry);
    return report;
}

// ------------------------------------------------------------- save and load ---

ContentTable SandboxModel::save_table() const {
    // The table in force names everything that is still there; the retired names are added on top so a
    // saved layout can still say what a lost cell used to be.
    ContentTable table = table_;
    for (const ContentEntry& entry : retired_.entries) add_unique(table, entry);
    return table;
}

std::vector<u8> SandboxModel::serialize() const {
    std::vector<u8> out;
    t2d::ByteWriter writer(out);
    writer.write_u32(kLayoutMagic);
    writer.write_u8(kLayoutVersion);
    writer.write_varint(width());
    writer.write_varint(height());
    writer.write_varint(static_cast<u32>(layer_count()));
    writer.write_varint(width() * height());
    // Layer major, row major inside a layer: the same order the grid stores.
    for (i32 layer = 0; layer < layer_count(); ++layer) {
        cells_.for_each(layer, [&](GridPos pos, const ContentRef& ref) {
            (void)pos;
            if (ref.empty()) {
                writer.write_u8(kEmptyKindByte);
                return;
            }
            writer.write_u8(static_cast<u8>(ref.kind) | (ref.stale ? kMissingKindBit : 0u));
            writer.write_varint(ref.id);
        });
    }
    const std::vector<u8> table = save_table().serialize();
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
    ContentGrid loaded(width, height, static_cast<i32>(layers));
    usize unknown_kind = 0;
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 within = 0; within < count; ++within) {
            const u8 kind_byte = reader.read_u8();
            if (kind_byte == kEmptyKindByte) continue;
            const u8 kind_value = kind_byte & ~kMissingKindBit;
            if (!reader.ok() || kind_value >= static_cast<u8>(ContentKind::Count)) {
                report.error = std::format("layer {} cell {} has an unknown kind {}", layer, within, kind_value);
                return report;
            }
            const ContentId id = reader.read_varint();
            if (!reader.ok()) {
                report.error = std::format("layer {} cell {} is truncated", layer, within);
                return report;
            }
            const ContentKind kind = static_cast<ContentKind>(kind_value);
            if (!is_placeable_kind(kind)) ++unknown_kind;
            loaded.set(static_cast<i32>(layer),
                       GridPos{static_cast<i32>(within % width), static_cast<i32>(within / width)},
                       ContentRef{kind, id, (kind_byte & kMissingKindBit) != 0});
        }
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
    // gone keeps its id and is marked missing, so the screen can still say what used to be there.
    const ContentRemap remap = ContentRemap::build(saved, registry);
    ContentTable retired;
    usize translated = 0;
    usize missing = 0;
    for (i32 layer = 0; layer < loaded.layer_count(); ++layer) {
        loaded.for_each(layer, [&](GridPos pos, ContentRef& ref) {
            (void)pos;
            if (ref.empty()) return;
            const ContentId current = remap.to_current(ref.kind, ref.id);
            if (current != kNoContent) {
                ref.id = current;
                ref.stale = false;
                ++translated;
                return;
            }
            ref.stale = true;
            ++missing;
            if (const ContentEntry* entry = saved.find(ref.kind, ref.id); entry != nullptr) {
                add_unique(retired, *entry);
            }
        });
    }

    cells_ = std::move(loaded);
    active_layer_ = 0;
    set_cursor(cursor_);
    // The cells now hold the running registry's ids, so its table is the one in force; the names of
    // what the save lost are kept beside it.
    table_ = registry.table();
    retired_ = std::move(retired);
    rebuild_palette(registry);

    report.ok = true;
    report.translated = translated;
    report.missing = missing;
    report.unknown_kind = unknown_kind;
    return report;
}

std::string SandboxModel::dump_text() const {
    const usize cells_per_layer = static_cast<usize>(width()) * height();
    std::string text = std::format("sandbox map {}x{}, {} tile layer(s), {} of {} cells filled, {} KiB\n",
                                   width(), height(), layer_count(), filled_cells(),
                                   cells_per_layer * static_cast<usize>(layer_count()),
                                   cell_bytes() / 1024);
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
    for (i32 layer = 0; layer < layer_count(); ++layer) {
        cells_.for_each(layer, [&](GridPos pos, const ContentRef& ref) {
            if (ref.empty()) return;
            const CellView view{ref.kind, ref.id, ref.stale, name_of(ref)};
            text += std::format("  L{} {},{} {} #{} {}{}\n", layer, pos.x, pos.y, content_kind_name(ref.kind),
                                view.shown_id(), view.name, view.missing() ? " (missing)" : "");
        });
    }
    return text;
}

} // namespace mine