#version 450
// Glide screen-space vertices (640x480 pixels, depth = 1/w) -> Vulkan clip space.
// Depth: the OpenGL version mapped oow to 1 - oow (0 = near); same here.
layout(location = 0) in vec3 in_pos;   // x, y (Glide pixels), oow
layout(location = 1) in vec3 in_stq;   // sow/256, tow/256, oow
layout(location = 2) in vec4 in_color; // unorm
layout(location = 3) in float in_fog;  // unorm
layout(location = 0) out vec3 v_stq;
layout(location = 1) out vec4 v_color;
layout(location = 2) out float v_fog;
void main() {
    v_stq = in_stq;
    v_color = in_color;
    v_fog = in_fog;
    gl_Position = vec4(in_pos.x * (2.0 / 640.0) - 1.0, in_pos.y * (2.0 / 480.0) - 1.0, 1.0 - in_pos.z, 1.0);
}
