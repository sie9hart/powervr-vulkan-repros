#version 450
// Enough work to keep the compiler busy; the specialization constant makes every pipeline unique.
layout(constant_id = 0) const int SEED = 0;
layout(location = 0) out vec4 color;
void main() {
    vec4 acc = vec4(float(SEED) * 0.001);
    vec2 p = gl_FragCoord.xy * 0.01;
    for (int i = 0; i < 24; ++i) {
        p = vec2(sin(p.x * 1.7 + float(i)), cos(p.y * 1.3 - float(SEED + i)));
        acc += vec4(p, p.yx) * (1.0 / float(i + 1));
        if (acc.x > 3.0) acc = fract(acc);
    }
    color = acc;
}
