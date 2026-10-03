// Tile2D - sprite/tilemap vertex shader: one batched draw call for everything on screen.
#version 450

layout(location = 0) in vec2 in_position;   // world units (pixels)
layout(location = 1) in vec2 in_uv;         // atlas coordinates, already normalised
layout(location = 2) in vec4 in_color;      // per vertex tint (RGBA8)

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = pc.view_projection * vec4(in_position, 0.0, 1.0);
}
