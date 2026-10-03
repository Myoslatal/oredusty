// Ore framework - forward mesh fragment shader (Lambert + Blinn-Phong highlight).
#version 450

layout(location = 0) in vec3 v_world_position;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;

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

layout(set = 1, binding = 0) uniform sampler2D u_albedo;

layout(push_constant) uniform PushConstants {
    vec4 tint;
    float uv_scale;
    int use_texture;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 albedo = object.base_color.rgb * push.tint.rgb;
    if (push.use_texture != 0) {
        albedo *= texture(u_albedo, v_uv).rgb;
    }

    const vec3 normal = normalize(v_normal);
    const vec3 to_light = normalize(-scene.light_direction.xyz);
    const vec3 to_eye = normalize(scene.camera_position.xyz - v_world_position);
    const vec3 half_vector = normalize(to_light + to_eye);

    const float diffuse = max(dot(normal, to_light), 0.0);
    const float specular = pow(max(dot(normal, half_vector), 0.0), 48.0) * 0.35;
    const float rim = pow(1.0 - max(dot(normal, to_eye), 0.0), 3.0) * 0.15;

    // A cheap two-tone sky/ground ambient term keeps unlit faces readable.
    const vec3 ambient = mix(vec3(0.05, 0.06, 0.08), vec3(0.16, 0.18, 0.22), normal.y * 0.5 + 0.5);

    vec3 color = albedo * (ambient + diffuse) + vec3(specular + rim);
    out_color = vec4(color, object.base_color.a * push.tint.a);
}
