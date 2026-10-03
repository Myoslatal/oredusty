#include <ore/renderer/mesh.h>

#include <ore/core/assert.h>
#include <ore/core/log.h>

#include <algorithm>
#include <cmath>

namespace ore {

rhi::VertexLayout Vertex::layout() {
    rhi::VertexLayout layout;
    layout.bindings.push_back(rhi::VertexBinding{0, static_cast<u32>(sizeof(Vertex)), VK_VERTEX_INPUT_RATE_VERTEX});
    layout.attributes.push_back(rhi::VertexAttribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                     static_cast<u32>(offsetof(Vertex, position))});
    layout.attributes.push_back(rhi::VertexAttribute{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                     static_cast<u32>(offsetof(Vertex, normal))});
    layout.attributes.push_back(rhi::VertexAttribute{2, 0, VK_FORMAT_R32G32_SFLOAT,
                                                     static_cast<u32>(offsetof(Vertex, uv))});
    return layout;
}

Aabb MeshData::bounds() const {
    Aabb box{};
    if (vertices.empty()) return box;
    box.min = vertices.front().position;
    box.max = vertices.front().position;
    for (const Vertex& vertex : vertices) {
        box.expand(vertex.position);
    }
    return box;
}

void MeshData::transform(const Mat4& matrix) {
    const Mat3 normal_matrix = Mat3(glm::transpose(glm::inverse(matrix)));
    for (Vertex& vertex : vertices) {
        vertex.position = Vec3(matrix * Vec4(vertex.position, 1.0f));
        vertex.normal = glm::normalize(normal_matrix * vertex.normal);
    }
}

void MeshData::flip_winding() {
    for (usize i = 0; i + 2 < indices.size(); i += 3) std::swap(indices[i + 1], indices[i + 2]);
}

MeshData make_cube_data(f32 size) {
    const f32 half = size * 0.5f;
    MeshData mesh;
    mesh.vertices = {
        // +X
        {{half, -half, -half}, {1, 0, 0}, {0, 0}}, {{half, half, -half}, {1, 0, 0}, {0, 1}},
        {{half, half, half}, {1, 0, 0}, {1, 1}}, {{half, -half, half}, {1, 0, 0}, {1, 0}},
        // -X
        {{-half, -half, half}, {-1, 0, 0}, {0, 0}}, {{-half, half, half}, {-1, 0, 0}, {0, 1}},
        {{-half, half, -half}, {-1, 0, 0}, {1, 1}}, {{-half, -half, -half}, {-1, 0, 0}, {1, 0}},
        // +Y
        {{-half, half, -half}, {0, 1, 0}, {0, 0}}, {{-half, half, half}, {0, 1, 0}, {0, 1}},
        {{half, half, half}, {0, 1, 0}, {1, 1}}, {{half, half, -half}, {0, 1, 0}, {1, 0}},
        // -Y
        {{-half, -half, half}, {0, -1, 0}, {0, 0}}, {{-half, -half, -half}, {0, -1, 0}, {0, 1}},
        {{half, -half, -half}, {0, -1, 0}, {1, 1}}, {{half, -half, half}, {0, -1, 0}, {1, 0}},
        // +Z
        {{-half, -half, half}, {0, 0, 1}, {0, 0}}, {{half, -half, half}, {0, 0, 1}, {1, 0}},
        {{half, half, half}, {0, 0, 1}, {1, 1}}, {{-half, half, half}, {0, 0, 1}, {0, 1}},
        // -Z
        {{half, -half, -half}, {0, 0, -1}, {0, 0}}, {{-half, -half, -half}, {0, 0, -1}, {1, 0}},
        {{-half, half, -half}, {0, 0, -1}, {1, 1}}, {{half, half, -half}, {0, 0, -1}, {0, 1}},
    };
    mesh.indices = {0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
                    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23};
    return mesh;
}

MeshData make_plane_data(f32 size, u32 subdivisions) {
    subdivisions = std::max(1u, subdivisions);
    MeshData mesh;
    const u32 steps = subdivisions + 1;
    mesh.vertices.reserve(static_cast<usize>(steps) * steps);
    for (u32 z = 0; z < steps; ++z) {
        for (u32 x = 0; x < steps; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(subdivisions);
            const f32 v = static_cast<f32>(z) / static_cast<f32>(subdivisions);
            Vertex vertex;
            vertex.position = {(u - 0.5f) * size, 0.0f, (v - 0.5f) * size};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.uv = {u, v};
            mesh.vertices.push_back(vertex);
        }
    }
    for (u32 z = 0; z < subdivisions; ++z) {
        for (u32 x = 0; x < subdivisions; ++x) {
            const u32 i0 = z * steps + x;
            const u32 i1 = i0 + 1;
            const u32 i2 = i0 + steps;
            const u32 i3 = i2 + 1;
            mesh.indices.insert(mesh.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return mesh;
}

MeshData make_sphere_data(f32 radius, u32 segments, u32 rings) {
    segments = std::max(3u, segments);
    rings = std::max(2u, rings);
    MeshData mesh;
    for (u32 ring = 0; ring <= rings; ++ring) {
        const f32 v = static_cast<f32>(ring) / static_cast<f32>(rings);
        const f32 phi = v * kPi;
        for (u32 segment = 0; segment <= segments; ++segment) {
            const f32 u = static_cast<f32>(segment) / static_cast<f32>(segments);
            const f32 theta = u * 2.0f * kPi;
            Vertex vertex;
            vertex.normal = {std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            vertex.position = vertex.normal * radius;
            vertex.uv = {u, v};
            mesh.vertices.push_back(vertex);
        }
    }
    const u32 stride = segments + 1;
    for (u32 ring = 0; ring < rings; ++ring) {
        for (u32 segment = 0; segment < segments; ++segment) {
            const u32 a = ring * stride + segment;
            const u32 b = a + stride;
            mesh.indices.insert(mesh.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return mesh;
}

MeshData make_grid_lines_data(f32 size, u32 divisions) {
    divisions = std::max(1u, divisions);
    MeshData mesh;
    const f32 half = size * 0.5f;
    const f32 step = size / static_cast<f32>(divisions);
    for (u32 i = 0; i <= divisions; ++i) {
        const f32 offset = -half + step * static_cast<f32>(i);
        mesh.vertices.push_back(Vertex{{offset, 0.0f, -half}, {0, 1, 0}, {0, 0}});
        mesh.vertices.push_back(Vertex{{offset, 0.0f, half}, {0, 1, 0}, {0, 0}});
        mesh.vertices.push_back(Vertex{{-half, 0.0f, offset}, {0, 1, 0}, {0, 0}});
        mesh.vertices.push_back(Vertex{{half, 0.0f, offset}, {0, 1, 0}, {0, 0}});
    }
    // Grid meshes are line lists: indices are implicit, so the index buffer stays empty.
    return mesh;
}

MeshData make_fullscreen_triangle_data() {
    MeshData mesh;
    mesh.vertices = {
        {{-1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0, 0}},
        {{3.0f, -1.0f, 0.0f}, {0, 0, 1}, {2, 0}},
        {{-1.0f, 3.0f, 0.0f}, {0, 0, 1}, {0, 2}},
    };
    mesh.indices = {0, 1, 2};
    return mesh;
}

Scope<Mesh> Mesh::create(rhi::GraphicsContext& context, const MeshData& data, std::string_view debug_name) {
    if (data.vertices.empty()) {
        ORE_ERROR("Mesh::create: no vertices ('{}')", debug_name);
        return nullptr;
    }

    Scope<Mesh> mesh(new Mesh());
    mesh->vertex_count_ = static_cast<u32>(data.vertices.size());
    mesh->index_count_ = static_cast<u32>(data.indices.size());
    mesh->bounds_ = data.bounds();
    mesh->topology_ = data.indices.empty() ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    rhi::BufferDesc vertex_desc;
    vertex_desc.size = data.vertices.size() * sizeof(Vertex);
    vertex_desc.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
    vertex_desc.debug_name = std::string(debug_name) + ".vertices";
    mesh->vertex_buffer_ = rhi::Buffer::create(context.allocator(), vertex_desc);
    if (mesh->vertex_buffer_ == nullptr) return nullptr;
    context.upload_buffer(*mesh->vertex_buffer_,
                          ConstSpan<u8>(reinterpret_cast<const u8*>(data.vertices.data()), vertex_desc.size));

    if (!data.indices.empty()) {
        rhi::BufferDesc index_desc;
        index_desc.size = data.indices.size() * sizeof(u32);
        index_desc.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
        index_desc.debug_name = std::string(debug_name) + ".indices";
        mesh->index_buffer_ = rhi::Buffer::create(context.allocator(), index_desc);
        if (mesh->index_buffer_ == nullptr) return nullptr;
        context.upload_buffer(*mesh->index_buffer_,
                              ConstSpan<u8>(reinterpret_cast<const u8*>(data.indices.data()), index_desc.size));
    }
    return mesh;
}

Scope<Mesh> Mesh::cube(rhi::GraphicsContext& context, f32 size) {
    return create(context, make_cube_data(size), "cube");
}

Scope<Mesh> Mesh::plane(rhi::GraphicsContext& context, f32 size, u32 subdivisions) {
    return create(context, make_plane_data(size, subdivisions), "plane");
}

Scope<Mesh> Mesh::sphere(rhi::GraphicsContext& context, f32 radius, u32 segments, u32 rings) {
    return create(context, make_sphere_data(radius, segments, rings), "sphere");
}

Scope<Mesh> Mesh::grid(rhi::GraphicsContext& context, f32 size, u32 divisions) {
    return create(context, make_grid_lines_data(size, divisions), "grid");
}

void Mesh::draw(rhi::CommandBuffer& cmd, u32 instance_count, u32 first_instance) const {
    if (index_count_ == 0) {
        draw_non_indexed(cmd, instance_count);
        return;
    }
    cmd.bind_vertex_buffer(0, *vertex_buffer_);
    cmd.bind_index_buffer(*index_buffer_, VK_INDEX_TYPE_UINT32);
    cmd.draw_indexed(index_count_, instance_count, 0, 0, first_instance);
}

void Mesh::draw_non_indexed(rhi::CommandBuffer& cmd, u32 instance_count) const {
    cmd.bind_vertex_buffer(0, *vertex_buffer_);
    cmd.draw(vertex_count_, instance_count);
}

} // namespace ore
