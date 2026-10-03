// Ore framework - example 01: a triangle generated from gl_VertexIndex, no vertex buffer.
#version 450

layout(push_constant) uniform PushConstants {
    vec4 color;
    float time;
} push;

layout(location = 0) out vec3 v_color;

void main() {
    // Three vertices with a bit of idle animation so the frame is obviously live.
    const vec2 positions[3] = vec2[3](vec2(0.0, -0.55), vec2(0.55, 0.45), vec2(-0.55, 0.45));
    const vec3 colors[3] = vec3[3](vec3(0.95, 0.35, 0.35), vec3(0.35, 0.85, 0.45), vec3(0.35, 0.55, 0.95));

    const float wobble = 0.06 * sin(push.time * 1.7 + float(gl_VertexIndex) * 2.0);
    vec2 position = positions[gl_VertexIndex];
    position *= 1.0 + wobble;

    gl_Position = vec4(position, 0.0, 1.0);
    v_color = colors[gl_VertexIndex] * push.color.rgb;
}
