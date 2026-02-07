// SPDX-License-Identifier: MIT
// Video playback module using Android NDK MediaCodec with zero-copy AHB import
//
// Flow: AMediaExtractor → AMediaCodec → AImageReader(YUV_420_888, GPU_SAMPLED)
//       → AImage_getHardwareBuffer → skr_tex_create_external_ahb (VkImage + YcbcrConversion)
//       → video_ahb.hlsl (hardware YUV→RGB via immutable sampler)

#include "video.h"
#include "scene_util.h"

#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <android/hardware_buffer.h>
#include <jni.h>
#include <sk_app.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>

///////////////////////////////////////////////////////////////////////////////
// Internal structures
///////////////////////////////////////////////////////////////////////////////

// Retain enough AImages so the GPU pipeline (3 frames in flight) never reads
// a recycled AHB. +2 for the current frame and one free buffer for the codec.
#define VIDEO_AHB_RETAIN_COUNT            3
#define VIDEO_AHB_IMAGE_READER_MAX_IMAGES (VIDEO_AHB_RETAIN_COUNT + 2)

struct video_t {
	// Stream metadata (read-only after open)
	int32_t     width, height;
	double      duration, framerate, time_base;
	bool        is_live, is_seekable;

	// Playback state
	double      current_pts;
	bool        valid, eof, needs_flush;
	atomic_bool abort_decode;

	// Android demuxer (replaces AVFormatContext)
	AMediaExtractor* extractor;
	int32_t          track_idx;

	// Android decoder + image output
	AMediaCodec*  codec;
	AImageReader* image_reader;
	ANativeWindow* image_reader_surface;

	// Current decoded frame + ring of retained frames to prevent AHB recycling
	// while the GPU pipeline is still reading from them.
	AImage*   current_image;
	AImage*   retained_images[VIDEO_AHB_RETAIN_COUNT];
	int32_t   retain_idx;

	// Pending frame: worker thread stores AHB here, main thread imports it
	// in video_get_material() to avoid racing with the render thread.
	AHardwareBuffer* pending_ahb;
	atomic_bool      has_pending_frame;

	// GPU resources
	skr_tex_t      ahb_tex;       // AHB-imported texture (VkImage + YcbcrConversion)
	skr_shader_t   shader;
	skr_material_t material;
	bool           material_ready;
};

///////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////

static const char* _media_status_str(media_status_t status) {
	switch (status) {
		case AMEDIA_OK:                  return "OK";
		case AMEDIA_ERROR_UNKNOWN:       return "UNKNOWN";
		case AMEDIA_ERROR_MALFORMED:     return "MALFORMED";
		case AMEDIA_ERROR_UNSUPPORTED:   return "UNSUPPORTED";
		case AMEDIA_ERROR_INVALID_OBJECT:return "INVALID_OBJECT";
		default:                         return "???";
	}
}

// Find the first video track in the extractor and select it.
// Returns track index or -1 on failure.
static int32_t _select_video_track(AMediaExtractor* extractor, AMediaFormat** out_format) {
	int32_t count = (int32_t)AMediaExtractor_getTrackCount(extractor);

	for (int32_t i = 0; i < count; i++) {
		AMediaFormat* fmt = AMediaExtractor_getTrackFormat(extractor, (size_t)i);
		const char* mime = NULL;
		AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime);

		if (mime && strncmp(mime, "video/", 6) == 0) {
			media_status_t st = AMediaExtractor_selectTrack(extractor, (size_t)i);
			if (st != AMEDIA_OK) {
				printf("[video] Failed to select track %d: %s\n", i, _media_status_str(st));
				AMediaFormat_delete(fmt);
				continue;
			}
			*out_format = fmt;
			return i;
		}

		AMediaFormat_delete(fmt);
	}

	return -1;
}

// Import AHB into sk_renderer and bind to material
static bool _import_ahb_frame(video_t* v, AHardwareBuffer* ahb) {
	// Destroy previous texture (deferred, GPU-safe)
	if (skr_tex_is_valid(&v->ahb_tex)) {
		skr_tex_destroy(&v->ahb_tex);
		v->ahb_tex = (skr_tex_t){0};
	}

	// Import AHB → VkImage with YcbcrConversion
	skr_err_ err = skr_tex_create_external_ahb((skr_tex_external_ahb_info_t){
		.hardware_buffer = ahb,
		.format          = skr_tex_fmt_none,  // auto-detect from AHB
		.sampler         = su_sampler_linear_clamp,
		.owns_buffer     = false,  // AImage owns the AHB lifetime
	}, &v->ahb_tex);

	if (err != skr_err_success) {
		printf("[video] Failed to import AHB: %d\n", (int)err);
		return false;
	}

	// Bind the texture to the material. skr_material_set_tex auto-detects the
	// YCbCr immutable sampler on the texture and bakes it into the descriptor
	// set layout if needed.
	skr_material_set_tex(&v->material, "tex_video", &v->ahb_tex);

	// Set UV crop to exclude codec padding (H.264 macroblocks round up to 16px,
	// so a 320x180 video may produce a 320x192 AHB).
	AHardwareBuffer_Desc ahb_desc;
	AHardwareBuffer_describe(ahb, &ahb_desc);
	float uv_crop[2] = {
		(ahb_desc.width  > 0) ? (float)v->width  / (float)ahb_desc.width  : 1.0f,
		(ahb_desc.height > 0) ? (float)v->height / (float)ahb_desc.height : 1.0f,
	};
	skr_material_set_param(&v->material, "uv_crop", sksc_shader_var_float, 2, uv_crop);

	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

video_t* video_open(const char* uri) {
	if (!uri) return NULL;

	video_t* v = calloc(1, sizeof(video_t));
	if (!v) return NULL;

	atomic_store(&v->abort_decode, false);

	// Attach this thread to the JVM. AMediaExtractor_setDataSource with HTTP
	// URLs requires a Java thread to create the HTTP service.
	JavaVM* vm       = (JavaVM*)ska_android_get_vm();
	JNIEnv* env      = NULL;
	bool    attached = false;
	if (vm && (*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
		if ((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK) {
			attached = true;
		}
	}

	// Create extractor and set data source
	v->extractor = AMediaExtractor_new();
	if (!v->extractor) {
		printf("[video] Failed to create AMediaExtractor\n");
		goto cleanup;
	}

	media_status_t st = AMediaExtractor_setDataSource(v->extractor, uri);
	if (st != AMEDIA_OK) {
		printf("[video] Failed to set data source '%s': %s\n", uri, _media_status_str(st));
		goto cleanup;
	}

	// Find and select video track
	AMediaFormat* track_format = NULL;
	v->track_idx = _select_video_track(v->extractor, &track_format);
	if (v->track_idx < 0) {
		printf("[video] No video track found in '%s'\n", uri);
		goto cleanup;
	}

	// Extract metadata from track format
	int32_t w = 0, h = 0;
	AMediaFormat_getInt32(track_format, AMEDIAFORMAT_KEY_WIDTH,  &w);
	AMediaFormat_getInt32(track_format, AMEDIAFORMAT_KEY_HEIGHT, &h);
	v->width  = w;
	v->height = h;

	int64_t duration_us = 0;
	if (AMediaFormat_getInt64(track_format, AMEDIAFORMAT_KEY_DURATION, &duration_us)) {
		v->duration = (double)duration_us / 1000000.0;
	}

	int32_t frame_rate = 0;
	if (AMediaFormat_getInt32(track_format, AMEDIAFORMAT_KEY_FRAME_RATE, &frame_rate) && frame_rate > 0) {
		v->framerate = (double)frame_rate;
	} else {
		v->framerate = 30.0; // default
	}

	v->is_live     = (v->duration <= 0.0);
	v->is_seekable = !v->is_live;

	// Get MIME type for codec creation
	const char* mime = NULL;
	AMediaFormat_getString(track_format, AMEDIAFORMAT_KEY_MIME, &mime);
	if (!mime) {
		printf("[video] No MIME type in track format\n");
		AMediaFormat_delete(track_format);
		goto cleanup;
	}

	// Create AImageReader with GPU-sampled usage for zero-copy AHB output
	st = AImageReader_newWithUsage(
		w, h,
		AIMAGE_FORMAT_YUV_420_888,
		AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
		VIDEO_AHB_IMAGE_READER_MAX_IMAGES,
		&v->image_reader
	);
	if (st != AMEDIA_OK) {
		printf("[video] Failed to create AImageReader: %s\n", _media_status_str(st));
		AMediaFormat_delete(track_format);
		goto cleanup;
	}

	st = AImageReader_getWindow(v->image_reader, &v->image_reader_surface);
	if (st != AMEDIA_OK || !v->image_reader_surface) {
		printf("[video] Failed to get AImageReader surface\n");
		AMediaFormat_delete(track_format);
		goto cleanup;
	}

	// Create and configure decoder
	v->codec = AMediaCodec_createDecoderByType(mime);
	if (!v->codec) {
		printf("[video] Failed to create decoder for '%s'\n", mime);
		AMediaFormat_delete(track_format);
		goto cleanup;
	}

	st = AMediaCodec_configure(v->codec, track_format, v->image_reader_surface, NULL, 0);
	AMediaFormat_delete(track_format);
	track_format = NULL;

	if (st != AMEDIA_OK) {
		printf("[video] Failed to configure decoder: %s\n", _media_status_str(st));
		goto cleanup;
	}

	st = AMediaCodec_start(v->codec);
	if (st != AMEDIA_OK) {
		printf("[video] Failed to start decoder: %s\n", _media_status_str(st));
		goto cleanup;
	}

	// Create shader and material
	v->shader = su_shader_load("shaders/video_ahb.hlsl.sks", "video_ahb");
	if (!skr_shader_is_valid(&v->shader)) {
		printf("[video] Failed to load video_ahb shader\n");
		goto cleanup;
	}

	skr_err_ err = skr_material_create((skr_material_info_t){
		.shader     = &v->shader,
		.cull       = skr_cull_none,
		.write_mask = skr_write_default,
		.depth_test = skr_compare_always,
	}, &v->material);

	if (err != skr_err_success) {
		printf("[video] Failed to create material: %d\n", (int)err);
		goto cleanup;
	}

	v->valid = true;

	// Decode first frame
	video_decode_next_frame(v);

	if (attached) (*vm)->DetachCurrentThread(vm);
	return v;

cleanup:
	if (attached) (*vm)->DetachCurrentThread(vm);
	video_destroy(v);
	return NULL;
}

void video_destroy(video_t* v) {
	if (!v) return;

	// Destroy rendering resources
	skr_material_destroy(&v->material);
	skr_shader_destroy(&v->shader);
	skr_tex_destroy(&v->ahb_tex);

	// Release current and all retained AImages
	if (v->current_image) AImage_delete(v->current_image);
	for (int32_t i = 0; i < VIDEO_AHB_RETAIN_COUNT; i++) {
		if (v->retained_images[i]) AImage_delete(v->retained_images[i]);
	}

	// Stop and delete codec
	if (v->codec) {
		AMediaCodec_stop(v->codec);
		AMediaCodec_delete(v->codec);
	}

	// Delete image reader (after codec, since codec renders to its surface)
	if (v->image_reader) AImageReader_delete(v->image_reader);

	// Delete extractor
	if (v->extractor) AMediaExtractor_delete(v->extractor);

	free(v);
}

bool video_is_valid(const video_t* v) {
	return v && v->valid;
}

int32_t video_get_width(const video_t* v) {
	return v ? v->width : 0;
}

int32_t video_get_height(const video_t* v) {
	return v ? v->height : 0;
}

double video_get_duration(const video_t* v) {
	return v ? v->duration : 0.0;
}

double video_get_framerate(const video_t* v) {
	return v ? v->framerate : 0.0;
}

double video_get_current_time(const video_t* v) {
	return v ? v->current_pts : 0.0;
}

bool video_is_live(const video_t* v) {
	return v ? v->is_live : false;
}

bool video_is_seekable(const video_t* v) {
	return v ? v->is_seekable : false;
}

bool video_is_hw_accelerated(const video_t* v) {
	return v && v->valid; // Always hardware on Android (MediaCodec)
}

void video_abort_decode(video_t* v) {
	if (v) atomic_store(&v->abort_decode, true);
}

video_decode_status_ video_decode_next_frame(video_t* v) {
	if (!v || !v->valid) return video_decode_error;
	if (v->eof)          return video_decode_eof;

	atomic_store(&v->abort_decode, false);

	// Deferred flush from seek
	if (v->needs_flush) {
		AMediaCodec_flush(v->codec);
		v->needs_flush = false;
	}

	// Feed packets and drain decoded frames
	for (;;) {
		if (atomic_load(&v->abort_decode)) return video_decode_aborted;

		// Try to dequeue an output buffer
		AMediaCodecBufferInfo buf_info;
		ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(v->codec, &buf_info, 0);

		if (out_idx >= 0) {
			// Got a decoded frame — render it to the AImageReader surface
			v->current_pts = (double)buf_info.presentationTimeUs / 1000000.0;

			AMediaCodec_releaseOutputBuffer(v->codec, (size_t)out_idx, true);

			// Release the oldest retained image (returns its AHB to the pool).
			// The ring ensures we keep VIDEO_AHB_RETAIN_COUNT images alive so
			// the GPU pipeline (3 frames in flight) never reads a recycled AHB.
			if (v->retained_images[v->retain_idx]) {
				AImage_delete(v->retained_images[v->retain_idx]);
			}
			v->retained_images[v->retain_idx] = v->current_image;
			v->retain_idx = (v->retain_idx + 1) % VIDEO_AHB_RETAIN_COUNT;
			v->current_image = NULL;

			// Acquire the latest image from the reader
			AImage* image = NULL;
			media_status_t st = AImageReader_acquireLatestImage(v->image_reader, &image);
			if (st != AMEDIA_OK || !image) {
				printf("[video] AImageReader_acquireLatestImage failed: %s\n", _media_status_str(st));
				return video_decode_error;
			}
			v->current_image = image;

			// Get the AHardwareBuffer backing this image
			AHardwareBuffer* ahb = NULL;
			st = AImage_getHardwareBuffer(image, &ahb);
			if (st != AMEDIA_OK || !ahb) {
				printf("[video] AImage_getHardwareBuffer failed: %s\n", _media_status_str(st));
				return video_decode_error;
			}

			// Store pending AHB for main thread import (avoids racing with
			// the render thread — material/pipeline changes must happen on
			// the same thread that renders).
			v->pending_ahb = ahb;
			atomic_store(&v->has_pending_frame, true);

			if (buf_info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
				v->eof = true;
			}

			return video_decode_ok;

		} else if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			// Update display dimensions from the crop rect if available.
			// Do NOT fall back to WIDTH/HEIGHT — the output format reports
			// coded/aligned dimensions (e.g., 320x192 for a 320x180 video)
			// which would break UV crop. The track format dimensions from
			// video_open() are correct as the default.
			AMediaFormat* new_fmt = AMediaCodec_getOutputFormat(v->codec);
			if (new_fmt) {
				int32_t cl = 0, ct = 0, cr = 0, cb = 0;
				if (AMediaFormat_getInt32(new_fmt, "crop-left",   &cl) &&
				    AMediaFormat_getInt32(new_fmt, "crop-right",  &cr) &&
				    AMediaFormat_getInt32(new_fmt, "crop-top",    &ct) &&
				    AMediaFormat_getInt32(new_fmt, "crop-bottom", &cb)) {
					v->width  = cr - cl + 1;
					v->height = cb - ct + 1;
				}
				AMediaFormat_delete(new_fmt);
			}
			continue;

		} else if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
			// No output yet, need to feed more input
		} else {
			// Other info change, just continue
			continue;
		}

		// Feed input packets from the extractor
		ssize_t in_idx = AMediaCodec_dequeueInputBuffer(v->codec, 10000); // 10ms timeout
		if (in_idx < 0) continue;

		size_t in_buf_size = 0;
		uint8_t* in_buf = AMediaCodec_getInputBuffer(v->codec, (size_t)in_idx, &in_buf_size);
		if (!in_buf) {
			AMediaCodec_queueInputBuffer(v->codec, (size_t)in_idx, 0, 0, 0, 0);
			continue;
		}

		ssize_t sample_size = AMediaExtractor_readSampleData(v->extractor, in_buf, in_buf_size);
		if (sample_size < 0) {
			// End of stream — send EOS to codec
			AMediaCodec_queueInputBuffer(v->codec, (size_t)in_idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
			v->eof = true;
			continue;
		}

		int64_t pts = AMediaExtractor_getSampleTime(v->extractor);
		uint32_t flags = 0;
		AMediaExtractor_advance(v->extractor);

		AMediaCodec_queueInputBuffer(v->codec, (size_t)in_idx, 0, (size_t)sample_size, (uint64_t)pts, flags);
	}
}

bool video_seek(video_t* v, double time_seconds) {
	if (!v || !v->valid || !v->is_seekable) return false;

	media_status_t st = AMediaExtractor_seekTo(
		v->extractor,
		(int64_t)(time_seconds * 1000000.0),
		AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC
	);
	if (st != AMEDIA_OK) return false;

	v->needs_flush = true;
	v->eof         = false;
	v->current_pts = time_seconds;

	return true;
}

skr_material_t* video_get_material(video_t* v) {
	if (!v) return NULL;

	// Import pending AHB frame on the main/render thread. This avoids racing
	// with the render pipeline — changing the YCbCr immutable sampler triggers
	// descriptor set layout re-registration, which must not happen while a
	// draw is in flight.
	if (atomic_load(&v->has_pending_frame)) {
		AHardwareBuffer* ahb = v->pending_ahb;
		atomic_store(&v->has_pending_frame, false);

		if (_import_ahb_frame(v, ahb)) {
			v->material_ready = true;
		}
	}

	if (!v->material_ready) return NULL;
	return &v->material;
}

skr_tex_t video_extract_thumbnail(const char* filename, int32_t max_size) {
	(void)filename;
	(void)max_size;
	// Not available on Android without FFmpeg
	return (skr_tex_t){0};
}
