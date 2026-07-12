# Corpus provenance

Real, open-source shader files vendored verbatim (or verbatim plus an
added attribution header, noted below) for `tests/corpus/`, distinct from
the hand-written fixtures in the parent directory. Each file's own header
carries its original copyright/license notice; this file is a summary
index, not a substitute for those notices.

| File | Source | License | Notes |
|---|---|---|---|
| `base.frag` | [KhronosGroup/Vulkan-Samples](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/shaders/base.frag) | Apache-2.0 (per-file SPDX header) | References `#include "lighting.h"`, which isn't vendored - the lexer never resolves `#include` (captured as an opaque, uninterpreted token, see `lib/src/Lexer.cpp`), so this is fine for lexing/parsing/formatting, but it means this file can't run through the reference-compiler test (`glslangValidator` would try to actually open the include and fail) - excluded from `compileChecks()` in `tests/test_main.cpp` for that reason, still covered by every other corpus test. |
| `base.vert` | [KhronosGroup/Vulkan-Samples](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/shaders/base.vert) | Apache-2.0 (per-file SPDX header) | Self-contained, covered by every corpus test including the reference-compiler check. |
| `hello_triangle.hlsl` | [microsoft/DirectX-Graphics-Samples](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12HelloWorld/src/HelloTriangle/shaders.hlsl) | MIT (per-file header) | Vertex (`VSMain`) + pixel (`PSMain`) shader in one file, HLSL semantics (`SV_POSITION`, `COLOR`). |
| `hello_triangle.wgsl` | [gfx-rs/wgpu](https://github.com/gfx-rs/wgpu/blob/trunk/examples/features/src/hello_triangle/shader.wgsl) | Apache-2.0 OR MIT (repo dual license, no per-file header - attribution comment added at the top of the vendored copy) | |
| `hello_workgroups.wgsl` | [gfx-rs/wgpu](https://github.com/gfx-rs/wgpu/blob/trunk/examples/features/src/hello_workgroups/shader.wgsl) | Apache-2.0 OR MIT (repo dual license, no per-file header - attribution comment added at the top of the vendored copy) | Uses `if`/`else if` conditions **without** parentheses (valid WGSL, no C-like requirement) - this file is what surfaced and got a regression test for that grammar gap (`ast_wgsl_if_and_while_conditions_without_parens` in `tests/test_main.cpp`) before it was added here. |
| `Metal_Blit.metal` | [libsdl-org/SDL](https://github.com/libsdl-org/SDL/blob/main/src/gpu/metal/Metal_Blit.metal) | Zlib (repo `LICENSE.txt`, no per-file header - attribution comment added at the top of the vendored copy) | Real-world MSL: `#include`/`using namespace metal`, `[[attribute]]` forms, `#if`/`#endif` wrapping whole functions, `switch`/`case`. No reference compiler is wired up for MSL (Apple's Metal compiler is macOS-only, unavailable in this Windows environment) - excluded from `compileChecks()`, covered by every other corpus test. |
| `Mobile-Diffuse.shader` | [TwoTailsGames/Unity-Built-in-Shaders](https://github.com/TwoTailsGames/Unity-Built-in-Shaders/blob/master/DefaultResourcesExtra/Mobile/Mobile-Diffuse.shader) | MIT (repo `license.txt`, "Copyright (c) 2016 Unity Technologies" - Unity's own built-in shader source, republished under MIT; per-file attribution already present in the original) | Real-world ShaderLab: a "surface shader" (`#pragma surface`) with `CGPROGRAM` directly inside `SubShader` (no explicit `Pass` block - valid, Unity auto-generates one) and a top-level `Fallback "..."` command outside any block. Not compilable with any reference compiler available here (no Unity/Cg toolchain on this machine) - excluded from `compileChecks()`, covered by every other corpus test. |

No Shadertoy-dialect file is included here: Shadertoy's own default license
(CC BY-NC-SA per their terms of service unless an author states otherwise)
is non-commercial and share-alike, which doesn't clear the bar for
vendoring into an open-source repo without per-shader verification: the
hand-written `shadertoy.glsl`/`shadertoy_extra.glsl` fixtures remain the
Shadertoy-dialect coverage for now.
