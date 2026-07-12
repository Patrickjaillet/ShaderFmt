#version 110

// Legacy GLSL 1.10 style: no 'layout', attribute/varying instead of in/out
attribute vec3 aPosition;
attribute vec2 aUv;
varying vec2 vUv;

uniform mat4 uModelViewProj;

void main() {
    vUv = aUv;
    gl_Position = uModelViewProj * vec4(aPosition, 1.0);
}
