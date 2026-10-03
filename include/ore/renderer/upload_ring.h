// Ore framework - per-frame dynamic data ring buffer.
//
// Uniform/storage data that changes every frame is written into a host visible buffer that is
// split into one segment per frame-in-flight. A segment is only reused once its frame has been
// retired, so the GPU never reads data that the CPU is overwriting.
#pragma once

#include <ore/core/types.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/device.h>

#include <string>
#include <vector>

namespace ore {

class UploadRing {
public:
    struct Slice {
        u64 offset = 0;
        void* mapped = nullptr;
        u64 size = 0;
        [[nodiscard]] bool valid() const { return mapped != nullptr && size > 0; }
    };

    struct Stats {
        u64 segment_size = 0;
        u64 peak_usage = 0;
        u32 allocations_this_frame = 0;
        u32 exhausted_frames = 0;
    };

    [[nodiscard]] static Scope<UploadRing> create(rhi::GpuAllocator& allocator, u64 segment_size, u32 segment_count,
                                                 std::string_view debug_name = "upload_ring");

    /// Rewinds the segment belonging to \p frame_slot.
    void begin_frame(u32 frame_slot);

    /// Bump-allocates \p size bytes with \p alignment (defaults to the device minimum).
    [[nodiscard]] Slice allocate(u64 size, u64 alignment = 0);
    template <class T>
    [[nodiscard]] T* allocate_array(u64 count, u64 alignment = 0) {
        const Slice slice = allocate(sizeof(T) * count, alignment);
        return slice.valid() ? static_cast<T*>(slice.mapped) : nullptr;
    }

    [[nodiscard]] rhi::Buffer& buffer() const { return *buffer_; }
    [[nodiscard]] VkBuffer handle() const { return buffer_->handle(); }
    [[nodiscard]] u64 segment_size() const { return segment_size_; }
    [[nodiscard]] u32 segment_count() const { return static_cast<u32>(segments_.size()); }
    [[nodiscard]] u64 alignment() const { return alignment_; }
    [[nodiscard]] const Stats& stats() const { return stats_; }
    [[nodiscard]] std::string dump_stats() const;

private:
    UploadRing() = default;

    struct Segment {
        u64 cursor = 0;
        u64 peak = 0;
        u32 allocations = 0;
    };

    Scope<rhi::Buffer> buffer_;
    std::vector<Segment> segments_;
    u64 segment_size_ = 0;
    u64 alignment_ = 256;
    u32 current_ = 0;
    Stats stats_{};
};

} // namespace ore
