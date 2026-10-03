// Ore framework - example 01 fragment shader.
#version 450

layout(location = 0) in vec3 v_color;

layout(push_constant) uniform PushConstants {
    vec4 color;
    float time;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    // Soft vignette towards the edges of the triangle.
    out_color = vec4(v_color, push.color.a);
}
