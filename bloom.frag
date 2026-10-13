#version 450
// Pasada 2: Bloom (muestreo en espiral de dos radios sobre el brillo) + viñeta.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(push_constant) uniform PC {
    vec2  resolution;
    float phase;
    float amp;
    float bass;
    float treble;
    float speed;
    float glow;
    float cellSize;
    float pxRange;
    vec2  atlasGrid;
    float glyphCount;
    float mid;
} pc;

vec3 brightPass(vec3 c) {
    float l = max(max(c.r, c.g), c.b);
    return c * smoothstep(0.35, 0.9, l);
}

void main() {
    vec2 px = 1.0 / pc.resolution;
    float s = pc.resolution.x / 1080.0;
    vec3 scene = texture(uScene, vUv).rgb;

    vec3 bloom = vec3(0.0);
    const int N = 20;
    for (int ring = 0; ring < 2; ring++) {
        float radius = (ring == 0 ? 5.0 : 16.0) * s * (1.0 + pc.bass * 0.6);
        float w = (ring == 0) ? 1.0 : 0.8;
        for (int i = 0; i < N; i++) {
            float a = float(i) * 2.399963 + float(ring) * 1.3;
            float r = sqrt((float(i) + 0.5) / float(N));
            vec2 o = vec2(cos(a), sin(a)) * r * radius * px;
            bloom += brightPass(texture(uScene, vUv + o).rgb) * (1.0 - 0.5 * r) * w / float(N);
        }
    }

    vec3 col = scene + bloom * pc.glow * 1.1;

    vec2 q = vUv - 0.5;
    col *= 1.0 - dot(q, q) * 0.9;
    outColor = vec4(col, 1.0);
}
