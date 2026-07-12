// Source: https://github.com/gfx-rs/wgpu/blob/trunk/examples/features/src/hello_triangle/shader.wgsl
// License: Apache-2.0 OR MIT (wgpu is dual-licensed; see repository root
// LICENSE.APACHE / LICENSE.MIT). Copyright the wgpu authors and
// contributors. Vendored verbatim for tests/corpus/ as a real,
// license-verified open-source shader.

@vertex
fn vs_main(@builtin(vertex_index) in_vertex_index: u32) -> @builtin(position) vec4<f32> {
    let x = f32(i32(in_vertex_index) - 1);
    let y = f32(i32(in_vertex_index & 1u) * 2 - 1);
    return vec4<f32>(x, y, 0.0, 1.0);
}

@fragment
fn fs_main() -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 0.0, 0.0, 1.0);
}
