# sk_renderer

A cross-platform MIT licensed forward renderer written in modern C with a Vulkan backend! Desgined for use in StereoKit.

## Overview

sk_renderer provides a clean, immediate-mode rendering API designed for VR applications. It abstracts Vulkan complexity while maintaining high performance, targeting Android VR headsets (Vulkan 1.1), Linux, Windows, and macOS via MoltenVK.

**Key Features:**

- Immediate-mode API with no scene graph overhead
- Triple-buffered rendering with async resource uploads
- PBR materials, compute shaders, shadow mapping, and post-processing
- Optimized for mobile VR with MSAA and low CPU overhead

## Building

**Prerequisites:** CMake 3.10+, Vulkan SDK

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
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw -j8
cd build-mingw/ && WINEPATH=./_deps/sdl2-build/ wine example/sk_renderer_test.exe ; cd -
```

This builds and runs the example application with multiple demo scenes. Use arrow keys to switch between scenes.

## Documentation

- [sk_renderer/include/sk_renderer.h](sk_renderer/include/sk_renderer.h) - Public API header
- [example/](example/) - Reference implementation
