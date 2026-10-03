// Ore framework - forward mesh vertex shader.
// Set 0 binding 0: per-frame scene uniforms (non dynamic).
// Set 0 binding 1: per-object uniforms (dynamic uniform buffer, one slice per draw).
// Push constants: per-draw tint, uv scale and texture switch.
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec4 camera_position;
    vec4 light_direction;
} scene;

layout(set = 0, binding = 1) uniform ObjectUniforms {
    mat4 model;
    mat4 normal_matrix;
    vec4 base_color;
} object;

layout(push_constant) uniform PushConstants {
    vec4 tint;
    float uv_scale;
    int use_texture;
} push;

layout(location = 0) out vec3 v_world_position;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;

void main() {
    const vec4 world_position = object.model * vec4(in_position, 1.0);
    v_world_position = world_position.xyz;
    v_normal = normalize(mat3(object.normal_matrix) * in_normal);
    v_uv = in_uv * push.uv_scale;
    gl_Position = scene.view_projection * world_position;
}
