#pragma once

#include "sk_renderer.h"
#include "HandmadeMath.h"
#include <stdint.h>
#include <stdbool.h>

///////////////////////////////////////////////////////////////////////////////
// Common Vertex Format
///////////////////////////////////////////////////////////////////////////////

// Standard vertex format: Position + Normal + UV + Color
typedef struct {
	skr_vec4_t position;
	skr_vec3_t normal;
	skr_vec2_t uv;
	skr_vec4_t color;
} skr_vertex_pnuc_t;

// Standard vertex type for PNUC format (must call skr_vertex_type_init first)
extern skr_vert_type_t skr_vertex_type_pnuc;

// Initialize standard vertex types (call once at startup)
void skr_vertex_types_init(void);

///////////////////////////////////////////////////////////////////////////////
// Common Instance Data
///////////////////////////////////////////////////////////////////////////////

// Standard instance data for world transform only
typedef struct {
	HMM_Mat4 world;
} skr_instance_transform_t;

///////////////////////////////////////////////////////////////////////////////
// Common Texture Samplers
///////////////////////////////////////////////////////////////////////////////

extern const skr_tex_sampler_t skr_sampler_linear_clamp;
extern const skr_tex_sampler_t skr_sampler_linear_wrap;
extern const skr_tex_sampler_t skr_sampler_point_clamp;

///////////////////////////////////////////////////////////////////////////////
// Mesh Generation
///////////////////////////////////////////////////////////////////////////////

// Creates a UV sphere mesh
// segments: Horizontal divisions (e.g., 16 or 32)
// rings: Vertical divisions (e.g., 12 or 24)
// radius: Sphere radius
// color: Vertex color for all vertices
skr_mesh_t skr_mesh_create_sphere(
	int32_t          segments,
	int32_t          rings,
	float            radius,
	skr_vec4_t       color
);

// Creates a cube mesh with optional per-face colors
// size: Cube edge length (e.g., 1.0f for unit cube)
// opt_face_colors: Array of 6 colors [Front, Back, Top, Bottom, Right, Left], or NULL for white
skr_mesh_t skr_mesh_create_cube(
	float            size,
	const skr_vec4_t* opt_face_colors
);

// Creates a pyramid mesh (square base + apex)
// base_size: Width/depth of square base
// height: Height from base center to apex
// color: Vertex color
skr_mesh_t skr_mesh_create_pyramid(
	float            base_size,
	float            height,
	skr_vec4_t       color
);

// Creates a quad mesh
// width: Quad width
// height: Quad height
// normal: Normal direction (determines plane: (0,1,0) = XZ plane, etc.)
// double_sided: If true, creates both front and back faces
// color: Vertex color
skr_mesh_t skr_mesh_create_quad(
	float            width,
	float            height,
	skr_vec3_t       normal,
	bool             double_sided,
	skr_vec4_t       color
);

// Creates a fullscreen quad for post-processing effects
// Uses NDC coordinates (-1 to +1) in XY with Z=0, uses skr_vertex_type_pnuc
// UV coordinates go from (0,0) to (1,1)
skr_mesh_t skr_mesh_create_fullscreen_quad(void);

///////////////////////////////////////////////////////////////////////////////
// Texture Generation
///////////////////////////////////////////////////////////////////////////////

// Creates a checkerboard texture with two alternating colors
// resolution: Texture size (e.g., 512 for 512x512)
// square_size: Size of each square in pixels (e.g., 32)
// color1: First color (RGBA format 0xAABBGGRR)
// color2: Second color (RGBA format 0xAABBGGRR)
// generate_mips: Whether to generate mipmaps
skr_tex_t skr_tex_create_checkerboard(
	int32_t  resolution,
	int32_t  square_size,
	uint32_t color1,
	uint32_t color2,
	bool     generate_mips
);

// Creates a 1x1 solid color texture
// color: RGBA color (format 0xAABBGGRR)
skr_tex_t skr_tex_create_solid_color(uint32_t color);

///////////////////////////////////////////////////////////////////////////////
// Image Loading
///////////////////////////////////////////////////////////////////////////////

// Loads an image from file using stb_image
// filename: Path to image file (png, jpg, etc.)
// opt_out_width: Optional output for image width
// opt_out_height: Optional output for image height
// opt_out_channels: Optional output for original channel count
// force_channels: Number of channels to force (4 for RGBA), or 0 for original
// Returns: Pixel data (must be freed with skr_image_free), or NULL on failure
unsigned char* skr_image_load(
	const char* filename,
	int32_t*    opt_out_width,
	int32_t*    opt_out_height,
	int32_t*    opt_out_channels,
	int32_t     force_channels
);

// Loads an image from memory using stb_image
// data: Pointer to image file data in memory
// size: Size of image data in bytes
// opt_out_width: Optional output for image width
// opt_out_height: Optional output for image height
// opt_out_channels: Optional output for original channel count
// force_channels: Number of channels to force (4 for RGBA), or 0 for original
// Returns: Pixel data (must be freed with skr_image_free), or NULL on failure
unsigned char* skr_image_load_from_memory(
	const void* data,
	size_t      size,
	int32_t*    opt_out_width,
	int32_t*    opt_out_height,
	int32_t*    opt_out_channels,
	int32_t     force_channels
);

// Frees image data allocated by skr_image_load or skr_image_load_from_memory
void skr_image_free(unsigned char* pixels);

///////////////////////////////////////////////////////////////////////////////
// Utility Functions
///////////////////////////////////////////////////////////////////////////////

// Generates a pseudo-random float [0.0, 1.0] from integer position and seed
// Useful for procedural generation with consistent results
float skr_hash_f(int32_t position, uint32_t seed);
