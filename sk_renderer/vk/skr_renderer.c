#include "_sk_renderer.h"
#include "skr_pipeline.h"
#include "skr_conversions.h"

#include <stdio.h>
#include <assert.h>

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

static void _skr_material_ensure_descriptors(skr_material_t* material) {
	// Check if shader requires descriptors
	bool needs_descriptors = material->info.shader && material->info.shader->meta &&
	                         (material->info.shader->meta->buffer_count > 0 ||
	                          material->info.shader->meta->resource_count > 0);

	// Rebuild if dirty or if descriptors are missing but shader requires them
	if (material->descriptors_dirty || (material->descriptor_write_count == 0 && needs_descriptors)) {
		_skr_material_rebuild_descriptors(material);
	}

	// Patch in current global bindings
	_skr_material_update_globals(material);
}

static void _skr_ensure_buffer(skr_buffer_t* buffer, bool* ref_valid, const void* data, size_t size,
                                skr_buffer_type_ type, const char* name) {
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

// Helper: Transition all textures in a render list to shader-read layout
// This must be called BEFORE render pass begins to avoid in-pass barriers
static void _skr_transition_render_list_textures(VkCommandBuffer cmd, skr_render_list_t* list) {
	if (!list) return;

	for (uint32_t i = 0; i < list->count; i++) {
		const skr_render_item_t* item = &list->items[i];
		if (!skr_material_is_valid(item->material)) continue;

		const sksc_shader_meta_t* meta = item->material->info.shader->meta;
		if (!meta) continue;

		for (uint32_t j = 0; j < meta->resource_count; j++) {
			int32_t slot = meta->resources[j].bind.slot;
			uint8_t reg_type = meta->resources[j].bind.register_type;

			// Get texture from material or globals
			skr_tex_t* tex = (slot >= 0 && slot < 16) ?
				(item->material->textures[slot] ? item->material->textures[slot] : _skr_vk.global_textures[slot]) :
				NULL;

			if (tex && tex->image) {
				// Transition based on register type
				if (reg_type == skr_register_texture) {
					_skr_tex_transition_for_shader_read(cmd, tex, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
				} else if (reg_type == skr_register_readwrite_tex) {
					_skr_tex_transition_for_storage(cmd, tex);
				}
			}
		}
	}
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
	VkCommandBuffer cmd = ctx.cmd;

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
		_skr_command_release(cmd);
		return;
	}

	// Ensure descriptors are up to date
	_skr_material_ensure_descriptors(material);

	// Transition any source textures in material to shader-read layout
	const sksc_shader_meta_t* meta = material->info.shader->meta;
	if (meta) {
		for (uint32_t i = 0; i < meta->resource_count; i++) {
			int32_t slot = meta->resources[i].bind.slot;
			if (slot >= 0 && slot < 16 && material->textures[slot]) {
				_skr_tex_transition_for_shader_read(cmd, material->textures[slot], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			}
		}
	}

	// Transition target texture to color attachment layout
	// Automatic system handles this - will use UNDEFINED if first use or track previous layout
	_skr_tex_transition(cmd, to, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
			vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
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
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

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
				vkCmdSetViewport(cmd, 0, 1, &viewport);
				vkCmdSetScissor (cmd, 0, 1, &scissor);

				// Bind descriptors
				if (material->descriptor_write_count > 0) {
					vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						_skr_pipeline_get_layout(material->pipeline_material_idx),
						0, material->descriptor_write_count, material->descriptor_writes);
				}

				// Draw fullscreen triangle (SV_VertexID generates positions)
				vkCmdDraw(cmd, 3, 1, 0, layer);
			}

			vkCmdEndRenderPass(cmd);

			// Queue per-layer resources for deferred destruction
			_skr_destroy_list_add_framebuffer(ctx.destroy_list, framebuffer);
			_skr_destroy_list_add_image_view (ctx.destroy_list, layer_view);
		}
	} else {
		// Regular 2D texture - use cached framebuffer
		VkFramebuffer framebuffer = _skr_get_or_create_framebuffer(to, render_pass, to, NULL, NULL, false);
		if (framebuffer == VK_NULL_HANDLE) {
			_skr_command_release(cmd);
			return;
		}

		// Begin render pass
		vkCmdBeginRenderPass(cmd, &(VkRenderPassBeginInfo){
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
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

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
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			vkCmdSetScissor (cmd, 0, 1, &scissor);

			// Bind descriptors
			if (material->descriptor_write_count > 0) {
				vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skr_pipeline_get_layout(material->pipeline_material_idx), 0, material->descriptor_write_count, material->descriptor_writes);
			}

			// Draw fullscreen triangle
			vkCmdDraw(cmd, 3, 1, 0, 0);
		}

		vkCmdEndRenderPass(cmd);
	}

	// Transition target texture back to shader read layout
	// Automatic system tracks that it's currently in COLOR_ATTACHMENT_OPTIMAL
	_skr_tex_transition_for_shader_read(cmd, to, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	_skr_command_release(cmd);
}

void skr_renderer_draw(skr_render_list_t* list, const void* system_data, size_t system_data_size, int32_t instance_multiplier) {
	if (!list || list->count == 0) return;

	VkCommandBuffer cmd = _skr_command_acquire().cmd;

	// Update system buffer (bind slot 1) - create or resize if needed
	if (system_data && system_data_size > 0) {
		_skr_ensure_buffer(&list->system_buffer, &list->system_buffer_valid, system_data, system_data_size, skr_buffer_type_constant, "system_buffer");
		skr_renderer_set_global_constants(1, &list->system_buffer);
	}

	// Clamp instance_multiplier to valid range (default to 1)
	instance_multiplier = (instance_multiplier < 1) ? 1 : instance_multiplier;

	// Sort list for batching if needed
	_skr_render_list_sort(list);

	// Upload instance data to GPU if present
	if (list->instance_data_used > 0) {
		char name[128];
		snprintf(name, sizeof(name), "renderlist_inst_data_%lX", (uint64_t)list);
		_skr_ensure_buffer(&list->instance_buffer, &list->instance_buffer_valid, list->instance_data, list->instance_data_used, skr_buffer_type_storage, name);

		// Bind instance buffer globally at slot 2 (transform buffer)
		skr_renderer_set_global_constants(2, &list->instance_buffer);
	}

	// Track bound state to avoid redundant state changes
	VkPipeline bound_pipeline = VK_NULL_HANDLE;

	// Draw items with batching
	for (uint32_t i = 0; i < list->count; ) {
		const skr_render_item_t* item = &list->items[i];

		if (!skr_mesh_is_valid(item->mesh) || !skr_material_is_valid(item->material)) {
			i++;
			continue;
		}

		// Get pipeline from the 3D cache (material x renderpass x vertformat)
		VkPipeline pipeline = _skr_pipeline_get(item->material->pipeline_material_idx, _skr_vk.current_renderpass_idx, item->mesh->vert_type->pipeline_idx);
		assert(pipeline != VK_NULL_HANDLE && "Is the Vertex format out of scope?");

		// Find consecutive items with same mesh/material for batching
		uint32_t batch_count     = 1;
		uint32_t total_instances = item->instance_count;
		while (i + batch_count < list->count) {
			const skr_render_item_t* next = &list->items[i + batch_count];
			if (next->mesh != item->mesh || next->material != item->material || next->instance_data_size != item->instance_data_size)
				break;
			total_instances += next->instance_count;
			batch_count++;
		}

		// Bind pipeline (only if changed)
		if (pipeline != bound_pipeline) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound_pipeline = pipeline;
		}

		// Update material descriptors (rebuild if dirty, patch in current globals)
		_skr_material_ensure_descriptors(item->material);

		// Note: Texture transitions should happen BEFORE render pass begins
		// We can't safely transition textures inside a render pass without self-dependencies
		// For now, we rely on textures being in correct layout from previous operations
		// TODO: Add a pre-draw texture transition pass before render pass begins

		if (item->material->descriptor_write_count > 0) {
			vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, _skr_pipeline_get_layout(item->material->pipeline_material_idx), 0, item->material->descriptor_write_count, item->material->descriptor_writes);
		}

		// Bind vertex buffer
		if (skr_buffer_is_valid(&item->mesh->vertex_buffer)) {
			VkDeviceSize offset = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &item->mesh->vertex_buffer.buffer, &offset);
		}

		// Calculate first instance offset (byte offset / instance size)
		uint32_t first_instance = (item->instance_data_size > 0) ? (item->instance_offset / item->instance_data_size) : 0;
		uint32_t draw_instances = total_instances * instance_multiplier;

		// Draw with instancing (batched)
		if (skr_buffer_is_valid(&item->mesh->index_buffer)) {
			vkCmdBindIndexBuffer(cmd, item->mesh->index_buffer.buffer, 0, item->mesh->ind_format_vk);
			vkCmdDrawIndexed    (cmd, item->mesh->ind_count,  draw_instances, 0, 0, first_instance * instance_multiplier);
		} else {
			vkCmdDraw           (cmd, item->mesh->vert_count, draw_instances, 0,    first_instance * instance_multiplier);
		}
		i += batch_count;
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
