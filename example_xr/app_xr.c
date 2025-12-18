// Application rendering layer for OpenXR example
// Creates resources and renders cubes at hand positions

#include "app_xr.h"
#include "openxr_util.h"

#include "float_math.h"

#include <sk_renderer.h>
#include <sksc_file.h>
#include <sk_app.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

///////////////////////////////////////////
// System buffer (matches common.hlsli)
///////////////////////////////////////////

typedef struct {
	float4x4 view[6];
	float4x4 view_inv[6];
	float4x4 projection[6];
	float4x4 projection_inv[6];
	float4x4 viewproj[6];
	float4   cam_pos[6];
	float4   cam_dir[6];
	float4   cubemap_info;
	float    time;
	uint32_t view_count;
	uint32_t _pad[2];
} system_buffer_t;

///////////////////////////////////////////
// Vertex format
///////////////////////////////////////////

typedef struct {
	float    pos[3];
	float    norm[3];
	float    uv[2];
	uint32_t color;
} vertex_pnuc_t;

typedef struct {
	float4x4 world;
} instance_t;

///////////////////////////////////////////
// Module state
///////////////////////////////////////////

static skr_vert_type_t    s_vertex_type;
static skr_mesh_t         s_cube_mesh;
static skr_shader_t       s_shader;
static skr_material_t     s_material;
static skr_tex_t          s_white_tex;
static skr_render_list_t  s_render_list;

// Spawned cubes (from select action)
#define MAX_CUBES 256
static XrPosef s_cubes[MAX_CUBES];
static int32_t s_cube_count = 0;

static float   s_time = 0.0f;

///////////////////////////////////////////
// Helper functions
///////////////////////////////////////////

// Create projection matrix from OpenXR asymmetric FOV
// Exact StereoKit approach: create in their format, then transpose
static float4x4 xr_projection(XrFovf fov, float clip_near, float clip_far) {
	const float tan_left   = tanf(fov.angleLeft);
	const float tan_right  = tanf(fov.angleRight);
	const float tan_down   = tanf(fov.angleDown);
	const float tan_up     = tanf(fov.angleUp);

	const float tan_width  = tan_right - tan_left;
	const float tan_height = tan_up - tan_down;
	const float range      = clip_far / (clip_near - clip_far);

	// StereoKit's pre-transpose layout (row-major [row][col]):
	// [0][0] = 2/tanW, [1][1] = 2/tanH
	// [2][0] = offset_x, [2][1] = offset_y, [2][2] = range, [2][3] = -1
	// [3][2] = range * near
	float4x4 sk = {{
		2.0f / tan_width, 0.0f, 0.0f, 0.0f,
		0.0f, 2.0f / tan_height, 0.0f, 0.0f,
		(tan_right + tan_left) / tan_width, (tan_up + tan_down) / tan_height, range, -1.0f,
		0.0f, 0.0f, range * clip_near, 0.0f
	}};

	// Transpose (StereoKit does this), then apply Vulkan Y-flip
	float4x4 result = float4x4_transpose(sk);
	result.m[5] *= -1.0f;  // Vulkan Y-flip: negate Y scale
	result.m[6] *= -1.0f;  // Vulkan Y-flip: negate Y offset
	return result;
}

// Create view matrix from OpenXR pose (inverse of pose transform)
static float4x4 xr_view_matrix(XrPosef pose) {
	float4 q = (float4){pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
	float3 p = (float3){pose.position.x, pose.position.y, pose.position.z};

	// Invert the pose: conjugate rotation, rotate negated position
	float4 q_inv = float4_quat_conjugate(q);
	float3 p_inv = float4_quat_rotate(q_inv, float3_mul_s(p, -1.0f));

	return float4x4_trs(p_inv, q_inv, (float3){1, 1, 1});
}

// Create world matrix from OpenXR pose
static float4x4 xr_world_matrix(XrPosef pose, float scale) {
	float4 q = (float4){pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
	float3 p = (float3){pose.position.x, pose.position.y, pose.position.z};
	return float4x4_trs(p, q, (float3){scale, scale, scale});
}

///////////////////////////////////////////
// Mesh creation (inline cube)
///////////////////////////////////////////

static void create_cube_mesh(void) {
	// Per-face colors (ABGR format)
	uint32_t colors[6] = {
		0xFF6464FF,  // +X red
		0xFF64FF64,  // +Y green
		0xFFFF6464,  // +Z blue
		0xFF6464C8,  // -X dark red
		0xFF64C864,  // -Y dark green
		0xFFC86464,  // -Z dark blue
	};

	float s = 0.5f;  // Half-size

	// 6 faces * 4 vertices = 24 vertices
	vertex_pnuc_t verts[24] = {
		// +X face
		{{ s, -s, -s}, { 1, 0, 0}, {0, 0}, colors[0]},
		{{ s,  s, -s}, { 1, 0, 0}, {1, 0}, colors[0]},
		{{ s,  s,  s}, { 1, 0, 0}, {1, 1}, colors[0]},
		{{ s, -s,  s}, { 1, 0, 0}, {0, 1}, colors[0]},
		// -X face
		{{-s, -s,  s}, {-1, 0, 0}, {0, 0}, colors[3]},
		{{-s,  s,  s}, {-1, 0, 0}, {1, 0}, colors[3]},
		{{-s,  s, -s}, {-1, 0, 0}, {1, 1}, colors[3]},
		{{-s, -s, -s}, {-1, 0, 0}, {0, 1}, colors[3]},
		// +Y face
		{{-s,  s, -s}, { 0, 1, 0}, {0, 0}, colors[1]},
		{{-s,  s,  s}, { 0, 1, 0}, {1, 0}, colors[1]},
		{{ s,  s,  s}, { 0, 1, 0}, {1, 1}, colors[1]},
		{{ s,  s, -s}, { 0, 1, 0}, {0, 1}, colors[1]},
		// -Y face
		{{-s, -s,  s}, { 0,-1, 0}, {0, 0}, colors[4]},
		{{-s, -s, -s}, { 0,-1, 0}, {1, 0}, colors[4]},
		{{ s, -s, -s}, { 0,-1, 0}, {1, 1}, colors[4]},
		{{ s, -s,  s}, { 0,-1, 0}, {0, 1}, colors[4]},
		// +Z face
		{{-s, -s,  s}, { 0, 0, 1}, {0, 0}, colors[2]},
		{{ s, -s,  s}, { 0, 0, 1}, {1, 0}, colors[2]},
		{{ s,  s,  s}, { 0, 0, 1}, {1, 1}, colors[2]},
		{{-s,  s,  s}, { 0, 0, 1}, {0, 1}, colors[2]},
		// -Z face
		{{ s, -s, -s}, { 0, 0,-1}, {0, 0}, colors[5]},
		{{-s, -s, -s}, { 0, 0,-1}, {1, 0}, colors[5]},
		{{-s,  s, -s}, { 0, 0,-1}, {1, 1}, colors[5]},
		{{ s,  s, -s}, { 0, 0,-1}, {0, 1}, colors[5]},
	};

	// 6 faces * 2 triangles * 3 indices = 36 indices
	uint16_t inds[36] = {
		 0, 1, 2,  0, 2, 3,   // +X
		 4, 5, 6,  4, 6, 7,   // -X
		 8, 9,10,  8,10,11,   // +Y
		12,13,14, 12,14,15,   // -Y
		16,17,18, 16,18,19,   // +Z
		20,21,22, 20,22,23,   // -Z
	};

	skr_mesh_create(&s_vertex_type, skr_index_fmt_u16, verts, 24, inds, 36, &s_cube_mesh);
	skr_mesh_set_name(&s_cube_mesh, "XR Cube");
}

///////////////////////////////////////////
// Shader loading
///////////////////////////////////////////

static bool load_shader(void) {
	void*  data = NULL;
	size_t size = 0;

	if (!ska_asset_read("shaders/test.hlsl.sks", &data, &size)) {
		skr_log(skr_log_critical, "Failed to open shader file");
		return false;
	}

	skr_err_ err = skr_shader_create(data, (uint32_t)size, &s_shader);
	ska_file_free_data(data);

	if (err != skr_err_success) {
		skr_log(skr_log_critical, "Failed to create shader");
		return false;
	}

	skr_shader_set_name(&s_shader, "XR Test Shader");
	return true;
}

///////////////////////////////////////////
// Public API
///////////////////////////////////////////

void app_xr_init(void) {
	// Create vertex type (position, normal, uv, color)
	skr_vert_component_t components[] = {
		{ .format = skr_vertex_fmt_f32,            .count = 3, .semantic = skr_semantic_position, .semantic_slot = 0 },
		{ .format = skr_vertex_fmt_f32,            .count = 3, .semantic = skr_semantic_normal,   .semantic_slot = 0 },
		{ .format = skr_vertex_fmt_f32,            .count = 2, .semantic = skr_semantic_texcoord, .semantic_slot = 0 },
		{ .format = skr_vertex_fmt_ui8_normalized, .count = 4, .semantic = skr_semantic_color,    .semantic_slot = 0 },
	};
	skr_vert_type_create(components, 4, &s_vertex_type);

	// Create cube mesh
	create_cube_mesh();

	// Load shader
	if (!load_shader()) {
		skr_log(skr_log_warning, "Failed to load shader - rendering will fail");
		return;
	}

	// Create 1x1 white texture
	uint32_t white_pixel = 0xFFFFFFFF;
	skr_tex_create(
		skr_tex_fmt_rgba32_srgb,
		skr_tex_flags_readable,
		(skr_tex_sampler_t){ .sample = skr_tex_sample_linear, .address = skr_tex_address_wrap },
		(skr_vec3i_t){ 1, 1, 1 },
		1, 1,
		&(skr_tex_data_t){.data = &white_pixel, .mip_count = 1, .layer_count = 1},
		&s_white_tex
	);
	skr_tex_set_name(&s_white_tex, "White");

	// Create material
	skr_material_create((skr_material_info_t){
		.shader     = &s_shader,
		.cull       = skr_cull_back,
		.depth_test = skr_compare_less,
	}, &s_material);
	skr_material_set_tex(&s_material, "tex", &s_white_tex);

	// Create render list
	skr_render_list_create(&s_render_list);

	// Initialize with hand cubes (indices 0 and 1)
	s_cubes[0] = xr_pose_identity;
	s_cubes[1] = xr_pose_identity;

	// Add some static cubes in front of the user so there's something to see
	// Cube at (0, 0, -1.5) - directly in front, 1.5m away
	s_cubes[2] = (XrPosef){ .orientation = {0, 0, 0, 1}, .position = {0, 0, -1.5f} };
	// Cube at (-0.5, 0.3, -1) - to the left and slightly up
	s_cubes[3] = (XrPosef){ .orientation = {0, 0, 0, 1}, .position = {-0.5f, 0.3f, -1.0f} };
	// Cube at (0.5, -0.2, -1) - to the right and slightly down
	s_cubes[4] = (XrPosef){ .orientation = {0, 0, 0, 1}, .position = {0.5f, -0.2f, -1.0f} };
	// Cube on the floor
	s_cubes[5] = (XrPosef){ .orientation = {0, 0, 0, 1}, .position = {0, -1.0f, -2.0f} };

	s_cube_count = 6;
}

void app_xr_shutdown(void) {
	skr_render_list_destroy(&s_render_list);
	skr_material_destroy(&s_material);
	skr_tex_destroy(&s_white_tex);
	skr_shader_destroy(&s_shader);
	skr_mesh_destroy(&s_cube_mesh);
	skr_vert_type_destroy(&s_vertex_type);
}

void app_xr_update(void) {
	// Check for select action - spawn cube at hand position
	for (uint32_t i = 0; i < 2; i++) {
		if (xr_input.handSelect[i] && s_cube_count < MAX_CUBES) {
			s_cubes[s_cube_count++] = xr_input.handPose[i];
		}
	}
}

void app_xr_update_predicted(void) {
	// Update hand cube positions with predicted poses
	for (uint32_t i = 0; i < 2; i++) {
		if (xr_input.renderHand[i]) {
			s_cubes[i] = xr_input.handPose[i];
		} else {
			// Hide hand cube by moving it far away
			s_cubes[i].position = (XrVector3f){ 0, -1000, 0 };
		}
	}
}

void app_xr_render_stereo(skr_tex_t* color_target, skr_tex_t* resolve_target, skr_tex_t* depth_target, const XrView* views, uint32_t view_count, int32_t width, int32_t height) {
	s_time += 1.0f / 72.0f;  // Assume 72Hz

	// Build system buffer with all views
	system_buffer_t sys = {0};
	sys.time       = s_time;
	sys.view_count = view_count;

	for (uint32_t v = 0; v < view_count && v < 6; v++) {
		const XrView* view = &views[v];

		float4x4 view_mat = xr_view_matrix(view->pose);
		float4x4 proj_mat = xr_projection(view->fov, 0.05f, 100.0f);

		float4 q       = (float4){view->pose.orientation.x, view->pose.orientation.y, view->pose.orientation.z, view->pose.orientation.w};
		float3 cam_pos = (float3){view->pose.position.x, view->pose.position.y, view->pose.position.z};
		float3 cam_dir = float4_quat_rotate(q, (float3){0, 0, -1});  // Forward is -Z

		sys.view[v]           = view_mat;
		sys.view_inv[v]       = float4x4_invert(view_mat);
		sys.projection[v]     = proj_mat;
		sys.projection_inv[v] = float4x4_invert(proj_mat);
		sys.viewproj[v]       = float4x4_mul(proj_mat, view_mat);
		sys.cam_pos[v]        = (float4){cam_pos.x, cam_pos.y, cam_pos.z, 1};
		sys.cam_dir[v]        = (float4){cam_dir.x, cam_dir.y, cam_dir.z, 0};
	}

	// Add cubes to render list
	for (int32_t i = 0; i < s_cube_count; i++) {
		instance_t inst = { .world = xr_world_matrix(s_cubes[i], 0.05f) };
		skr_render_list_add(&s_render_list, &s_cube_mesh, &s_material, &inst, sizeof(inst), 1);
	}

	// Begin render pass with MSAA resolve (in-tile resolve on mobile)
	skr_renderer_begin_pass(
		color_target,
		depth_target,
		resolve_target,  // Resolve MSAA to this target
		skr_clear_all,
		(skr_vec4_t){ 0.0f, 0.0f, 0.0f, 0.0f },
		1.0f,
		0
	);

	skr_renderer_set_viewport((skr_rect_t){ 0, 0, (float)width, (float)height });
	skr_renderer_set_scissor((skr_recti_t){ 0, 0, width, height });

	// Draw with multi-view instancing - view_count instances per object
	skr_renderer_draw(&s_render_list, &sys, sizeof(sys), view_count);

	skr_renderer_end_pass();

	// Clear render list for next frame
	skr_render_list_clear(&s_render_list);
}


