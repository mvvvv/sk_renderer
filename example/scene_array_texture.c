#include "scene.h"
#include "scene_util.h"
#include "app.h"

#define HANDMADE_MATH_IMPLEMENTATION
#include "HandmadeMath.h"

#include <stdlib.h>
#include <string.h>

// Array texture scene - renders cubes to a 2-layer array texture, displays as red/cyan stereo
typedef struct {
	scene_t            base;
	skr_render_list_t  render_list;

	// 3D rendering (cubes to array texture)
	skr_mesh_t     cube_mesh;
	skr_shader_t   cube_shader;
	skr_material_t cube_material;
	skr_tex_t      checkerboard_texture;
	skr_tex_t      array_render_target;  // 2-layer array texture
	skr_tex_t      depth_buffer;

	// Stereo display (array texture to screen)
	skr_mesh_t     fullscreen_quad;
	skr_shader_t   stereo_shader;
	skr_material_t stereo_material;

	float rotation;
	float eye_separation;
} scene_array_texture_t;

static scene_t* _scene_array_texture_create() {
	scene_array_texture_t* scene = calloc(1, sizeof(scene_array_texture_t));
	if (!scene) return NULL;

	scene->base.size      = sizeof(scene_array_texture_t);
	scene->rotation       = 0.0f;
	scene->eye_separation = 0.2f;
	scene->render_list    = skr_render_list_create();

	// Create cube mesh with per-face colors using utility function
	// Order: Front, Back, Top, Bottom, Right, Left
	skr_vec4_t face_colors[6] = {
		{1.0f, 0.5f, 0.5f, 1.0f},  // Front: Red
		{0.5f, 1.0f, 0.5f, 1.0f},  // Back: Green
		{0.5f, 0.5f, 1.0f, 1.0f},  // Top: Blue
		{1.0f, 1.0f, 0.5f, 1.0f},  // Bottom: Yellow
		{1.0f, 0.5f, 1.0f, 1.0f},  // Right: Magenta
		{0.5f, 1.0f, 1.0f, 1.0f},  // Left: Cyan
	};
	scene->cube_mesh = skr_mesh_create_cube(1.0f, face_colors);
	skr_mesh_set_name(&scene->cube_mesh, "stereo_cube");

	// Create fullscreen quad for stereo display
	scene->fullscreen_quad = skr_mesh_create_fullscreen_quad();
	skr_mesh_set_name(&scene->fullscreen_quad, "stereo_quad");

	// Load cube shader
	void*  shader_data = NULL;
	size_t shader_size = 0;
	if (app_read_file("shaders/test.hlsl.sks", &shader_data, &shader_size)) {
		scene->cube_shader = skr_shader_create(shader_data, shader_size);
		skr_shader_set_name(&scene->cube_shader, "cube_shader");
		free(shader_data);

		if (skr_shader_is_valid(&scene->cube_shader)) {
			scene->cube_material = skr_material_create((skr_material_info_t){
				.shader     = &scene->cube_shader,
				.write_mask = skr_write_default,
				.depth_test = skr_compare_less,
			});
		}
	}

	// Load stereo display shader
	if (app_read_file("shaders/stereo_display.hlsl.sks", &shader_data, &shader_size)) {
		scene->stereo_shader = skr_shader_create(shader_data, shader_size);
		skr_shader_set_name(&scene->stereo_shader, "stereo_shader");
		free(shader_data);

		if (skr_shader_is_valid(&scene->stereo_shader)) {
			scene->stereo_material = skr_material_create((skr_material_info_t){
				.shader     = &scene->stereo_shader,
				.cull       = skr_cull_none,
				.write_mask = skr_write_rgba,
				.depth_test = skr_compare_always,
			});
		}
	}

	// Create checkerboard texture using utility function
	scene->checkerboard_texture = skr_tex_create_checkerboard(512, 32, 0xFFFFFFFF, 0xFF000000, true);
	skr_tex_set_name(&scene->checkerboard_texture, "checkerboard");

	// Create 2-layer array texture (rendered target) - will be created in resize
	scene->array_render_target.image = VK_NULL_HANDLE;
	scene->depth_buffer.image = VK_NULL_HANDLE;

	// Bind textures to materials
	if (skr_material_is_valid(&scene->cube_material)) {
		skr_material_set_tex(&scene->cube_material, 0, &scene->checkerboard_texture);
	}

	return (scene_t*)scene;
}

static void _scene_array_texture_destroy(scene_t* base) {
	scene_array_texture_t* scene = (scene_array_texture_t*)base;

	skr_render_list_destroy(&scene->render_list);
	skr_mesh_destroy(&scene->cube_mesh);
	skr_mesh_destroy(&scene->fullscreen_quad);
	skr_material_destroy(&scene->cube_material);
	skr_material_destroy(&scene->stereo_material);
	skr_shader_destroy(&scene->cube_shader);
	skr_shader_destroy(&scene->stereo_shader);
	skr_tex_destroy(&scene->checkerboard_texture);
	skr_tex_destroy(&scene->array_render_target);
	skr_tex_destroy(&scene->depth_buffer);

	free(scene);
}

static void _scene_array_texture_update(scene_t* base, float delta_time) {
	scene_array_texture_t* scene = (scene_array_texture_t*)base;
	scene->rotation += delta_time;
}

static void _scene_array_texture_render(scene_t* base, int32_t width, int32_t height, HMM_Mat4 viewproj, skr_render_list_t* ref_render_list, app_system_buffer_t* ref_system_buffer) {
	scene_array_texture_t* scene = (scene_array_texture_t*)base;

	// Create/resize array texture if needed
	if (!skr_tex_is_valid(&scene->array_render_target) ||
	    scene->array_render_target.size.x != width ||
	    scene->array_render_target.size.y != height) {

		if (skr_tex_is_valid(&scene->array_render_target)) {
			skr_tex_destroy(&scene->array_render_target);
			skr_tex_destroy(&scene->depth_buffer);
		}

		// Create 2-layer array texture
		scene->array_render_target = skr_tex_create(
			skr_tex_fmt_rgba32,
			skr_tex_flags_writeable | skr_tex_flags_readable | skr_tex_flags_array,
			skr_sampler_linear_clamp,
			(skr_vec3i_t){width, height, 2},  // 2 layers
			1, 0, NULL
		);
		skr_tex_set_name(&scene->array_render_target, "array_stereo_rt");

		// Create depth buffer
		scene->depth_buffer = skr_tex_create(
			skr_tex_fmt_depth32,
			skr_tex_flags_writeable | skr_tex_flags_array,
			skr_sampler_linear_clamp,
			(skr_vec3i_t){width, height, 2},  // 2 layers
			1, 0, NULL
		);
		skr_tex_set_name(&scene->depth_buffer, "array_stereo_depth");

		// Bind array texture to stereo material
		if (skr_material_is_valid(&scene->stereo_material)) {
			skr_material_set_tex(&scene->stereo_material, 0, &scene->array_render_target);
		}
	}

	// Build stereo system buffer (2 views for left/right eye)
	app_system_buffer_t sys_buffer = {0};
	sys_buffer.view_count = 2;

	// Extract projection matrix from viewproj passed from app.c
	HMM_Mat4 projection;
	memcpy(&projection, ref_system_buffer->projection[0], sizeof(float) * 16);

	// Create view matrices for left and right eye
	HMM_Vec3 camera_pos = HMM_V3(0, 3, 8);  // Match app.c default camera
	HMM_Vec3 target     = HMM_V3(0, 0, 0);
	HMM_Vec3 up         = HMM_V3(0, 1, 0);

	// Left eye (offset to the left)
	HMM_Vec3 eye_sep = HMM_V3(-scene->eye_separation * 0.5f, 0, 0);
	HMM_Mat4 view_left = HMM_LookAt_RH(HMM_AddV3(camera_pos, eye_sep), HMM_AddV3(target, eye_sep), up);

	// Right eye (offset to the right)
	eye_sep = HMM_V3(scene->eye_separation * 0.5f, 0, 0);
	HMM_Mat4 view_right = HMM_LookAt_RH(HMM_AddV3(camera_pos, eye_sep), HMM_AddV3(target, eye_sep), up);

	// Calculate inverse matrices and transpose everything to match app.c convention
	HMM_Mat4 view_left_inv_t   = HMM_Transpose(HMM_InvGeneralM4(view_left));
	HMM_Mat4 view_right_inv_t  = HMM_Transpose(HMM_InvGeneralM4(view_right));
	HMM_Mat4 projection_inv_t  = HMM_Transpose(HMM_InvGeneralM4(HMM_Transpose(projection))); // projection is already transposed
	HMM_Mat4 view_left_t       = HMM_Transpose(view_left);
	HMM_Mat4 view_right_t      = HMM_Transpose(view_right);

	// Copy to system buffer (all matrices transposed to match shader expectations)
	memcpy(sys_buffer.view          [0], &view_left_t,       sizeof(float) * 16);
	memcpy(sys_buffer.view          [1], &view_right_t,      sizeof(float) * 16);
	memcpy(sys_buffer.view_inv      [0], &view_left_inv_t,   sizeof(float) * 16);
	memcpy(sys_buffer.view_inv      [1], &view_right_inv_t,  sizeof(float) * 16);
	memcpy(sys_buffer.projection    [0], &projection,        sizeof(float) * 16); // Already transposed
	memcpy(sys_buffer.projection    [1], &projection,        sizeof(float) * 16); // Already transposed
	memcpy(sys_buffer.projection_inv[0], &projection_inv_t,  sizeof(float) * 16);
	memcpy(sys_buffer.projection_inv[1], &projection_inv_t,  sizeof(float) * 16);

	// Compute viewproj matrices (projection * view, matching app.c)
	// projection is already transposed, so un-transpose it for the multiply
	HMM_Mat4 proj_untransposed = HMM_Transpose(projection);
	HMM_Mat4 vp_left           = HMM_MulM4(proj_untransposed, view_left);
	HMM_Mat4 vp_right          = HMM_MulM4(proj_untransposed, view_right);
	HMM_Mat4 viewproj_left     = HMM_Transpose(vp_left);
	HMM_Mat4 viewproj_right    = HMM_Transpose(vp_right);
	memcpy(sys_buffer.viewproj[0], &viewproj_left,  sizeof(float) * 16);
	memcpy(sys_buffer.viewproj[1], &viewproj_right, sizeof(float) * 16);

	// Calculate camera positions and directions for both eyes
	HMM_Vec3 cam_pos_left  = HMM_AddV3(camera_pos, HMM_V3(-scene->eye_separation * 0.5f, 0, 0));
	HMM_Vec3 cam_pos_right = HMM_AddV3(camera_pos, HMM_V3( scene->eye_separation * 0.5f, 0, 0));
	HMM_Vec3 cam_forward   = HMM_NormV3(HMM_SubV3(target, camera_pos));

	sys_buffer.cam_pos[0][0] = cam_pos_left.X;
	sys_buffer.cam_pos[0][1] = cam_pos_left.Y;
	sys_buffer.cam_pos[0][2] = cam_pos_left.Z;
	sys_buffer.cam_pos[0][3] = 0.0f;
	sys_buffer.cam_pos[1][0] = cam_pos_right.X;
	sys_buffer.cam_pos[1][1] = cam_pos_right.Y;
	sys_buffer.cam_pos[1][2] = cam_pos_right.Z;
	sys_buffer.cam_pos[1][3] = 0.0f;
	sys_buffer.cam_dir[0][0] = cam_forward.X;
	sys_buffer.cam_dir[0][1] = cam_forward.Y;
	sys_buffer.cam_dir[0][2] = cam_forward.Z;
	sys_buffer.cam_dir[0][3] = 0.0f;
	sys_buffer.cam_dir[1][0] = cam_forward.X;
	sys_buffer.cam_dir[1][1] = cam_forward.Y;
	sys_buffer.cam_dir[1][2] = cam_forward.Z;
	sys_buffer.cam_dir[1][3] = 0.0f;

	// Build cube instance data (configurable grid)
	const int32_t grid_size_x = 100;
	const int32_t grid_size_z = 100;
	const float   spacing     = 2.0f;
	const int32_t total_cubes = grid_size_x * grid_size_z;

	typedef struct { HMM_Mat4 world; } instance_data_t;
	instance_data_t cube_instances[total_cubes];

	for (int z = 0; z < grid_size_z; z++) {
		for (int x = 0; x < grid_size_x; x++) {
			int idx = x + z * grid_size_x;
			float xpos = (x - grid_size_x * 0.5f + 0.5f) * spacing;
			float zpos = (z - grid_size_z * 0.5f + 0.5f) * spacing;
			float yrot = scene->rotation + (x + z) * 0.2f;
			HMM_Mat4 transform = HMM_MulM4(
				HMM_Translate(HMM_V3(xpos, 0.0f, zpos)),
				HMM_Rotate_RH(yrot, HMM_V3(0.0f, 1.0f, 0.0f))
			);
			cube_instances[idx].world = HMM_Transpose(transform);
		}
	}

	// Render cubes to array texture (separate render pass)
	skr_renderer_begin_pass(&scene->array_render_target, &scene->depth_buffer, NULL, skr_clear_all, (skr_vec4_t){0.0f, 0.0f, 0.0f, 0.0f}, 1.0f, 0);
	skr_renderer_set_viewport((skr_rect_t ){0, 0, (float)scene->array_render_target.size.x, (float)scene->array_render_target.size.y});
	skr_renderer_set_scissor ((skr_recti_t){0, 0, scene->array_render_target.size.x, scene->array_render_target.size.y});

	skr_render_list_add  (&scene->render_list, &scene->cube_mesh, &scene->cube_material, cube_instances, sizeof(instance_data_t), total_cubes);
	skr_renderer_draw    (&scene->render_list, &sys_buffer, sizeof(app_system_buffer_t), sys_buffer.view_count);
	skr_render_list_clear(&scene->render_list);
	skr_renderer_end_pass();

	// Display array texture as red/cyan stereo to swapchain (in the main render pass)
	skr_render_list_add(ref_render_list, &scene->fullscreen_quad, &scene->stereo_material, NULL, 0, 1);
}

const scene_vtable_t scene_array_texture_vtable = {
	.name       = "Array Texture Stereo",
	.create     = _scene_array_texture_create,
	.destroy    = _scene_array_texture_destroy,
	.update     = _scene_array_texture_update,
	.render     = _scene_array_texture_render,
	.get_camera = NULL,
};
