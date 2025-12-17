// SPDX-License-Identifier: MIT
// Video playback module using FFmpeg with Vulkan hardware acceleration

#include "video.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <vulkan/vulkan.h>

#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Vulkan video decode capability check
///////////////////////////////////////////////////////////////////////////////

// Cached video decode capability info
typedef struct {
	bool    checked;
	bool    available;
	char    device_name[256];
	uint8_t device_uuid[VK_UUID_SIZE];
} _vk_video_info_t;

static _vk_video_info_t _vk_video_info = {0};

// Required extensions for FFmpeg Vulkan video decode
static const char* _vk_video_required_extensions[] = {
	"VK_KHR_video_queue",
	"VK_KHR_video_decode_queue",
	"VK_KHR_video_decode_h264",  // At minimum need H264 support
};
static const uint32_t _vk_video_required_extension_count = sizeof(_vk_video_required_extensions) / sizeof(_vk_video_required_extensions[0]);

// Pure function: checks if any Vulkan device supports video decode
// Returns true if found, and writes device name/UUID to output parameters
static bool _find_vulkan_video_device(char* out_device_name, size_t name_size, uint8_t* out_device_uuid) {
	// Create minimal Vulkan instance to query devices
	VkApplicationInfo app_info = {
		.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName   = "video_decode_check",
		.applicationVersion = 1,
		.pEngineName        = "sk_renderer",
		.engineVersion      = 1,
		.apiVersion         = VK_API_VERSION_1_3,  // Video extensions require 1.3
	};

	VkInstanceCreateInfo instance_info = {
		.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app_info,
	};

	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) {
		fprintf(stderr, "[video] Failed to create Vulkan instance for capability check\n");
		return false;
	}

	// Enumerate physical devices
	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(instance, &device_count, NULL);
	if (device_count == 0) {
		vkDestroyInstance(instance, NULL);
		return false;
	}

	VkPhysicalDevice* devices = malloc(sizeof(VkPhysicalDevice) * device_count);
	vkEnumeratePhysicalDevices(instance, &device_count, devices);

	// Check each device for ALL required video decode extensions
	for (uint32_t i = 0; i < device_count; i++) {
		uint32_t ext_count = 0;
		vkEnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, NULL);

		if (ext_count == 0) continue;

		VkExtensionProperties* extensions = malloc(sizeof(VkExtensionProperties) * ext_count);
		vkEnumerateDeviceExtensionProperties(devices[i], NULL, &ext_count, extensions);

		// Check if all required extensions are present
		uint32_t found_count = 0;
		for (uint32_t r = 0; r < _vk_video_required_extension_count; r++) {
			for (uint32_t j = 0; j < ext_count; j++) {
				if (strcmp(extensions[j].extensionName, _vk_video_required_extensions[r]) == 0) {
					found_count++;
					break;
				}
			}
		}

		free(extensions);

		if (found_count == _vk_video_required_extension_count) {
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(devices[i], &props);

			// Output device name
			if (out_device_name && name_size > 0) {
				strncpy(out_device_name, props.deviceName, name_size - 1);
				out_device_name[name_size - 1] = '\0';
			}

			// Output device UUID
			if (out_device_uuid) {
				VkPhysicalDeviceIDProperties id_props = {
					.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
				};
				VkPhysicalDeviceProperties2 props2 = {
					.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
					.pNext = &id_props,
				};
				vkGetPhysicalDeviceProperties2(devices[i], &props2);
				memcpy(out_device_uuid, id_props.deviceUUID, VK_UUID_SIZE);
			}

			free(devices);
			vkDestroyInstance(instance, NULL);
			return true;
		}
	}

	free(devices);
	vkDestroyInstance(instance, NULL);

	fprintf(stderr, "[video] No Vulkan device with full video decode support found, using software decode\n");
	return false;
}

///////////////////////////////////////////////////////////////////////////////
// Internal structures
///////////////////////////////////////////////////////////////////////////////

struct video_t {
	// FFmpeg state
	AVFormatContext*   format_ctx;
	AVCodecContext*    codec_ctx;
	AVBufferRef*       hw_device_ctx;   // Vulkan hardware device context
	AVFrame*           frame;           // Current decoded frame
	AVFrame*           sw_frame;        // Software frame (for fallback or transfer)
	AVPacket*          packet;
	int32_t            stream_idx;
	double             time_base;
	double             current_pts;
	double             duration;
	double             framerate;

	// Texture wrappers for decoded frame planes
	skr_tex_t          tex_y;           // Y plane (R8)
	skr_tex_t          tex_uv;          // UV plane (RG8)

	// Software decode fallback textures
	skr_tex_t          sw_tex_y;
	skr_tex_t          sw_tex_uv;
	bool               using_sw_textures;

	// State
	int32_t            width;
	int32_t            height;
	bool               hw_accel;
	bool               same_device;     // True if FFmpeg and sk_renderer use the same GPU (zero-copy possible)
	bool               valid;
	bool               eof;
	bool               is_live;         // True for live streams (no duration/seek)
	bool               is_seekable;     // True if seeking is supported
};

///////////////////////////////////////////////////////////////////////////////
// Hardware context setup
///////////////////////////////////////////////////////////////////////////////

static enum AVPixelFormat _get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
	for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_VULKAN) {
			return *p;
		}
	}
	// Fallback to software format
	return AV_PIX_FMT_YUV420P;
}

static bool _init_vulkan_hwcontext(video_t* v) {
	// Check if any Vulkan device supports video decode before attempting
	// to create FFmpeg's Vulkan context. This avoids FFmpeg error spam
	// about missing VK_KHR_video_decode_queue when it's not supported.
	if (!_vk_video_info.checked) {
		_vk_video_info.available = _find_vulkan_video_device(_vk_video_info.device_name, sizeof(_vk_video_info.device_name), _vk_video_info.device_uuid);
		_vk_video_info.checked = true;
	}
	if (!_vk_video_info.available) return false;

	// Check if video-capable device matches sk_renderer's device
	uint8_t skr_uuid[VK_UUID_SIZE];
	skr_get_vk_device_uuid(skr_uuid);
	v->same_device = memcmp(_vk_video_info.device_uuid, skr_uuid, VK_UUID_SIZE) == 0;

	// NOTE: Zero-copy VkDevice sharing requires additional work:
	//
	// Sharing sk_renderer's VkDevice with FFmpeg for true zero-copy would require:
	// 1. sk_renderer enabling FFmpeg's required extensions during device creation:
	//    - VK_KHR_synchronization2 (required by video queue extensions)
	//    - VK_KHR_external_memory_fd
	//    - VK_EXT_external_memory_dma_buf
	//    - VK_EXT_image_drm_format_modifier
	//    - VK_KHR_external_semaphore_fd
	//    - VK_EXT_external_memory_host
	// 2. Setting up the qf[] array with proper queue family info
	// 3. Providing lock_queue/unlock_queue callbacks for thread safety
	// 4. Using skr_tex_create_external() to wrap FFmpeg's AVVkFrame VkImages
	//
	// For now, we let FFmpeg create its own Vulkan device. This still provides
	// hardware-accelerated decode - only the final transfer to sk_renderer uses CPU.
	// The performance impact is minimal for typical video resolutions.
	(void)v->same_device;  // Tracked but not used for zero-copy yet

	// Let FFmpeg create its own Vulkan device
	int ret = av_hwdevice_ctx_create(&v->hw_device_ctx, AV_HWDEVICE_TYPE_VULKAN, _vk_video_info.device_name, NULL, 0);
	if (ret < 0) {
		char err_buf[256];
		av_strerror(ret, err_buf, sizeof(err_buf));
		fprintf(stderr, "[video] Failed to create Vulkan hw context: %s\n", err_buf);
		return false;
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// Texture creation helpers
///////////////////////////////////////////////////////////////////////////////

static void _create_software_textures(video_t* v) {
	if (v->using_sw_textures) return;

	skr_tex_sampler_t sampler = {
		.sample  = skr_tex_sample_linear,
		.address = skr_tex_address_clamp,
	};

	// Y plane: full resolution, R8
	skr_tex_create(
		skr_tex_fmt_r8,
		skr_tex_flags_dynamic,
		sampler,
		(skr_vec3i_t){v->width, v->height, 1},
		1, 1, NULL, &v->sw_tex_y);
	skr_tex_set_name(&v->sw_tex_y, "video_y");

	// UV plane: half resolution, RG8
	skr_tex_create(
		skr_tex_fmt_r8g8,
		skr_tex_flags_dynamic,
		sampler,
		(skr_vec3i_t){v->width / 2, v->height / 2, 1},
		1, 1, NULL, &v->sw_tex_uv);
	skr_tex_set_name(&v->sw_tex_uv, "video_uv");

	v->using_sw_textures = true;
}

static void _upload_software_frame(video_t* v, AVFrame* frame) {
	if (!v->using_sw_textures) {
		_create_software_textures(v);
	}

	int32_t uv_width  = v->width / 2;
	int32_t uv_height = v->height / 2;

	// Handle different pixel formats
	if (frame->format == AV_PIX_FMT_NV12) {
		// NV12: Y plane (full res) + interleaved UV plane (half res)
		skr_tex_set_data(&v->sw_tex_y,  &(skr_tex_data_t){.data = frame->data[0], .mip_count = 1, .layer_count = 1, .row_pitch = frame->linesize[0]});
		skr_tex_set_data(&v->sw_tex_uv, &(skr_tex_data_t){.data = frame->data[1], .mip_count = 1, .layer_count = 1, .row_pitch = frame->linesize[1]});

	} else if (frame->format == AV_PIX_FMT_YUV420P) {
		// Planar YUV420: Y + U + V separate planes
		skr_tex_set_data(&v->sw_tex_y, &(skr_tex_data_t){.data = frame->data[0], .mip_count = 1, .layer_count = 1, .row_pitch = frame->linesize[0]});

		// Interleave U and V into UV texture (RG8 format)
		uint8_t* uv_buffer = malloc(uv_width * uv_height * 2);
		if (uv_buffer) {
			uint8_t* u_plane = frame->data[1];
			uint8_t* v_plane = frame->data[2];
			int32_t  u_pitch = frame->linesize[1];
			int32_t  v_pitch = frame->linesize[2];

			for (int32_t y = 0; y < uv_height; y++) {
				uint8_t* dst = uv_buffer + y * uv_width * 2;
				uint8_t* u_row = u_plane + y * u_pitch;
				uint8_t* v_row = v_plane + y * v_pitch;
				for (int32_t x = 0; x < uv_width; x++) {
					dst[x * 2 + 0] = u_row[x];  // U -> R
					dst[x * 2 + 1] = v_row[x];  // V -> G
				}
			}
			skr_tex_set_data(&v->sw_tex_uv, &(skr_tex_data_t){.data = uv_buffer, .mip_count = 1, .layer_count = 1});
			free(uv_buffer);
		}
	} else {
		fprintf(stderr, "[video] Unsupported pixel format: %d\n", frame->format);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Public API
///////////////////////////////////////////////////////////////////////////////

video_t* video_open(const char* uri) {
	video_t* v = calloc(1, sizeof(video_t));
	if (!v) return NULL;

	// Set up network options for URLs
	AVDictionary* opts = NULL;
	bool is_url = strstr(uri, "://") != NULL;
	if (is_url) {
		av_dict_set(&opts, "timeout",             "5000000", 0);  // 5 second timeout (microseconds)
		av_dict_set(&opts, "reconnect",           "1",       0);  // Auto-reconnect on disconnect
		av_dict_set(&opts, "reconnect_streamed",  "1",       0);  // Reconnect even if we started streaming
		av_dict_set(&opts, "reconnect_delay_max", "5",       0);  // Max 5 seconds between reconnect attempts
	}

	// Open input file/URL
	int ret = avformat_open_input(&v->format_ctx, uri, NULL, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		fprintf(stderr, "[video] Failed to open: %s\n", uri);
		free(v);
		return NULL;
	}

	// Find stream info
	ret = avformat_find_stream_info(v->format_ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "[video] Failed to find stream info\n");
		avformat_close_input(&v->format_ctx);
		free(v);
		return NULL;
	}

	// Find best video stream
	v->stream_idx = av_find_best_stream(v->format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (v->stream_idx < 0) {
		fprintf(stderr, "[video] No video stream found\n");
		avformat_close_input(&v->format_ctx);
		free(v);
		return NULL;
	}

	AVStream* stream = v->format_ctx->streams[v->stream_idx];
	v->time_base = av_q2d(stream->time_base);
	v->framerate = av_q2d(stream->avg_frame_rate);

	// Handle duration - live streams have unknown/infinite duration
	if (v->format_ctx->duration == AV_NOPTS_VALUE || v->format_ctx->duration <= 0) {
		v->duration = 0.0;
		v->is_live  = true;
	} else {
		v->duration = (double)v->format_ctx->duration / AV_TIME_BASE;
		v->is_live  = false;
	}

	// Check if seeking is supported (requires seekable I/O context)
	v->is_seekable = !v->is_live && v->format_ctx->pb && (v->format_ctx->pb->seekable & AVIO_SEEKABLE_NORMAL);

	// Find decoder
	const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		fprintf(stderr, "[video] No decoder found for codec\n");
		avformat_close_input(&v->format_ctx);
		free(v);
		return NULL;
	}

	// Allocate codec context
	v->codec_ctx = avcodec_alloc_context3(codec);
	if (!v->codec_ctx) {
		fprintf(stderr, "[video] Failed to allocate codec context\n");
		avformat_close_input(&v->format_ctx);
		free(v);
		return NULL;
	}

	// Copy codec parameters
	ret = avcodec_parameters_to_context(v->codec_ctx, stream->codecpar);
	if (ret < 0) {
		fprintf(stderr, "[video] Failed to copy codec parameters\n");
		avcodec_free_context(&v->codec_ctx);
		avformat_close_input(&v->format_ctx);
		free(v);
		return NULL;
	}

	v->width  = v->codec_ctx->width;
	v->height = v->codec_ctx->height;

	// Try to set up Vulkan hardware acceleration
	v->hw_accel = _init_vulkan_hwcontext(v);
	if (v->hw_accel) {
		v->codec_ctx->hw_device_ctx = av_buffer_ref(v->hw_device_ctx);
		v->codec_ctx->get_format    = _get_hw_format;
	}

	// Open codec
	ret = avcodec_open2(v->codec_ctx, codec, NULL);
	if (ret < 0) {
		fprintf(stderr, "[video] Failed to open codec: %d\n", ret);
		if (v->hw_device_ctx) av_buffer_unref(&v->hw_device_ctx);
		avcodec_free_context(&v->codec_ctx);
		avformat_close_input(&v->format_ctx);
		free(v);
		return NULL;
	}

	// Allocate frames and packet
	v->frame    = av_frame_alloc();
	v->sw_frame = av_frame_alloc();
	v->packet   = av_packet_alloc();

	if (!v->frame || !v->sw_frame || !v->packet) {
		fprintf(stderr, "[video] Failed to allocate frames/packet\n");
		video_destroy(v);
		return NULL;
	}

	// Create software textures for CPU transfer
	_create_software_textures(v);

	v->valid = true;

	// Decode first frame to avoid green flash on first render
	// (uninitialized UV textures produce green in YUV->RGB conversion)
	// For network streams, this may block briefly but it's better than green flash.
	video_decode_next_frame(v);

	return v;
}

void video_destroy(video_t* v) {
	if (!v) return;

	// Destroy textures
	skr_tex_destroy(&v->tex_y);
	skr_tex_destroy(&v->tex_uv);
	skr_tex_destroy(&v->sw_tex_y);
	skr_tex_destroy(&v->sw_tex_uv);

	// Free FFmpeg resources
	if (v->packet)        av_packet_free      (&v->packet);
	if (v->sw_frame)      av_frame_free       (&v->sw_frame);
	if (v->frame)         av_frame_free       (&v->frame);
	if (v->hw_device_ctx) av_buffer_unref     (&v->hw_device_ctx);
	if (v->codec_ctx)     avcodec_free_context(&v->codec_ctx);
	if (v->format_ctx)    avformat_close_input(&v->format_ctx);

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

bool video_decode_next_frame(video_t* v) {
	if (!v || !v->valid || v->eof) return false;

	while (true) {
		// Try to receive a decoded frame
		int ret = avcodec_receive_frame(v->codec_ctx, v->frame);
		if (ret == 0) {
			// Got a frame
			v->current_pts = v->frame->pts * v->time_base;

			// Handle hardware vs software frame
			if (v->frame->format == AV_PIX_FMT_VULKAN && v->hw_accel) {
				// Hardware decoded frame on FFmpeg's Vulkan device
				AVVkFrame* vk_frame = (AVVkFrame*)v->frame->data[0];
				if (vk_frame) {
					// TODO: OPTIMIZATION - External Memory Sharing
					// Currently FFmpeg uses its own VkDevice (with video extensions),
					// so we can't directly use vk_frame->img as sk_renderer textures.
					// Future optimization: Use VK_KHR_external_memory_fd to share
					// memory between FFmpeg's device and sk_renderer's device,
					// enabling true zero-copy hardware decode.
					//
					// For now, transfer through CPU (still benefits from hw decode):
					int ret = av_hwframe_transfer_data(v->sw_frame, v->frame, 0);
					if (ret >= 0) {
						_upload_software_frame(v, v->sw_frame);
					} else {
						fprintf(stderr, "[video] Failed to transfer hw frame to CPU\n");
					}

					// The code below is for future use when external memory sharing
					// is implemented - it would wrap FFmpeg's VkImages directly:
					#if 0 // Enable when using shared VkDevice or external memory
					skr_tex_sampler_t sampler = {
						.sample  = skr_tex_sample_linear,
						.address = skr_tex_address_clamp,
					};

					// Y plane
					if (!skr_tex_is_valid(&v->tex_y)) {
						skr_tex_create_external((skr_tex_external_info_t){
							.image          = vk_frame->img[0],
							.view           = VK_NULL_HANDLE,
							.memory         = VK_NULL_HANDLE,
							.format         = skr_tex_fmt_r8,
							.size           = {v->width, v->height, 1},
							.current_layout = vk_frame->layout[0],
							.sampler        = sampler,
							.owns_image     = false,
						}, &v->tex_y);
					} else {
						skr_tex_update_external(&v->tex_y, (skr_tex_external_update_t){
							.image          = vk_frame->img[0],
							.view           = VK_NULL_HANDLE,
							.current_layout = vk_frame->layout[0],
						});
					}

					// UV plane (NV12 has interleaved UV in second plane)
					if (!skr_tex_is_valid(&v->tex_uv)) {
						skr_tex_create_external((skr_tex_external_info_t){
							.image          = vk_frame->img[1],
							.view           = VK_NULL_HANDLE,
							.memory         = VK_NULL_HANDLE,
							.format         = skr_tex_fmt_r8g8,
							.size           = {v->width / 2, v->height / 2, 1},
							.current_layout = vk_frame->layout[1],
							.sampler        = sampler,
							.owns_image     = false,
						}, &v->tex_uv);
					} else {
						skr_tex_update_external(&v->tex_uv, (skr_tex_external_update_t){
							.image          = vk_frame->img[1],
							.view           = VK_NULL_HANDLE,
							.current_layout = vk_frame->layout[1],
						});
					}
					#endif
				}
			} else {
				// Software decoded frame - upload YUV data to GPU textures
				_upload_software_frame(v, v->frame);
			}

			return true;
		} else if (ret == AVERROR(EAGAIN)) {
			// Need more input
		} else if (ret == AVERROR_EOF) {
			v->eof = true;
			return false;
		} else {
			// Error
			return false;
		}

		// Read next packet
		ret = av_read_frame(v->format_ctx, v->packet);
		if (ret < 0) {
			if (ret == AVERROR_EOF) {
				// Send flush packet to decoder
				avcodec_send_packet(v->codec_ctx, NULL);
				v->eof = true;
			}
			continue;
		}

		// Skip non-video packets
		if (v->packet->stream_index != v->stream_idx) {
			av_packet_unref(v->packet);
			continue;
		}

		// Send packet to decoder
		ret = avcodec_send_packet(v->codec_ctx, v->packet);
		av_packet_unref(v->packet);

		if (ret < 0 && ret != AVERROR(EAGAIN)) {
			return false;
		}
	}
}

bool video_seek(video_t* v, double time_seconds) {
	if (!v || !v->valid || !v->is_seekable) return false;

	int64_t timestamp = (int64_t)(time_seconds * AV_TIME_BASE);
	int ret = av_seek_frame(v->format_ctx, -1, timestamp, AVSEEK_FLAG_BACKWARD);
	if (ret < 0) return false;

	// Flush decoder
	avcodec_flush_buffers(v->codec_ctx);
	v->eof = false;

	// Update current_pts to reflect seek position so the playback loop
	// knows where we are. The actual frame will be decoded in update loop.
	v->current_pts = time_seconds;

	return true;
}

skr_tex_t* video_get_tex_y(video_t* v) {
	if (!v) return NULL;
	if (v->hw_accel && skr_tex_is_valid(&v->tex_y)) {
		return &v->tex_y;
	}
	return &v->sw_tex_y;
}

skr_tex_t* video_get_tex_uv(video_t* v) {
	if (!v) return NULL;
	if (v->hw_accel && skr_tex_is_valid(&v->tex_uv)) {
		return &v->tex_uv;
	}
	return &v->sw_tex_uv;
}

bool video_is_hw_accelerated(const video_t* v) {
	return v && v->hw_accel;
}

skr_tex_t video_extract_thumbnail(const char* filename, int32_t max_size) {
	skr_tex_t result = {0};

	// Open video temporarily
	video_t* v = video_open(filename);
	if (!v) return result;

	// Decode first frame
	if (!video_decode_next_frame(v)) {
		video_destroy(v);
		return result;
	}

	// For now, return invalid texture
	// Full implementation would convert frame to RGBA and create texture
	// TODO: Implement thumbnail extraction with swscale

	video_destroy(v);
	return result;
}
