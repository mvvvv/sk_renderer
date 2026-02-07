// SPDX-License-Identifier: MIT
// Video playback module using FFmpeg with Vulkan hardware acceleration

#pragma once

#include <sk_renderer.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct video_t video_t;

typedef enum video_decode_status_ {
	video_decode_ok,      // Frame decoded successfully
	video_decode_eof,     // End of stream
	video_decode_aborted, // Interrupted by seek/abort
	video_decode_error,   // Decode error
} video_decode_status_;

///////////////////////////////////////////////////////////////////////////////
// Lifecycle
///////////////////////////////////////////////////////////////////////////////

video_t*   video_open             (const char* uri); // Open a video file/url for playback. Returns NULL on failure
void       video_destroy          (      video_t* v); // Destroy video and release all resources
bool       video_is_valid         (const video_t* v); // Check if video is valid and ready for playback

///////////////////////////////////////////////////////////////////////////////
// Metadata
///////////////////////////////////////////////////////////////////////////////

int32_t    video_get_width        (const video_t* v);
int32_t    video_get_height       (const video_t* v);
double     video_get_duration     (const video_t* v); // Duration in seconds (0 for live streams)
double     video_get_framerate    (const video_t* v); // Frames per second
double     video_get_current_time (const video_t* v); // Current playback position in seconds
bool       video_is_live          (const video_t* v); // True for live streams (no duration)
bool       video_is_seekable      (const video_t* v); // True if seeking is supported
bool       video_is_hw_accelerated(const video_t* v); // Returns true if using Vulkan/VAAPI hardware decode, false for software decode

///////////////////////////////////////////////////////////////////////////////
// Decoding
///////////////////////////////////////////////////////////////////////////////

video_decode_status_ video_decode_next_frame(video_t* v); // Decode the next frame. Returns status indicating success, EOF, abort, or error
bool                 video_seek             (video_t* v, double time_seconds); // Seek to a specific time (seconds). Returns true on success.
void                 video_abort_decode     (video_t* v); // Interrupt any blocking I/O in video_decode_next_frame (thread-safe)

///////////////////////////////////////////////////////////////////////////////
// Rendering
///////////////////////////////////////////////////////////////////////////////

// Get the video material for rendering. The video module owns the shader,
// material, and textures internally. The returned material is ready to use
// with a fullscreen quad - texture bindings and UV crop are updated
// automatically after each decoded frame. Returns NULL if no frame is ready.
skr_material_t* video_get_material(video_t* v);

///////////////////////////////////////////////////////////////////////////////
// Utility
///////////////////////////////////////////////////////////////////////////////

// Extract a thumbnail from the first frame of a video file.
// Returns an RGBA texture (caller owns it and must call skr_tex_destroy).
// max_size: Maximum dimension (width or height) of the thumbnail.
// Returns invalid texture on failure.
skr_tex_t video_extract_thumbnail(const char* filename, int32_t max_size);

#ifdef __cplusplus
}
#endif
