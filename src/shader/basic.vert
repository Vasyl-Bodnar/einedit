#version 450

layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 in_uv;

layout(set = 0, binding = 1) uniform UniformBufferObject {
  layout(row_major) mat4 proj;
} ubo;

layout(location = 0) out vec2 uv;

void main() {
    gl_Position = ubo.proj * vec4(pos, 0.0, 1.0);
    uv = in_uv;
}
