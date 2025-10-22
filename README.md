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

This builds and runs the example application with multiple demo scenes. Use arrow keys to switch between scenes.

## Documentation

- [sk_renderer/include/sk_renderer.h](sk_renderer/include/sk_renderer.h) - Public API header
- [example/](example/) - Reference implementation
