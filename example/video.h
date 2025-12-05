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

bool       video_decode_next_frame(video_t* v); // Returns true if a frame was decoded, false on EOF or error
bool       video_seek             (video_t* v, double time_seconds); //Seek to a specific time (seconds). Returns true on success.
skr_tex_t* video_get_tex_y        (video_t* v); // Get the Y (luma) plane texture for the current frame. Format: R8 (single channel, full resolution)
skr_tex_t* video_get_tex_uv       (video_t* v); // Get the UV (chroma) plane texture for the current frame. Format: RG8 (two channels interleaved, half resolution)

///////////////////////////////////////////////////////////////////////////////
// Utility
///////////////////////////////////////////////////////////////////////////////

// Extract a thumbnail from the first frame of a video file
// Returns an RGBA texture (caller owns it and must call skr_tex_destroy)
// max_size: Maximum dimension (width or height) of the thumbnail
// Returns invalid texture on failure
skr_tex_t video_extract_thumbnail(const char* filename, int32_t max_size);

#ifdef __cplusplus
}
#endif
