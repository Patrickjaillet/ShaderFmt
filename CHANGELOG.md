# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).
Versions from 0.2.0 onward are generated automatically from commit
history; the 0.1.0 entry below was written by hand.

## [0.2.1](https://github.com/Patrickjaillet/ShaderFmt/compare/v0.2.0...v0.2.1) (2026-07-12)

### Bug Fixes

* **ci:** trigger workflow on push to master, not main ([6211dbf](https://github.com/Patrickjaillet/ShaderFmt/commit/6211dbf392226d7650de3d99beffa9af9e8d1917))

## [0.2.0](https://github.com/Patrickjaillet/ShaderFmt/compare/v0.1.0...v0.2.0) (2026-07-12)

### Features

* GLSL version lint, configurable spacing, WGSL/naga reference test, NSIS installer, release tooling ([75d7f5a](https://github.com/Patrickjaillet/ShaderFmt/commit/75d7f5afdaa9723263c705bf8a7f4bf5fd4cc9e9))

## [0.1.0]

- Lexer, tolerant recursive-descent parser, and lossless concrete syntax
  tree (CST) covering GLSL, Shadertoy, HLSL, WGSL, MSL, and Unity
  ShaderLab (with embedded Cg/HLSL `CGPROGRAM`/`ENDCG` blocks parsed as a
  nested sub-language).
- Language auto-detection (`detectLanguage()`) with a real, sourced-shader
  and regression-tested heuristic per dialect.
- AST-driven pretty-printer/emitter that reconstructs formatted output
  from the parsed tree, preserving comments as trivia attached to their
  owning node.
- Qt6 + QScintilla desktop editor (`app/`) with syntax highlighting,
  language selection, and asynchronous (non-blocking) formatting.
- Style configuration panel with presets, live preview, and persisted
  settings (`QSettings`).
- Reference-compiler non-regression testing against `glslangValidator`
  for GLSL/HLSL corpus entries.
- Fuzz and truncation testing to guarantee the lexer/parser never throws
  or crashes on malformed or partial input.
- CI (`.github/workflows/ci.yml`) building and testing on Linux, Windows,
  and macOS.
- A licensed, provenance-tracked shader corpus (`tests/corpus/licensed/`,
  see `SOURCES.md`) sourced from real open-source projects (Khronos
  Vulkan-Samples, Microsoft DirectX-Graphics-Samples, wgpu, SDL, Unity
  Technologies).

### Known limitations

See the "Known limitations" section of `README.md` for the current,
full list.
