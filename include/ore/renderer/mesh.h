// Ore framework - vertex format, CPU mesh data and GPU meshes.
#pragma once

#include <ore/core/types.h>
#include <ore/math/math.h>
#include <ore/rhi/buffer.h>
#include <ore/rhi/commands.h>
#include <ore/rhi/context.h>
#include <ore/rhi/pipeline.h>

#include <string_view>
#include <vector>

namespace ore {

/// Interleaved vertex format used by the built-in shaders.
struct Vertex {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec2 uv{0.0f};

    /// Vertex input description matching this struct (binding 0).
    [[nodiscard]] static rhi::VertexLayout layout();
    friend bool operator==(const Vertex&, const Vertex&) = default;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    [[nodiscard]] bool empty() const { return vertices.empty() || indices.empty(); }
    [[nodiscard]] usize triangle_count() const { return indices.size() / 3; }
    [[nodiscard]] Aabb bounds() const;
    void transform(const Mat4& matrix);
    void flip_winding();
};

/// Procedural mesh generators (unit sized unless documented otherwise).
[[nodiscard]] MeshData make_cube_data(f32 size = 1.0f);
[[nodiscard]] MeshData make_plane_data(f32 size = 1.0f, u32 subdivisions = 1);
[[nodiscard]] MeshData make_sphere_data(f32 radius = 0.5f, u32 segments = 32, u32 rings = 16);
[[nodiscard]] MeshData make_grid_lines_data(f32 size = 10.0f, u32 divisions = 10);
/// A single triangle in clip space: useful for fullscreen effects and smoke tests.
[[nodiscard]] MeshData make_fullscreen_triangle_data();

class Mesh {
public:
    [[nodiscard]] static Scope<Mesh> create(rhi::GraphicsContext& context, const MeshData& data,
                                           std::string_view debug_name = "mesh");
    [[nodiscard]] static Scope<Mesh> cube(rhi::GraphicsContext& context, f32 size = 1.0f);
    [[nodiscard]] static Scope<Mesh> plane(rhi::GraphicsContext& context, f32 size = 1.0f, u32 subdivisions = 1);
    [[nodiscard]] static Scope<Mesh> sphere(rhi::GraphicsContext& context, f32 radius = 0.5f, u32 segments = 32,
                                           u32 rings = 16);
    [[nodiscard]] static Scope<Mesh> grid(rhi::GraphicsContext& context, f32 size = 10.0f, u32 divisions = 10);

    [[nodiscard]] rhi::Buffer& vertex_buffer() const { return *vertex_buffer_; }
    [[nodiscard]] rhi::Buffer& index_buffer() const { return *index_buffer_; }
    [[nodiscard]] u32 vertex_count() const { return vertex_count_; }
    [[nodiscard]] u32 index_count() const { return index_count_; }
    [[nodiscard]] VkPrimitiveTopology topology() const { return topology_; }
    [[nodiscard]] const Aabb& bounds() const { return bounds_; }

    /// Binds the vertex/index buffers and issues one indexed draw call.
    void draw(rhi::CommandBuffer& cmd, u32 instance_count = 1, u32 first_instance = 0) const;
    /// For line meshes and wireframe overlays: draws without an index buffer.
    void draw_non_indexed(rhi::CommandBuffer& cmd, u32 instance_count = 1) const;

private:
    Mesh() = default;

    Scope<rhi::Buffer> vertex_buffer_;
    Scope<rhi::Buffer> index_buffer_;
    Aabb bounds_{};
    u32 vertex_count_ = 0;
    u32 index_count_ = 0;
    VkPrimitiveTopology topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
};

} // namespace ore
