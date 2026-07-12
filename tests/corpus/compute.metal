#include <metal_stdlib>
using namespace metal;

struct Particle {
    float3 position;
    float3 velocity;
};

kernel void update_particles(device Particle *particles [[buffer(0)]],
                              uint id [[thread_position_in_grid]]) {
    particles[id].position = particles[id].position + particles[id].velocity;
}
