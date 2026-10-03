// Ore framework - infinite-ish ground grid (line list, no vertex buffers needed for sizing).
#version 450

layout(location = 0) in vec3 in_position;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 view_projection;
    vec4 camera_position;
    vec4 light_direction;
} scene;

layout(location = 0) out vec3 v_world_position;

void main() {
    v_world_position = in_position;
    gl_Position = scene.view_projection * vec4(in_position, 1.0);
}
