struct Uniforms {
    view_proj: mat4x4<f32>,
    light_dir: vec3<f32>,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

@vertex
fn vs_main(@location(0) position: vec3<f32>) -> @builtin(position) vec4<f32> {
    return uniforms.view_proj * vec4<f32>(position, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    /* flat white for now */
    return vec4<f32>(1.0, 1.0, 1.0, 1.0);
}
