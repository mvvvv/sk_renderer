// Application rendering layer for OpenXR example
// Creates resources and renders cubes at hand positions

#include "app_xr.h"
#include "openxr_util.h"

#include "float_math.h"

#include <sk_renderer.h>
#include <sksc_file.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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

// Pack RGBA into uint32
static uint32_t color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

// Create projection matrix from OpenXR asymmetric FOV
static float4x4 xr_projection(XrFovf fov, float near_plane, float far_plane) {
	const float left   = near_plane * tanf(fov.angleLeft);
	const float right  = near_plane * tanf(fov.angleRight);
	const float down   = near_plane * tanf(fov.angleDown);
	const float up     = near_plane * tanf(fov.angleUp);

	const float width  = right - left;
	const float height = up - down;

	// Row-major, right-handed, zero-to-one depth, Vulkan Y-flip
	return (float4x4){{
		2.0f * near_plane / width, 0.0f, (right + left) / width, 0.0f,
		0.0f, -2.0f * near_plane / height, (up + down) / height, 0.0f,
		0.0f, 0.0f, far_plane / (near_plane - far_plane), -(far_plane * near_plane) / (far_plane - near_plane),
		0.0f, 0.0f, -1.0f, 0.0f
	}};
}

// Create view matrix from OpenXR pose
static float4x4 xr_view_matrix(XrPosef pose) {
	// OpenXR provides camera pose (position + orientation)
	// We need the inverse for the view matrix
	XrQuaternionf q = pose.orientation;
	XrVector3f    p = pose.position;

	// Quaternion to rotation matrix (transposed for view matrix)
	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	// Rotation matrix (transposed = inverse for unit quaternion)
	float r00 = 1.0f - 2.0f * (yy + zz);
	float r01 = 2.0f * (xy + wz);
	float r02 = 2.0f * (xz - wy);
	float r10 = 2.0f * (xy - wz);
	float r11 = 1.0f - 2.0f * (xx + zz);
	float r12 = 2.0f * (yz + wx);
	float r20 = 2.0f * (xz + wy);
	float r21 = 2.0f * (yz - wx);
	float r22 = 1.0f - 2.0f * (xx + yy);

	// Apply inverse translation
	float tx = -(r00 * p.x + r01 * p.y + r02 * p.z);
	float ty = -(r10 * p.x + r11 * p.y + r12 * p.z);
	float tz = -(r20 * p.x + r21 * p.y + r22 * p.z);

	return (float4x4){{
		r00, r01, r02, tx,
		r10, r11, r12, ty,
		r20, r21, r22, tz,
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

// Create world matrix from OpenXR pose
static float4x4 xr_world_matrix(XrPosef pose, float scale) {
	XrQuaternionf q = pose.orientation;
	XrVector3f    p = pose.position;

	float xx = q.x * q.x;
	float yy = q.y * q.y;
	float zz = q.z * q.z;
	float xy = q.x * q.y;
	float xz = q.x * q.z;
	float yz = q.y * q.z;
	float wx = q.w * q.x;
	float wy = q.w * q.y;
	float wz = q.w * q.z;

	return (float4x4){{
		scale * (1.0f - 2.0f * (yy + zz)), scale * (2.0f * (xy - wz)), scale * (2.0f * (xz + wy)), p.x,
		scale * (2.0f * (xy + wz)), scale * (1.0f - 2.0f * (xx + zz)), scale * (2.0f * (yz - wx)), p.y,
		scale * (2.0f * (xz - wy)), scale * (2.0f * (yz + wx)), scale * (1.0f - 2.0f * (xx + yy)), p.z,
		0.0f, 0.0f, 0.0f, 1.0f
	}};
}

///////////////////////////////////////////
// Mesh creation (inline cube)
///////////////////////////////////////////

static void create_cube_mesh(void) {
	// Per-face colors
	uint32_t colors[6] = {
		color_rgba(255, 100, 100, 255),  // +X red
		color_rgba(100, 255, 100, 255),  // +Y green
		color_rgba(100, 100, 255, 255),  // +Z blue
		color_rgba(200, 100, 100, 255),  // -X dark red
		color_rgba(100, 200, 100, 255),  // -Y dark green
		color_rgba(100, 100, 200, 255),  // -Z dark blue
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
	// Try to load test.hlsl.sks from assets
	FILE* f = fopen("assets/shaders/test.hlsl.sks", "rb");
	if (!f) {
		// Try alternate path
		f = fopen("../example/assets/shaders/test.hlsl.sks", "rb");
	}
	if (!f) {
		skr_log(skr_log_critical, "Failed to open shader file");
		return false;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	void* data = malloc(size);
	fread(data, 1, size, f);
	fclose(f);

	skr_err_ err = skr_shader_create(data, (uint32_t)size, &s_shader);
	free(data);

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
		1, 1, &white_pixel,
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
		float4x4 viewproj = float4x4_mul(proj_mat, view_mat);

		// Camera position from pose
		float3 cam_pos = { view->pose.position.x, view->pose.position.y, view->pose.position.z };

		// Camera direction (forward is -Z in OpenXR)
		XrQuaternionf q = view->pose.orientation;
		float3 cam_dir = {
			2.0f * (q.x * q.z + q.w * q.y),
			2.0f * (q.y * q.z - q.w * q.x),
			1.0f - 2.0f * (q.x * q.x + q.y * q.y)
		};
		cam_dir = float3_mul_s(cam_dir, -1.0f);  // Negate for forward

		sys.view[v]           = view_mat;
		sys.view_inv[v]       = float4x4_invert(view_mat);
		sys.projection[v]     = proj_mat;
		sys.projection_inv[v] = float4x4_invert(proj_mat);
		sys.viewproj[v]       = viewproj;
		sys.cam_pos[v]        = (float4){ cam_pos.x, cam_pos.y, cam_pos.z, 1.0f };
		sys.cam_dir[v]        = (float4){ cam_dir.x, cam_dir.y, cam_dir.z, 0.0f };
	}

	// Add cubes to render list
	for (int32_t i = 0; i < s_cube_count; i++) {
		float scale = (i < 2) ? 0.05f : 0.05f;  // Hand cubes and spawned cubes same size
		instance_t inst = { .world = xr_world_matrix(s_cubes[i], scale) };
		skr_render_list_add(&s_render_list, &s_cube_mesh, &s_material, &inst, sizeof(inst), 1);
	}

	// Begin render pass with MSAA resolve (in-tile resolve on mobile)
	skr_renderer_begin_pass(
		color_target,
		depth_target,
		resolve_target,  // Resolve MSAA to this target
		skr_clear_all,
		(skr_vec4_t){ 0.0f, 0.0f, 0.0f, 1.0f },
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
