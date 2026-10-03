// Tile2D - sprite/tilemap fragment shader.
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    const vec4 texel = texture(u_atlas, v_uv);
    out_color = texel * v_color;
    if (out_color.a < 0.004) discard;  // keeps the grid lines of the debug overlays crisp
}
