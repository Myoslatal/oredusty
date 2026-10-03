#include <ore/renderer/upload_ring.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

#include <algorithm>
#include <format>

namespace ore {

Scope<UploadRing> UploadRing::create(rhi::GpuAllocator& allocator, u64 segment_size, u32 segment_count,
                                    std::string_view debug_name) {
    if (segment_size == 0 || segment_count == 0) {
        ORE_ERROR("UploadRing::create: segment_size and segment_count must be non zero");
        return nullptr;
    }

    Scope<UploadRing> ring(new UploadRing());
    ring->segment_size_ = align_up(segment_size, 256);
    ring->segments_.resize(segment_count);
    ring->alignment_ = std::max<u64>(allocator.device().properties().limits.minUniformBufferOffsetAlignment, 16);

    rhi::BufferDesc desc;
    desc.size = ring->segment_size_ * segment_count;
    desc.usage = rhi::BufferUsage::Uniform | rhi::BufferUsage::Storage | rhi::BufferUsage::Vertex |
                 rhi::BufferUsage::Index;
    desc.host_visible = true;
    desc.debug_name = std::string(debug_name);
    ring->buffer_ = rhi::Buffer::create(allocator, desc);
    if (ring->buffer_ == nullptr) return nullptr;

    ORE_DEBUG("upload ring: {} segments of {} KiB ({} total)", segment_count, ring->segment_size_ / 1024,
              desc.size / 1024);
    return ring;
}

void UploadRing::begin_frame(u32 frame_slot) {
    current_ = frame_slot % static_cast<u32>(segments_.size());
    Segment& segment = segments_[current_];
    stats_.peak_usage = std::max(stats_.peak_usage, segment.cursor);
    stats_.allocations_this_frame = segment.allocations;
    segment.cursor = 0;
    segment.allocations = 0;
}

UploadRing::Slice UploadRing::allocate(u64 size, u64 alignment) {
    const u64 align = std::max(alignment == 0 ? alignment_ : alignment, static_cast<u64>(1));
    Segment& segment = segments_[current_];
    const u64 offset_in_segment = align_up(segment.cursor, align);
    if (offset_in_segment + size > segment_size_) {
        ++stats_.exhausted_frames;
        ORE_ERROR("UploadRing: segment exhausted ({} + {} > {}); increase the segment size", offset_in_segment, size,
                  segment_size_);
        return {};
    }
    segment.cursor = offset_in_segment + size;
    segment.peak = std::max(segment.peak, segment.cursor);
    ++segment.allocations;
    stats_.allocations_this_frame = segment.allocations;
    stats_.peak_usage = std::max(stats_.peak_usage, segment.cursor);

    Slice slice;
    slice.offset = static_cast<u64>(current_) * segment_size_ + offset_in_segment;
    slice.size = size;
    slice.mapped = static_cast<u8*>(buffer_->mapped_data()) + slice.offset;
    return slice;
}

std::string UploadRing::dump_stats() const {
    std::string out = std::format("upload ring: {} segment(s) x {} KiB, peak {:.1f} KiB, exhausted {} times",
                                  segments_.size(), segment_size_ / 1024,
                                  static_cast<f64>(stats_.peak_usage) / 1024.0, stats_.exhausted_frames);
    return out;
}

} // namespace ore
