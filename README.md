# ShaderFmt

A desktop code formatter and editor for shader source code. Paste or
open a shader, format it in place, tweak the result, copy it out —
all without ever leaving the editor.

![ShaderFmt editor screenshot](docs/screenshot.png)

## Features

- **Formats without changing behavior.** Only whitespace, indentation,
  and brace placement are ever touched — never your code's logic.
- **Six shader dialects**, auto-detected from what you paste (or pick
  manually):
  - GLSL (OpenGL/Vulkan)
  - Shadertoy (GLSL with `mainImage`, `iTime`, `iResolution`, etc.)
  - HLSL (DirectX)
  - WGSL (WebGPU)
  - MSL (Metal Shading Language)
  - Unity ShaderLab (including embedded Cg/HLSL `CGPROGRAM` blocks)
- **Configurable style**: indentation width, tabs vs. spaces, brace
  style (K&R or Allman), spacing around control-flow keywords, and a
  visual line-length guide — with one-click presets (`Default`,
  `Shadertoy`, `Unreal`, `Unity`) and a live preview. Your preferences
  are remembered between sessions.
- **Format-on-the-fly**, undo/redo, drag-and-drop, open/save files.
- **Non-blocking hints**: a GLSL-specific check flags qualifiers that
  don't match your `#version` (e.g. `attribute`/`varying` used with
  `#version 330`, or `layout(...)` used before `#version 140`) with a
  gentle underline — it never stops you from formatting or editing.
- **Live render preview** (GLSL/Shadertoy only): a docked panel renders
  your fragment shader on a fullscreen quad as you type, with Shadertoy's
  standard `iResolution`/`iTime`/`iMouse`/`iFrame` uniforms wired up and
  animated continuously. Debounced independently of format-on-the-fly, so
  it doesn't recompile on every keystroke. Falls back to a plain message
  (compile error, or "not available for this language") instead of a
  fake or silently-wrong render for anything it can't show — HLSL/WGSL/
  MSL/ShaderLab would each need their own real graphics backend
  (Direct3D/wgpu/Metal/Unity itself), none of which are wired up here.
- Runs entirely offline. Nothing is ever sent anywhere.

## Installing

Windows builds are available from the
[Releases page](https://github.com/Patrickjaillet/ShaderFmt/releases):
grab the `.exe` installer for a normal install (Start Menu shortcut,
uninstaller included), or the `.zip` if you'd rather just unpack and
run.

macOS and Linux builds aren't published yet — see
[Building from source](#building-from-source) below.

## Building from source

Requirements: CMake 3.16+, a C++17 compiler.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The formatting engine (`lib/`) has no external dependencies and builds
on its own. The desktop editor additionally needs Qt6 (`Widgets` +
`Concurrent`) and QScintilla built against that same Qt — if Qt6 isn't
found, the editor is skipped automatically and the engine/tests still
build and run fine.

```bash
# 1. Qt6 (via aqtinstall)
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O C:/Qt

# 2. QScintilla, built from source against that Qt (no prebuilt package
#    exists for Qt6). From a Visual Studio developer command prompt,
#    with the Qt bin directory from step 1 first on PATH:
curl -LO <QScintilla-2.14.1 source tarball, from pypi.org/project/QScintilla>
tar xzf QScintilla-2.14.1.tar.gz
cd QScintilla-2.14.1/src
qmake CONFIG+=staticlib qscintilla.pro
nmake
nmake install

# 3. Build ShaderFmt itself, in Release (QScintilla above was built
#    Release; mixing Debug/Release causes MSVC runtime link errors)
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --target shaderfmt_app
```

On Windows, the build automatically runs `windeployqt` right after
linking `shaderfmt_app.exe`, so `build/app/Release/shaderfmt_app.exe`
(or `build/app/shaderfmt_app.exe` with a single-config generator) is
directly runnable — its Qt DLLs are copied alongside it, no need to have
Qt on `PATH`. If you see "missing Qt6Core.dll" or similar when running
it, `windeployqt` wasn't found at configure time (check the CMake
configure log for a warning) — make sure Qt's `bin/` directory is on
`PATH` or in `CMAKE_PREFIX_PATH` and reconfigure.

To produce a standalone package elsewhere (a `.zip`, and on Windows also
an NSIS `.exe` installer if `makensis` is available):

```bash
cpack --config build/CPackConfig.cmake -B build
```

## Known limitations

- Comments and code are preserved exactly, but multiple consecutive
  blank lines collapse down to a single blank line.
- The line-length setting is a visual guide only — nothing wraps long
  lines automatically.
- Syntax highlighting is by token category (identifier, number,
  string, comment, preprocessor), not by per-language keyword.
- Spacing around unary operators (`-x`, `!x`) and WGSL generics
  (`vec3<f32>`) can look slightly off in edge cases, though it's never
  semantically wrong.
- No Shadertoy sample ships in the test corpus due to Shadertoy's
  default non-commercial license — everything else is exercised
  against real, license-verified open-source shaders (see
  `tests/corpus/licensed/SOURCES.md`).

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for how to build, test, and
add a new language or style option. Please also read our
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).

## License

GPL v3 — see [`LICENSE`](LICENSE).

Copyright (c) 2026 Patrick JAILLET.
