// WGSL compute shader: storage buffer and workgroup attributes
struct Particle {
    position: vec3<f32>,
    velocity: vec3<f32>,
};

@group(0) @binding(0) var<storage, read_write> particles: array<Particle>;

@compute @workgroup_size(64)
fn cs_main(@builtin(global_invocation_id) id: vec3<u32>) {
    var p = particles[id.x];
    p.position = p.position + p.velocity;
    particles[id.x] = p;
}
