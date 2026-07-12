// Shadertoy buffer pass sampling a channel and reacting to the mouse
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec2 mouse = iMouse.xy / iResolution.xy;

    vec4 prev = texture(iChannel0, uv);
    float pulse = 0.5 + 0.5 * sin(iTime + float(iFrame) * 0.01);

    fragColor = mix(prev, vec4(mouse, pulse, 1.0), 0.1);
}
