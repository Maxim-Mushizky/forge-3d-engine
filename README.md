<div align="center">

<pre>
███████╗ ██████╗ ██████╗  ██████╗ ███████╗
██╔════╝██╔═══██╗██╔══██╗██╔════╝ ██╔════╝
█████╗  ██║   ██║██████╔╝██║  ███╗█████╗
██╔══╝  ██║   ██║██╔══██╗██║   ██║██╔══╝
██║     ╚██████╔╝██║  ██║╚██████╔╝███████╗
╚═╝      ╚═════╝ ╚═╝  ╚═╝ ╚═════╝ ╚══════╝
</pre>

### **A Simple 3D Engine & Editor**

*Forge primitives. Import worlds. Trace light.*

[![CI](https://github.com/Maxim-Mushizky/forge-3d-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Maxim-Mushizky/forge-3d-engine/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-22c55e.svg?style=flat-square)](LICENSE)
[![Language: C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=cplusplus)](https://en.cppreference.com/w/cpp/20)
[![Graphics: OpenGL 4.6](https://img.shields.io/badge/OpenGL-4.6-5586A4.svg?style=flat-square&logo=opengl)](https://www.opengl.org/)
[![Platform: Windows 11](https://img.shields.io/badge/Platform-Windows%2011-0078D6.svg?style=flat-square&logo=windows)](#build--run)
[![Build: CMake](https://img.shields.io/badge/Build-CMake%20%2B%20Ninja-064F8C.svg?style=flat-square&logo=cmake)](CMakeLists.txt)

</div>

---

Forge is a small but real-time 3D engine with an integrated editor, written in modern
C++20 on top of OpenGL 4.6. It can place primitives, import free glTF/OBJ assets, edit a
scene with gizmos and an inspector, shade it from flat through PBR, and flip into a
progressive **GPU path-traced** render mode implemented as an OpenGL compute shader.

> **Status:** in development. See [`docs/PLAN.md`](docs/PLAN.md) for the full architecture
> plan and [`docs/PLAN-M8-MODELING.md`](docs/PLAN-M8-MODELING.md) for the modeling roadmap.

<div align="center">

![Forge editor](images/intro_example.jpg)

<sub><i>The Forge editor — dockable viewport, hierarchy, and inspector panels.</i></sub>

</div>

---

## Features

- **Scene editing** — place cube / sphere / plane / cylinder / cone / torus primitives,
  select by click, translate/rotate/scale with on-screen gizmos, edit via an inspector
  sidebar, full undo/redo.
- **Asset import** — glTF 2.0 and OBJ (the formats CC0 libraries like Kenney.nl and
  PolyHaven ship), via header-only `tinygltf` / `tinyobjloader`.
- **Shading** — flat → Blinn-Phong → PBR (metallic/roughness), plus a directional shadow map.
- **Ray tracing** — toggle a progressive path tracer (BVH traversal, lambertian + metallic
  BRDFs, emissive lights, hard shadows) running entirely in a GL compute shader — no
  Vulkan/DXR required, runs on any GL 4.3+ GPU.
- **Dockable editor UI** — Dear ImGui (docking branch) with viewport, hierarchy, inspector,
  and toolbox panels.

## Tech stack

| Concern        | Choice                                  |
|----------------|-----------------------------------------|
| Language       | C++20                                   |
| Build          | CMake 3.29 + Ninja, deps via FetchContent |
| Compiler       | MinGW-w64 GCC 13.2                       |
| Window / input | GLFW 3.4                                |
| Graphics API   | OpenGL 4.6 core                         |
| GL loader      | GLEW (glew-cmake)                       |
| UI / gizmos    | Dear ImGui (docking) + ImGuizmo         |
| Math           | GLM                                     |
| Asset import   | tinygltf + tinyobjloader + stb_image    |
| Scene files    | nlohmann/json                           |

All dependencies are fetched and built automatically at configure time — nothing to install
manually beyond the toolchain.

## Project layout

```
3d-engine/
├── CMakeLists.txt        # root: options, FetchContent deps
├── CMakePresets.json     # mingw-debug / mingw-release
├── run.ps1               # configure + build + launch one-liner
├── docs/                 # architecture & milestone plans
├── engine/               # static lib `forge` (core/platform/renderer/scene/assets/raytrace)
├── editor/               # exe `ForgeEditor` (app, camera, panels, command stack)
└── assets/               # shaders, models, HDRIs
```

## Build & run

Requirements: GCC 13.2 (MinGW-w64), CMake 3.29, Ninja 1.12, Git — on Windows 11 x64.

```powershell
# configure + build (FetchContent downloads deps on first configure)
cmake --preset mingw-release
cmake --build --preset mingw-release
.\build\release\bin\ForgeEditor.exe

# or the one-liner
.\run.ps1                 # debug:  .\run.ps1 -Config debug
```

## Deploying on another machine

There is no installer. Forge is deployed either by **building from source** (recommended — it
is reproducible and avoids path/runtime pitfalls) or by **copying a portable binary bundle**.

### Hardware/OS prerequisites (both methods)

- Windows 10/11, x64.
- A GPU + driver exposing **OpenGL 4.6 core** (compute shaders) — any reasonably modern
  Intel / AMD / NVIDIA GPU with current drivers. Verify with a tool like `glxinfo`/GPU-Z, or
  just launch the editor: it logs the GL version on startup and aborts with a clear message if
  4.6 is unavailable.

### Method A — build from source (recommended)

1. **Install the toolchain via MSYS2** (this is exactly what CI uses, so it is known-good).
   Download and run the installer from <https://www.msys2.org>, then open the **"MSYS2 MINGW64"**
   shell and install the compiler and build tools:

   ```bash
   pacman -Syu                       # update, may ask to reopen the shell once
   pacman -S --needed \
       git \
       mingw-w64-x86_64-gcc \
       mingw-w64-x86_64-cmake \
       mingw-w64-x86_64-ninja
   ```

2. **Put the MinGW toolchain on PATH** so `gcc`/`g++`/`cmake`/`ninja` resolve (the presets call
   them by bare name). In a normal PowerShell session:

   ```powershell
   $env:Path = "C:\msys64\mingw64\bin;$env:Path"   # or add it permanently in System settings
   gcc --version; cmake --version                  # sanity check
   ```

3. **Clone and build.** FetchContent downloads and builds every dependency
   (GLFW, GLEW, GLM, Dear ImGui, ImGuizmo, tinygltf, tinyobjloader, Manifold) on the first
   configure — this needs network access and a few minutes.

   ```powershell
   git clone https://github.com/Maxim-Mushizky/forge-3d-engine.git
   cd forge-3d-engine
   cmake --preset mingw-release
   cmake --build --preset mingw-release
   ```

   > **CMake 4.x note:** a current MSYS2 ships CMake 4, which rejects a dependency that requests
   > policy compatibility below 3.5. If configure fails with that error, append the documented
   > escape hatch (CI does the same):
   > ```powershell
   > cmake --preset mingw-release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
   > ```

4. **Run.** The executable lands in the preset's `bin` directory:

   ```powershell
   .\build\release\bin\ForgeEditor.exe
   # or just:  .\run.ps1
   ```

### Method B — portable binary bundle (no toolchain on the target)

Build once on a machine that has the toolchain, then ship a self-contained folder. Because the
build links the third-party libraries **statically**, the only external runtime dependencies are
the MinGW C/C++ runtime DLLs.

1. On the build machine, assemble a folder:

   ```
   ForgeBundle/
   ├── ForgeEditor.exe              # from build/release/bin/
   ├── assets/                      # the entire assets/ directory from the repo
   ├── libgcc_s_seh-1.dll           # the three MinGW runtime DLLs, copied from
   ├── libstdc++-6.dll              #   C:\msys64\mingw64\bin\
   └── libwinpthread-1.dll
   ```

   Find which DLLs the exe actually needs with `ldd ./build/release/bin/ForgeEditor.exe` in the
   MSYS2 shell — copy every listed DLL whose path is under `mingw64\bin`. OpenGL itself
   (`opengl32.dll`) is provided by the OS/GPU driver — **do not** ship it.

2. **Asset path caveat (important).** The asset directory is currently baked in at compile time
   via the `FORGE_ASSET_DIR` define (the absolute source path on the build machine), so a copied
   `assets/` folder beside the exe is **not** found automatically on a different machine. For a
   portable bundle, build with the define pointing at a relative `assets` folder next to the exe:

   ```powershell
   cmake --preset mingw-release -D FORGE_ASSET_DIR="assets"
   cmake --build --preset mingw-release
   ```

   Then run the exe from the bundle root so `assets/` resolves relative to the working directory.

3. Zip `ForgeBundle/` and copy it to the target machine. The user runs `ForgeEditor.exe` — no
   install, no toolchain required, only an OpenGL 4.6 driver.

## Contributing

Contributions welcome. Read [`CONTRIBUTING.md`](CONTRIBUTING.md) first — it covers the build,
the house style (naming, class structure, RAII/ownership), the mandatory logging policy, and
the error-handling rules.

## License

Released under the [MIT License](LICENSE). © 2026 Maxim Mushizky.
