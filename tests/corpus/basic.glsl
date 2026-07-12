#version 450 core

// Simple lighting fragment shader
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

uniform vec3 uLightDir;
uniform sampler2D uAlbedo;


void main() {
    vec3 n = normalize(vNormal);
    float ndotl = max(dot(n, -uLightDir), 0.0); /* clamp to zero */

    vec4 albedo = texture(uAlbedo, vUv);
    vec3 color = albedo.rgb * ndotl;

    for (int i = 0; i < 4; i++) {
        color += vec3(0.01) * float(i);
    }

    fragColor = vec4(color, albedo.a);
}
