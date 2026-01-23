# sk_renderer

A Vulkan 1.1 renderer targeting Android/Linux/Windows/MacOS! Written with C11, and desgined for use in StereoKit (and elsewhere). Core goal is performance and compatability on XR standalone headsets.

It uses a mid-high level API, with an opinionated instanced rendering system, allowing for easy use and internal implementation flexibility. This renderer uses HLSL for shaders, and uses its own skshaderc tool to compile HLSL to a custom format containing optimized SPIRV with metadata.

The full API for sk_renderer [can be found in `sk_renderer.h`](sk_renderer/include/sk_renderer.h).

## Examples and tools

For usage, please see the [example project](/example/) for example implementations of various features. For usage with OpenXR, see the [example_xr project](/example_xr/).
- [Basic mesh drawing.](/example/scene_meshes.c)
- [PBR via GLTF.](/example/scene_gltf.c)
- [Gaussian splat rendering.](/example/scene_gaussian_splat.c)
- [Curve based text rendering.](/example/scene_text.c)
- [Basic shadow mapping.](/example/scene_shadows.c)
- [Compute shaders.](/example/scene_reaction_diffusion.c)
- [Texture compression.](/example/scene_tex_compress.c)
- ... and more

Along with the core sk_renderer, the example projects include a number of useful high-level tools. Many of these are completely standalone, or may be adaptable to other contexts.
- [scene_util.h/c](/example/tools/scene_util.h) - Utilities for kickstarting projects.
  - GLTF Loader
  - STB image loading
  - Mesh generation
  - Texture generation
  - Default system info and vertex types
- [hdr_load.h](/example/tools/hdr_load.h) - Fast load .hdr to rg11b10 or rgb9e5 formats.
- [micro_ply.h](/example/tools/micro_ply.h) - Gaussian Splat friendly .ply loader.
- [float_math.h](/example/tools/float_math.h) - SIMD optimized vector math.
- [tex_compress.h/c](/example/tools/tex_compress.h) - Fast convert images to BC1/ETC2 format.
- [text.h/c](/example/text/text.h) - Vector text rendering.

## Dear ImGui

sk_renderer has a [Dear ImGui rendering backend](/example/imgui_backend/) with prebuilt shaders that pairs quite well with [sk_app](https://github.com/StereoKit/sk_app), another similar tool that has a [Dear ImGui platform backend](https://github.com/StereoKit/sk_app/tree/main/examples/imgui_example). These make for a great combo for building portable Dear ImGui applications that work on all major operating systems.

## Building

**Prerequisites:** CMake 3.10+

From the repository root:

```bash
cmake -B build
cmake --build build -j8 --target run
```

```bash
# For Android
cmake -B build-android -G Ninja -DCMAKE_ANDROID_NDK=$ANDROID_NDK_HOME -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=32 -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a
cmake --build build-android -j8
cmake --build build-android -j8 --target run_apk

cmake -B build-androidx86 -G Ninja -DCMAKE_ANDROID_NDK=$ANDROID_NDK_HOME -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=32 -DCMAKE_ANDROID_ARCH_ABI=x86_64
cmake --build build-androidx86 -j8
cmake --build build-androidx86 -j8 --target run_apk
```

```bash
# For Windows .exe from linux with mingw-w64
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j8
cd build-mingw/ && wine example/sk_renderer_test.exe ; cd -
```