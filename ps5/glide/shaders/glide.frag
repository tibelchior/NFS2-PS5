#version 450
layout(set = 0, binding = 0) uniform sampler2DArray u_texBig;   // 256x256 layers
layout(set = 0, binding = 1) uniform sampler2DArray u_texSmall; // 64x64 layers
layout(push_constant) uniform PC {
    vec4 fog_color;
    float tex_enabled;
    float fog_enabled;
    float layer;
    float size;      // texture edge in texels (square, stored in the top-left of its layer)
    float big;       // 1: 256x256 layer array, 0: 64x64 layer array
    float wrap;      // bit 0: repeat in s, bit 1: repeat in t (GR_TEXTURECLAMP_WRAP), otherwise clamp
} pc;
layout(location = 0) in vec3 v_stq;
layout(location = 1) in vec4 v_color;
layout(location = 2) in float v_fog;
layout(location = 0) out vec4 out_color;
void main() {
    vec4 tex = vec4(0.0);
    if (pc.tex_enabled > 0.5) {
        vec2 uv = v_stq.xy / v_stq.z;
        if (mod(pc.wrap, 2.0) > 0.5) uv.x = fract(uv.x);
        if (pc.wrap > 1.5) uv.y = fract(uv.y);
        float half_texel = 0.5 / pc.size;
        uv = clamp(uv, vec2(half_texel), vec2(1.0 - half_texel));   // clamp-to-edge inside the layer
        if (pc.big > 0.5) tex = texture(u_texBig, vec3(uv * (pc.size / 256.0), pc.layer));
        else tex = texture(u_texSmall, vec3(uv * (pc.size / 64.0), pc.layer));
    }
    vec4 ret = v_color * ((1.0 - pc.tex_enabled) + tex);
    if (ret.a <= 16.0 / 255.0) discard;
    ret.rgb = mix(pc.fog_color.rgb, ret.rgb, 1.0 - (1.0 - v_fog) * pc.fog_enabled);
    out_color = ret;
}
