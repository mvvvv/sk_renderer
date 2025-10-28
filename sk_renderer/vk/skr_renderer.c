// SPDX-License-Identifier: MIT
// The authors below grant copyright rights under the MIT license:
// Copyright (c) 2025 Nick Klingensmith
// Copyright (c) 2025 Qualcomm Technologies, Inc.

#include "_sk_renderer.h"
#include "skr_pipeline.h"
#include "skr_conversions.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////
// Constants
///////////////////////////////////////////////////////////////////////////////

// Query pool has 2 queries per frame (start/end timestamps)
#define SKR_QUERIES_PER_FRAME 2

// Maximum global buffer/texture binding slots
#define SKR_MAX_GLOBAL_BINDINGS 16

///////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////

static VkFramebuffer _skr_get_or_create_framebuffer(skr_tex_t* cache_target, VkRenderPass render_pass,
                                                     skr_tex_t* color, skr_tex_t* depth, skr_tex_t* opt_resolve,
                                                     bool has_depth) {
	VkFramebuffer* cached_fb = has_depth ? &cache_target->framebuffer_depth : &cache_target->framebuffer;

	// Check if we have a cached framebuffer for this render pass
	if (*cached_fb != VK_NULL_HANDLE && cache_target->framebuffer_pass == render_pass) {
		return *cached_fb;
	}

	// Destroy old cached framebuffer if render pass changed
	if (*cached_fb != VK_NULL_HANDLE) {
		vkDestroyFramebuffer(_skr_vk.device, *cached_fb, NULL);
	}

	// Create and cache new framebuffer
	*cached_fb = _skr_create_framebuffer(render_pass, color, depth, opt_resolve);
	cache_target->framebuffer_pass = render_pass;
	return *cached_fb;
}

static void _skr_ensure_buffer(skr_buffer_t* buffer, bool* ref_valid, const void* data, size_t size, skr_buffer_type_ type, const char* name) {
	bool needs_recreate = !*ref_valid || buffer->size < size;

	if (needs_recreate) {
		// Destroy old buffer if it exists
		if (*ref_valid) {
			skr_buffer_destroy(buffer);
		}
		// Create new buffer with required size
		*buffer = skr_buffer_create(data, size, 1, type, skr_use_dynamic);
		skr_buffer_set_name(buffer, name);
		*ref_valid = true;
	} else {
		// Buffer is valid and large enough, just update contents
		skr_buffer_set(buffer, data, size);
	}
}

///////////////////////////////////////////////////////////////////////////////
// Deferred Texture Transition System
///////////////////////////////////////////////////////////////////////////////

// Queue a texture for transition (will be flushed before next render pass)
void _skr_tex_transition_enqueue(skr_tex_t* tex, uint8_t type) {
	if (!tex || !tex->image) return;

	// Check if already queued (avoid duplicates)
	for (uint32_t i = 0; i < _skr_vk.pending_transition_count; i++) {
		if (_skr_vk.pending_transitions[i] == tex) {
			// Update type if needed (storage takes priority over shader_read)
			if (type > _skr_vk.pending_transition_types[i]) {
				_skr_vk.pending_transition_types[i] = type;
			}
			return;
		}
	}

	// Add to queue if space available
	if (_skr_vk.pending_transition_count < 64) {
		_skr_vk.pending_transitions[_skr_vk.pending_transition_count] = tex;
		_skr_vk.pending_transition_types[_skr_vk.pending_transition_count] = type;
		_skr_vk.pending_transition_count++;
	}
}

// Flush all pending texture transitions (called before render pass begins)
static void _skr_flush_texture_transitions(VkCommandBuffer cmd) {
	for (uint32_t i = 0; i < _skr_vk.pending_transition_count; i++) {
		skr_tex_t* tex  = _skr_vk.pending_transitions[i];
		uint8_t    type = _skr_vk.pending_transition_types[i];

		if (type == 1) {  // storage
			_skr_tex_transition_for_storage(cmd, tex);
		} else {  // shader_read
			_skr_tex_transition_for_shader_read(cmd, tex, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}
	}

	// Clear the queue
	_skr_vk.pending_transition_count = 0;
}

///////////////////////////////////////////////////////////////////////////////
// Rendering
///////////////////////////////////////////////////////////////////////////////

void skr_renderer_frame_begin() {
	_skr_vk.in_frame = true;

	// Start a command buffer batch for this frame
	VkCommandBuffer cmd = _skr_command_begin().cmd;

	// Reset and write start timestamp
	uint32_t query_start = _skr_vk.flight_idx * SKR_QUERIES_PER_FRAME;
	vkCmdResetQueryPool(cmd, _skr_vk.timestamp_pool, query_start, SKR_QUERIES_PER_FRAME);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, _skr_vk.timestamp_pool, query_start);
}

void skr_renderer_frame_end() {
	// Note: Command buffer is ended and submitted by skr_surface_present
	// If not using surfaces, user must call _skr_command_end_submit manually

	// Only read timestamps after we've completed a full ring buffer cycle
	if (_skr_vk.frame >= SKR_MAX_FRAMES_IN_FLIGHT) {
		// Retrieve timestamps from oldest completed frame (N-frames_in_flight ago)
		uint32_t prev_flight = (_skr_vk.flight_idx + 1) % SKR_MAX_FRAMES_IN_FLIGHT;
		uint32_t query_start = prev_flight * SKR_QUERIES_PER_FRAME;

		// Get timestamps (this reads from a completed frame due to ring buffering)
		VkResult result = vkGetQueryPoolResults(
			_skr_vk.device, _skr_vk.timestamp_pool, query_start, SKR_QUERIES_PER_FRAME,
			sizeof(uint64_t) * SKR_QUERIES_PER_FRAME, _skr_vk.frame_timestamps[prev_flight],
			sizeof(uint64_t), VK_QUERY_RESULT_64_BIT
		);
		_skr_vk.timestamps_valid[prev_flight] = (result == VK_SUCCESS);
	}

	_skr_vk.in_frame = false;

	// Increment frame counter and advance flight index
	_skr_vk.frame++;
	_skr_vk.flight_idx = _skr_vk.frame % SKR_MAX_FRAMES_IN_FLIGHT;
}

void skr_renderer_begin_pass(skr_tex_t* color, skr_tex_t* depth, skr_tex_t* opt_resolve, skr_clear_ clear, skr_vec4_t clear_color, float clear_depth, uint32_t clear_stencil) {
	// Require at least one attachment (color or depth)
	if (!color && !depth) return;

	VkCommandBuffer cmd = _skr_command_acquire().cmd;

	// Flush all pending texture transitions BEFORE starting render pass
	// This prevents barriers inside render pass which require self-dependencies
	_skr_flush_texture_transitions(cmd);

	// Register render pass format with pipeline system
	skr_pipeline_renderpass_key_t rp_key = {
		.color_format    = color                                           ? _skr_to_vk_tex_fmt(color->format)         : VK_FORMAT_UNDEFINED,
		.depth_format    = depth                                           ? _skr_to_vk_tex_fmt(depth->format)         : VK_FORMAT_UNDEFINED,
		.resolve_format  = (opt_resolve && color && color->samples > VK_SAMPLE_COUNT_1_BIT) ? _skr_to_vk_tex_fmt(opt_resolve->format) : VK_FORMAT_UNDEFINED,
		.samples         = color ? color->samples : (depth ? depth->samples : VK_SAMPLE_COUNT_1_BIT),
		.depth_store_op  = (depth && (depth->flags & skr_tex_flags_readable)) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.color_load_op   = VK_ATTACHMENT_LOAD_OP_CLEAR,  // Always clear for main render pass
	};
	_skr_vk.current_renderpass_idx = _skr_pipeline_register_renderpass(&rp_key);

	// Get render pass from pipeline system
	VkRenderPass render_pass = _skr_pipeline_get_renderpass(_skr_vk.current_renderpass_idx);
	if (render_pass == VK_NULL_HANDLE) return;

	// Determine which texture to use for framebuffer caching
	// Priority: resolve target (for MSAA) > color > depth
	skr_tex_t* fb_cache_target = color;
	if (opt_resolve && rp_key.samples > VK_SAMPLE_COUNT_1_BIT) {
		fb_cache_target = opt_resolve;  // Use resolve target for MSAA
	} else if (!color) {
		fb_cache_target = depth;  // Depth-only pass
	}

	// Get or create cached framebuffer
	VkFramebuffer framebuffer = _skr_get_or_create_framebuffer(fb_cache_target, render_pass, color, depth, opt_resolve, depth != NULL);

	if (framebuffer == VK_NULL_HANDLE) return;

	// Transition depth texture to attachment layout if needed
	// Automatic system handles the optimization:
	// - Non-readable depth (transient_discard=true): Uses UNDEFINED oldLayout (tile GPU optimization)
	// - Readable depth: Properly tracks previous layout
	if (depth && (depth->flags & skr_tex_flags_writeable)) {
		_skr_tex_transition(cmd, depth,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
	}

	// Note: Color attachments use render pass implicit transitions (initialLayout/finalLayout)
	// We'll notify the system after vkCmdBeginRenderPass about the layout change

	// Setup clear values
	// Need to match attachment count: [color], [resolve], [depth]
	VkClearValue clear_values[3];
	uint32_t     clear_value_count = 0;

	if (color) {
		if (clear & skr_clear_color) {
			clear_values[clear_value_count] = (VkClearValue){ .color = {.float32 = {clear_color.x, clear_color.y, clear_color.z, clear_color.w}} };
		}
		clear_value_count++; // Color attachment needs an entry

		if (opt_resolve && rp_key.samples > VK_SAMPLE_COUNT_1_BIT) {
			// Resolve has loadOp = DONT_CARE, but still needs an entry
			clear_value_count++;
		}
	}

	if (depth) {
		if (clear & (skr_clear_depth | skr_clear_stencil)) {
			clear_values[clear_value_count] = (VkClearValue){ .depthStencil = {.depth = clear_depth, .stencil = clear_stencil} };
		}
		clear_value_count++;
	}

	// Determine render area from whichever attachment is available
	uint32_t render_width  = color ? color->size.x : depth->size.x;
	uint32_t render_height = color ? color->size.y : depth->size.y;

	// Begin render pass
	vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
		.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass      = render_pass,
		.framebuffer     = framebuffer,
		.clearValueCount = clear_value_count,
		.pClearValues    = clear_values,
		.renderArea      = {
			.extent = {render_width, render_height}
		},
	}, VK_SUBPASS_CONTENTS_INLINE);

	// Notify automatic system about render pass implicit layout transitions
	// Render pass transitions color to COLOR_ATTACHMENT_OPTIMAL
	if (color) {
		_skr_tex_transition_notify_layout(color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}
	// Resolve target (if used) goes to COLOR_ATTACHMENT_OPTIMAL
	if (opt_resolve && rp_key.samples > VK_SAMPLE_COUNT_1_BIT) {
		_skr_tex_transition_notify_layout(opt_resolve, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}
	// Depth remains in DEPTH_STENCIL_ATTACHMENT_OPTIMAL (render pass preserves it)
	if (depth) {
		_skr_tex_transition_notify_layout(depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	// Store current textures for end_pass layout transitions
	_skr_vk.current_color_texture = color;
	_skr_vk.current_depth_texture = depth;

	_skr_command_release(cmd);
}

void skr_renderer_end_pass() {
	VkCommandBuffer cmd = _skr_command_acquire().cmd;
	vkCmdEndRenderPass(cmd);

	// Transition readable color attachments to shader-read layout for next use
	// Automatic system handles this - tracks that color is currently in COLOR_ATTACHMENT_OPTIMAL
	if (_skr_vk.current_color_texture && (_skr_vk.current_color_texture->flags & skr_tex_flags_readable)) {
		_skr_tex_transition_for_shader_read(cmd, _skr_vk.current_color_texture,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	}

	// Transition readable depth texture to shader-read layout for next use (e.g., shadow maps)
	// Automatic system handles this - tracks that depth is currently in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	if (_skr_vk.current_depth_texture && (_skr_vk.current_depth_texture->flags & skr_tex_flags_readable)) {
		_skr_tex_transition_for_shader_read(cmd, _skr_vk.current_depth_texture,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	}

	_skr_vk.current_color_texture = NULL;
	_skr_vk.current_depth_texture = NULL;
	_skr_command_release(cmd);
}

void skr_renderer_set_global_constants(int32_t bind, const skr_buffer_t* buffer) {
	if (bind < 0 || bind >= SKR_MAX_GLOBAL_BINDINGS) return;
	_skr_vk.global_buffers[bind] = (skr_buffer_t*)buffer;
}

void skr_renderer_set_global_texture(int32_t bind, const skr_tex_t* tex) {
	if (bind < 0 || bind >= SKR_MAX_GLOBAL_BINDINGS) return;
	_skr_vk.global_textures[bind] = (skr_tex_t*)tex;

	// Queue transition for this global texture (only if needed)
	// It will be flushed before the next render pass begins
	if (tex) {
		uint8_t type = (tex->flags & skr_tex_flags_compute) ? 1 : 0;  // storage : shader_read
		if (_skr_tex_needs_transition(tex, type)) {
			_skr_tex_transition_enqueue((skr_tex_t*)tex, type);
		}
	}
}

void skr_renderer_set_viewport(skr_rect_t viewport) {
	VkCommandBuffer cmd = _skr_command_acquire().cmd;
	vkCmdSetViewport(cmd, 0, 1, &(VkViewport){
		.x        = viewport.x,
		.y        = viewport.y,
		.width    = viewport.w,
		.height   = viewport.h,
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	});
	_skr_command_release(cmd);
}

void skr_renderer_set_scissor(skr_recti_t scissor) {
	VkCommandBuffer cmd = _skr_command_acquire().cmd;
	vkCmdSetScissor(cmd, 0, 1, &(VkRect2D){
		.offset = {scissor.x, scissor.y},
		.extent = {(uint32_t)scissor.w, (uint32_t)scissor.h},
	});
	_skr_command_release(cmd);
}

void skr_renderer_blit(skr_material_t* material, skr_tex_t* to, skr_recti_t bounds_px) {
	if (!material || !to) return;
	if (!skr_material_is_valid(material) || !skr_tex_is_valid(to)) return;

	// Determine if this is a cubemap, array, or regular 2D texture
	bool is_cubemap = (to->flags & skr_tex_flags_cubemap) != 0;
	bool is_array   = (to->flags & skr_tex_flags_array) != 0;
	uint32_t layer_count = to->layer_count;

	// Determine if this is a full-image blit or partial
	bool is_full_blit = (bounds_px.w <= 0 || bounds_px.h <= 0) ||
	                    (bounds_px.x == 0 && bounds_px.y == 0 &&
	                     bounds_px.w == to->size.x && bounds_px.h == to->size.y);

	_skr_command_context_t ctx = _skr_command_acquire();

	// Register render pass format with pipeline system
	// Use DONT_CARE for full blit (discard previous contents), LOAD for partial (preserve)
	skr_pipeline_renderpass_key_t rp_key = {
		.color_format   = _skr_to_vk_tex_fmt(to->format),
		.depth_format   = VK_FORMAT_UNDEFINED,
		.resolve_format = VK_FORMAT_UNDEFINED,
		.samples        = to->samples,
		.depth_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,  // No depth in blit
		.color_load_op  = is_full_blit ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD,
	};
	int32_t renderpass_idx = _skr_pipeline_register_renderpass(&rp_key);
	int32_t vert_idx = _skr_pipeline_register_vertformat((skr_vert_type_t){});

	// Get render pass from pipeline system
	VkRenderPass render_pass = _skr_pipeline_get_renderpass(renderpass_idx);
	if (render_pass == VK_NULL_HANDLE) {
		_skr_command_release(ctx.cmd);
		return;
	}

	// Build per-draw descriptor writes
	VkWriteDescriptorSet   writes      [32];
	VkDescriptorBufferInfo buffer_infos[16];
	VkDescriptorImageInfo  image_infos [16];
	uint32_t write_ct  = 0;
	uint32_t buffer_ct = 0;
	uint32_t image_ct  = 0;

	skr_buffer_t param_buffer;
	if (material->param_buffer_size > 0) {
		param_buffer = skr_buffer_create(material->param_buffer, 1, material->param_buffer_size, skr_buffer_type_constant, skr_use_static);

		buffer_infos[buffer_ct] = (VkDescriptorBufferInfo){
			.buffer = param_buffer.buffer,
			.range  = param_buffer.size,
		};
		writes[write_ct++] = (VkWriteDescriptorSet){
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstBinding      = SKR_BIND_SHIFT_BUFFER + SKR_BIND_MATERIAL,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo     = &buffer_infos[buffer_ct++],
		};
	}

	// Material texture and buffer binds
	const int32_t ignore_slots[] = { SKR_BIND_SHIFT_BUFFER + SKR_BIND_MATERIAL };
	_skr_material_add_writes(material->binds, material->bind_count, ignore_slots, sizeof(ignore_slots)/sizeof(ignore_slots[0]),
		writes,       sizeof(writes      )/sizeof(writes      [0]),
		buffer_infos, sizeof(buffer_infos)/sizeof(buffer_infos[0]),
		image_infos,  sizeof(image_infos )/sizeof(image_infos [0]),
		&write_ct, &buffer_ct, &image_ct);

	// Transition any source textures in material to shader-read layout
	const sksc_shader_meta_t* meta = material->info.shader->meta;
	for (int32_t i=0;i<meta->resource_count;i++) {
		skr_material_bind_t* res = &material->binds[meta->buffer_count + i];
		if (res->texture)
			_skr_tex_transition_for_shader_read(ctx.cmd, res->texture, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}

	// Transition target texture to color attachment layout
	// Automatic system handles this - will use UNDEFINED if first use or track previous layout
	_skr_tex_transition(ctx.cmd, to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

	// For cubemaps and array textures, we need to create separate image views
	// and framebuffers for each layer
	if (is_cubemap || is_array) {
		VkFormat vk_format = _skr_to_vk_tex_fmt(to->format);

		for (uint32_t layer = 0; layer < layer_count; layer++) {
			// Create image view for this specific layer
			VkImageView layer_view = VK_NULL_HANDLE;
			{
				VkImageViewCreateInfo view_info = {
					.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					.image      = to->image,
					.viewType   = VK_IMAGE_VIEW_TYPE_2D,
					.format     = vk_format,
					.subresourceRange = {
						.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
						.baseMipLevel   = 0,
						.levelCount     = 1,
						.baseArrayLayer = layer,
						.layerCount     = 1,
					},
				};
				if (vkCreateImageView(_skr_vk.device, &view_info, NULL, &layer_view) != VK_SUCCESS) {
					continue;
				}
			}

			// Create framebuffer for this layer
			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			{
				VkFramebufferCreateInfo fb_info = {
					.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
					.renderPass      = render_pass,
					.attachmentCount = 1,
					.pAttachments    = &layer_view,
					.width           = bounds_px.w > 0 ? bounds_px.w : to->size.x,
					.height          = bounds_px.h > 0 ? bounds_px.h : to->size.y,
					.layers          = 1,
				};
				if (vkCreateFramebuffer(_skr_vk.device, &fb_info, NULL, &framebuffer) != VK_SUCCESS) {
					vkDestroyImageView(_skr_vk.device, layer_view, NULL);
					continue;
				}
			}

			// Begin render pass for this layer
			vkCmdBeginRenderPass(ctx.cmd, &(VkRenderPassBeginInfo){
				.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				.renderPass  = render_pass,
				.framebuffer = framebuffer,
				.renderArea  = {
					.offset = {bounds_px.x, bounds_px.y},
					.extent = {
						bounds_px.w > 0 ? bounds_px.w : to->size.x,
						bounds_px.h > 0 ? bounds_px.h : to->size.y
					}
				},
			}, VK_SUBPASS_CONTENTS_INLINE);

			// Get pipeline (we need a dummy vertex type since we're using SV_VertexID)
			VkPipeline pipeline = _skr_pipeline_get(material->pipeline_material_idx, renderpass_idx, vert_idx);
			if (pipeline != VK_NULL_HANDLE) {
				vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

				// Set viewport and scissor
				VkViewport viewport = {
					.x        = (float)bounds_px.x,
					.y        = (float)bounds_px.y,
					.width    = bounds_px.w > 0 ? (float)bounds_px.w : (float)to->size.x,
					.height   = bounds_px.h > 0 ? (float)bounds_px.h : (float)to->size.y,
					.minDepth = 0.0f,
					.maxDepth = 1.0f,
				};
				VkRect2D scissor = {
					.offset = {bounds_px.x, bounds_px.y},
					.extent = {
						bounds_px.w > 0 ? bounds_px.w : to->size.x,
						bounds_px.h > 0 ? bounds_px.h : to->size.y
					}
				};
				vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
				vkCmdSetScissor (ctx.cmd, 0, 1, &scissor);

				// Bind descriptors
				if (write_ct > 0) {
					vkCmdPushDescriptorSetKHR(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skr_pipeline_get_layout(material->pipeline_material_idx), 0, write_ct, writes);
				}

				// Draw fullscreen triangle (SV_VertexID generates positions)
				vkCmdDraw(ctx.cmd, 3, 1, 0, layer);
			}

			vkCmdEndRenderPass(ctx.cmd);

			// Queue per-layer resources for deferred destruction
			_skr_command_destroy_framebuffer(ctx.destroy_list, framebuffer);
			_skr_command_destroy_image_view (ctx.destroy_list, layer_view);
		}
	} else {
		// Regular 2D texture - use cached framebuffer
		VkFramebuffer framebuffer = _skr_get_or_create_framebuffer(to, render_pass, to, NULL, NULL, false);
		if (framebuffer == VK_NULL_HANDLE) {
			_skr_command_release(ctx.cmd);
			return;
		}

		// Begin render pass
		vkCmdBeginRenderPass(ctx.cmd, &(VkRenderPassBeginInfo){
			.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass  = render_pass,
			.framebuffer = framebuffer,
			.renderArea  = {
				.offset = {bounds_px.x, bounds_px.y},
				.extent = {
					bounds_px.w > 0 ? bounds_px.w : to->size.x,
					bounds_px.h > 0 ? bounds_px.h : to->size.y
				}
			},
		}, VK_SUBPASS_CONTENTS_INLINE);

		// Get pipeline
		VkPipeline pipeline = _skr_pipeline_get(material->pipeline_material_idx, renderpass_idx, 0);
		if (pipeline != VK_NULL_HANDLE) {
			vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

			// Set viewport and scissor
			VkViewport viewport = {
				.x        = (float)bounds_px.x,
				.y        = (float)bounds_px.y,
				.width    = bounds_px.w > 0 ? (float)bounds_px.w : (float)to->size.x,
				.height   = bounds_px.h > 0 ? (float)bounds_px.h : (float)to->size.y,
				.minDepth = 0.0f,
				.maxDepth = 1.0f,
			};
			VkRect2D scissor = {
				.offset = {bounds_px.x, bounds_px.y},
				.extent = {
					bounds_px.w > 0 ? bounds_px.w : to->size.x,
					bounds_px.h > 0 ? bounds_px.h : to->size.y
				}
			};
			vkCmdSetViewport(ctx.cmd, 0, 1, &viewport);
			vkCmdSetScissor (ctx.cmd, 0, 1, &scissor);

			// Bind descriptors
			if (write_ct > 0) {
				vkCmdPushDescriptorSetKHR(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skr_pipeline_get_layout(material->pipeline_material_idx), 0, write_ct, writes);
			}

			// Draw fullscreen triangle
			vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
		}

		vkCmdEndRenderPass(ctx.cmd);
	}

	// Transition target texture back to shader read layout
	// Automatic system tracks that it's currently in COLOR_ATTACHMENT_OPTIMAL
	_skr_tex_transition_for_shader_read(ctx.cmd, to, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	skr_buffer_destroy(&param_buffer);
	_skr_command_release(ctx.cmd);
}

void skr_renderer_draw(skr_render_list_t* list, const void* system_data, size_t system_data_size, int32_t instance_multiplier) {
	if (!list || list->count == 0) return;
	instance_multiplier = (instance_multiplier < 1) ? 1 : instance_multiplier;

	VkCommandBuffer cmd = _skr_command_acquire().cmd;

	_skr_render_list_sort(list);

	// This consolidates all material params into a single buffer
	list->material_data_used = 0;
	skr_material_t* prev_material = NULL;
	for (uint32_t i = 0; i < list->count; i++) {
		skr_material_t* material = list->items[i].material;
		if (material == prev_material) continue;

		// Resize material param data if needed
		while (list->material_data_used + material->param_buffer_size > list->material_data_capacity) {
			list->material_data_capacity = list->material_data_capacity * 2;
			list->material_data          = realloc(list->material_data, list->material_data_capacity);
		}

		memcpy(&list->material_data[list->material_data_used], material->param_buffer, material->param_buffer_size);
		list->material_data_used += material->param_buffer_size;

		prev_material = material;
	}

	// Upload data to our material and instance buffers
	if (system_data && system_data_size > 0) _skr_ensure_buffer(&list->system_buffer,         &list->system_buffer_valid,         system_data,         system_data_size,         skr_buffer_type_constant, "system_buffer");
	if (list->material_data_used        > 0) _skr_ensure_buffer(&list->material_param_buffer, &list->material_param_buffer_valid, list->material_data, list->material_data_used, skr_buffer_type_constant, "renderlist_material_params");
	if (list->instance_data_used        > 0) _skr_ensure_buffer(&list->instance_buffer,       &list->instance_buffer_valid,       list->instance_data, list->instance_data_used, skr_buffer_type_storage,  "renderlist_inst_data");

	// Draw items with batching
	VkPipeline bound_pipeline       = VK_NULL_HANDLE;
	uint32_t   material_data_offset = 0;
	prev_material = NULL;
	for (uint32_t i = 0; i < list->count; ) {
		const skr_render_item_t* item = &list->items[i];

		// Get pipeline from the cache
		VkPipeline pipeline = _skr_pipeline_get(item->material->pipeline_material_idx, _skr_vk.current_renderpass_idx, item->mesh->vert_type->pipeline_idx);
		assert(pipeline != VK_NULL_HANDLE && "Is the Vertex format out of scope?");

		// Find consecutive items with same mesh/material for batching
		uint32_t batch_count     = 1;
		uint32_t total_instances = item->instance_count;
		uint32_t total_inst_data = item->instance_data_size * item->instance_count;
		while (i + batch_count < list->count) {
			const skr_render_item_t* next = &list->items[i + batch_count];
			if (next->mesh != item->mesh || next->material != item->material)
				break;
			total_instances += next->instance_count;
			total_inst_data += next->instance_data_size * next->instance_count;
			batch_count++;
		}

		// Bind pipeline (only if changed)
		if (pipeline != bound_pipeline) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound_pipeline = pipeline;
		}

		// Build per-draw descriptor writes
		VkWriteDescriptorSet   writes      [32]; // Material writes
		VkDescriptorBufferInfo buffer_infos[16];
		VkDescriptorImageInfo  image_infos [16];
		uint32_t write_ct  = 0;
		uint32_t buffer_ct = 0;
		uint32_t image_ct  = 0;

		// Material parameter buffer
		if (item->material->param_buffer_size > 0) {
			buffer_infos[buffer_ct] = (VkDescriptorBufferInfo){
				.buffer = list->material_param_buffer.buffer,
				.offset = material_data_offset,
				.range  = item->material->param_buffer_size,
			};
			writes[write_ct++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = SKR_BIND_SHIFT_BUFFER + SKR_BIND_MATERIAL,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo     = &buffer_infos[buffer_ct++],
			};
		}

		// System data buffer
		if (item->material->has_system_buffer) {
			buffer_infos[buffer_ct] = (VkDescriptorBufferInfo){
				.buffer = list->system_buffer.buffer,
				.range  = list->system_buffer.size,
			};
			writes[write_ct++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = SKR_BIND_SHIFT_BUFFER + SKR_BIND_SYSTEM,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo     = &buffer_infos[buffer_ct++],
			};
		}

		// Instance data buffer
		if (item->instance_data_size > 0) {
			buffer_infos[buffer_ct] = (VkDescriptorBufferInfo){
				.buffer = list->instance_buffer.buffer,
				.offset = item->instance_offset,
				.range  = total_inst_data,
			};
			writes[write_ct++] = (VkWriteDescriptorSet){
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstBinding      = SKR_BIND_SHIFT_TEXTURE + SKR_BIND_INSTANCE,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo     = &buffer_infos[buffer_ct++],
			};
		}

		const int32_t ignore_slots[] = {
			SKR_BIND_SHIFT_TEXTURE + SKR_BIND_INSTANCE,
			SKR_BIND_SHIFT_BUFFER  + SKR_BIND_MATERIAL,
			SKR_BIND_SHIFT_BUFFER  + SKR_BIND_SYSTEM };
		// Material texture and buffer binds
		const skr_material_t* mat = item->material;
		_skr_material_add_writes(mat->binds, mat->bind_count, ignore_slots, sizeof(ignore_slots)/sizeof(ignore_slots[0]),
			writes,       sizeof(writes      )/sizeof(writes      [0]),
			buffer_infos, sizeof(buffer_infos)/sizeof(buffer_infos[0]),
			image_infos,  sizeof(image_infos )/sizeof(image_infos [0]),
			&write_ct, &buffer_ct, &image_ct);

		// Push all descriptors at once
		if (write_ct > 0) {
			vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skr_pipeline_get_layout(mat->pipeline_material_idx), 0, write_ct, writes);
		}

		// Bind vertex buffer
		if (skr_buffer_is_valid(&item->mesh->vertex_buffer)) {
			VkDeviceSize offset = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &item->mesh->vertex_buffer.buffer, &offset);
		}

		// Draw with instancing (batched)
		// firstInstance = 0 because we offset the buffer binding itself in the descriptor
		uint32_t draw_instances = total_instances * instance_multiplier;
		if (skr_buffer_is_valid(&item->mesh->index_buffer)) {
			vkCmdBindIndexBuffer(cmd, item->mesh->index_buffer.buffer, 0, item->mesh->ind_format_vk);
			vkCmdDrawIndexed    (cmd, item->mesh->ind_count,  draw_instances, 0, 0, 0);
		} else {
			vkCmdDraw           (cmd, item->mesh->vert_count, draw_instances, 0,    0);
		}
		i += batch_count;

		if (item->material != prev_material) {
			prev_material         = item->material;
			material_data_offset += item->material->param_buffer_size;
		}
	}
	_skr_command_release(cmd);
}

float skr_renderer_get_gpu_time_ms() {
	// Return timing from most recently completed frame
	uint32_t read_flight = (_skr_vk.flight_idx + 1) % SKR_MAX_FRAMES_IN_FLIGHT;

	if (!_skr_vk.timestamps_valid[read_flight]) {
		return 0.0f;
	}

	uint64_t start = _skr_vk.frame_timestamps[read_flight][0];
	uint64_t end   = _skr_vk.frame_timestamps[read_flight][1];

	// Convert ticks to milliseconds: (ticks * ns_per_tick) / 1,000,000
	float time_ns = (float)(end - start) * _skr_vk.timestamp_period;
	return time_ns / 1000000.0f;
}
