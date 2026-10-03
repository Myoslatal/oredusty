// Ore framework - grid colouring: axis lines highlighted, distance fade towards the horizon.
#version 450

layout(location = 0) in vec3 v_world_position;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec4 camera_position;
    vec4 light_direction;
} scene;

layout(location = 0) out vec4 out_color;

void main() {
    const float distance_to_eye = length(v_world_position - scene.camera_position.xyz);
    const float fade = clamp(1.0 - distance_to_eye / 45.0, 0.0, 1.0);

    vec3 color = vec3(0.28, 0.30, 0.34);
    if (abs(v_world_position.x) < 0.02) color = vec3(0.75, 0.25, 0.28);  // Z axis
    if (abs(v_world_position.z) < 0.02) color = vec3(0.25, 0.45, 0.80);  // X axis

    out_color = vec4(color * fade, fade * 0.9);
}
