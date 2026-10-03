#include <mine/content_grid.h>

#include <algorithm>

namespace mine {

ContentGrid::ContentGrid(u32 width, u32 height, i32 layers) {
    width_ = std::clamp(width, 1u, kMaxDimension);
    height_ = std::clamp(height, 1u, kMaxDimension);
    layer_count_ = std::clamp(layers, 1, kMaxLayers);
    layers_.resize(static_cast<usize>(layer_count_));
    filled_.assign(static_cast<usize>(layer_count_), 0);
}

bool ContentGrid::reserve_layer(i32 layer) {
    if (layer < 0 || layer >= layer_count_) return false;
    std::vector<u32>& cells = layers_[static_cast<usize>(layer)];
    if (cells.empty()) cells.assign(cell_count(), 0u);
    return true;
}

ContentRef ContentGrid::at(i32 layer, GridPos pos) const {
    if (layer < 0 || layer >= layer_count_ || !inside(pos)) return ContentRef{};
    const std::vector<u32>& cells = layers_[static_cast<usize>(layer)];
    if (cells.empty()) return ContentRef{};
    return ContentRef::unpack(cells[index_of(pos)]);
}

void ContentGrid::set(i32 layer, GridPos pos, ContentRef value) {
    if (layer < 0 || layer >= layer_count_ || !inside(pos)) return;
    std::vector<u32>& cells = layers_[static_cast<usize>(layer)];
    if (cells.empty()) {
        if (value.empty()) return;   // nothing to write into a layer that has no storage
        if (!reserve_layer(layer)) return;
    }
    const usize index = index_of(pos);
    const bool was_filled = cells[index] != 0;
    const bool is_filled = !value.empty();
    if (was_filled != is_filled) {
        usize& count = filled_[static_cast<usize>(layer)];
        count = is_filled ? count + 1 : count - 1;
    }
    cells[index] = value.packed();
}

void ContentGrid::clear_layer(i32 layer) {
    if (layer < 0 || layer >= layer_count_) return;
    // Released, not just zeroed: the point of a lazy layer is that an empty one costs nothing.
    std::vector<u32>().swap(layers_[static_cast<usize>(layer)]);
    filled_[static_cast<usize>(layer)] = 0;
}

void ContentGrid::clear() {
    for (i32 layer = 0; layer < layer_count_; ++layer) clear_layer(layer);
}

void ContentGrid::resize(u32 width, u32 height) {
    const u32 new_width = std::clamp(width, 1u, kMaxDimension);
    const u32 new_height = std::clamp(height, 1u, kMaxDimension);
    if (new_width == width_ && new_height == height_) return;
    const u32 keep_x = std::min(new_width, width_);
    const u32 keep_y = std::min(new_height, height_);
    const usize new_count = static_cast<usize>(new_width) * new_height;
    for (i32 layer = 0; layer < layer_count_; ++layer) {
        std::vector<u32>& cells = layers_[static_cast<usize>(layer)];
        if (cells.empty()) continue;
        std::vector<u32> resized(new_count, 0u);
        usize count = 0;
        for (u32 y = 0; y < keep_y; ++y) {
            for (u32 x = 0; x < keep_x; ++x) {
                const u32 value = cells[static_cast<usize>(y) * width_ + x];
                if (value == 0) continue;
                resized[static_cast<usize>(y) * new_width + x] = value;
                ++count;
            }
        }
        cells = std::move(resized);
        filled_[static_cast<usize>(layer)] = count;
        // A layer whose cells all fell outside the new size is released, like a cleared one: the point
        // of a lazy layer is that an empty one costs nothing.
        if (count == 0) std::vector<u32>().swap(cells);
    }
    width_ = new_width;
    height_ = new_height;
}

usize ContentGrid::filled() const {
    usize total = 0;
    for (const usize count : filled_) total += count;
    return total;
}

usize ContentGrid::filled(i32 layer) const {
    if (layer < 0 || layer >= layer_count_) return 0;
    return filled_[static_cast<usize>(layer)];
}

bool ContentGrid::allocated(i32 layer) const {
    if (layer < 0 || layer >= layer_count_) return false;
    return !layers_[static_cast<usize>(layer)].empty();
}

usize ContentGrid::bytes() const {
    usize total = 0;
    for (const std::vector<u32>& cells : layers_) total += cells.size() * sizeof(u32);
    return total;
}

} // namespace mine