// Test shader: a fullscreen triangle whose colour comes from a push constant.
#version 450

layout(push_constant) uniform PushConstants {
    vec4 color;
} push;

layout(location = 0) out vec4 v_color;

void main() {
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    v_color = push.color;
}
