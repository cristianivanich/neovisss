#version 450
// Pasada 1: lluvia Matrix procedural + decodificación MSDF + tinte verde dinámico.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(push_constant) uniform PC {
    vec2  resolution;
    float phase;      // tiempo integrado (la velocidad la modula el audio)
    float amp;        // amplitud suavizada 0..1
    float bass;       // graves 0..1
    float treble;     // agudos 0..1
    float speed;
    float glow;
    float cellSize;   // tamaño de celda en píxeles
    float pxRange;    // rango de distancia del MSDF en píxeles del atlas
    vec2  atlasGrid;  // columnas, filas del atlas
    float glyphCount; // glifos válidos
    float mid;
} pc;

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float median3(vec3 v) {
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

// Decodifica un glifo MSDF: mediana de RGB -> distancia con signo -> cobertura nítida.
float glyphCoverage(vec2 f, float index, float cellPx) {
    float gx = mod(index, pc.atlasGrid.x);
    float gy = floor(index / pc.atlasGrid.x);
    vec2 g = vec2(1.0 - f.x, f.y);                 // espejado, como en la película
    g = mix(vec2(0.04), vec2(0.96), g);            // margen para evitar sangrado entre celdas
    vec2 uv = (vec2(gx, gy) + g) / pc.atlasGrid;
    vec3 msd = textureLod(uAtlas, uv, 0.0).rgb;
    float sd = median3(msd) - 0.5;
    float atlasCellPx = float(textureSize(uAtlas, 0).x) / pc.atlasGrid.x;
    float screenPxRange = max(pc.pxRange * cellPx / atlasCellPx, 1.0);
    return clamp(sd * screenPxRange + 0.5, 0.0, 1.0);
}

vec3 rainLayer(vec2 frag, float cellPx, float seed, float speedScale) {
    vec2 p = frag / cellPx;
    float col = floor(p.x);
    float row = floor(p.y);
    vec2 f = fract(p);

    float rows  = ceil(pc.resolution.y / cellPx);
    float hs    = hash11(col * 7.13 + seed + 3.1);
    float hl    = hash11(col * 3.77 + seed + 9.7);
    float hp    = hash11(col * 5.31 + seed + 1.3);

    float trail = mix(8.0, 26.0, hl);
    float total = rows + trail + 6.0;
    float colSpeed = mix(5.0, 15.0, hs) * speedScale;
    float head = mod(pc.phase * colSpeed + hp * total, total);

    float d = head - row;                           // >= 0: celda detrás de la cabeza
    if (d < 0.0 || d >= trail) return vec3(0.0);

    float inten  = pow(1.0 - d / trail, 1.8) * (1.0 + pc.bass * 0.8);
    float isHead = 1.0 - smoothstep(0.0, 1.0, d);

    // Elección de glifo: cada celda parpadea a su propio ritmo
    float rate = mix(0.5, 3.0, hash21(vec2(col, row) + seed));
    float tick = floor(pc.phase * rate * 0.7 + hash21(vec2(row, col) * 1.7) * 50.0);
    float gi = floor(hash21(vec2(col + seed * 13.0, row) + tick * 0.731) * (pc.glyphCount - 1.0));
    if (gi >= 4.0) gi += 1.0;                       // la celda 4 del atlas está vacía

    float cov = glyphCoverage(f, gi, cellPx);

    // Tinte verde dinámico: más brillante con el volumen, hacia turquesa con los agudos
    vec3 base = mix(vec3(0.0, 0.45, 0.10), vec3(0.10, 1.0, 0.35), clamp(pc.amp * 1.4, 0.0, 1.0));
    base = mix(base, vec3(0.35, 1.0, 0.75), clamp(pc.treble, 0.0, 1.0) * 0.5);
    vec3 headCol = vec3(0.80, 1.0, 0.85);

    return mix(base * inten, headCol, isHead) * cov;
}

void main() {
    vec2 frag = gl_FragCoord.xy;
    vec3 c = rainLayer(frag, pc.cellSize, 0.0, 1.0);
    c += 0.45 * rainLayer(frag + vec2(37.0, 91.0), pc.cellSize * 0.6, 17.0, 0.6); // capa lejana
    c *= mix(0.55, 1.15, clamp(pc.amp, 0.0, 1.0));
    outColor = vec4(c, 1.0);
}
