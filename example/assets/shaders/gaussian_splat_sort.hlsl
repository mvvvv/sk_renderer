// Gaussian splat depth computation and bitonic sort
// Pass 1: Compute depths from camera
// Pass 2+: Bitonic sort steps

// View matrix passed as parameter (no SystemBuffer dependency)
float4x4 view_matrix;

uint   splat_count;
uint   sort_pass;      // 0 = depth compute, 1+ = sort passes
uint   sort_step;      // Current step within bitonic sort stage
uint   sort_stage;     // Current stage of bitonic sort

struct GaussianSplat {
	float3 position;
	float  opacity;
	float3 sh_dc;
	float  _pad1;
	float3 scale;
	float  _pad2;
	float4 rotation;
	float4 sh_rest[15];  // Padded to match C struct
};

StructuredBuffer<GaussianSplat>   splats       : register(t1);
RWStructuredBuffer<uint>          sort_indices : register(u2);
RWStructuredBuffer<float>         sort_depths  : register(u3);

// Compute view-space depth for a splat
float compute_depth(float3 position) {
	float4 p_view = mul(float4(position, 1), view_matrix);
	return -p_view.z; // Negative because we want back-to-front (larger depth = further)
}

[numthreads(256, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
	uint idx = id.x;
	if (idx >= splat_count) return;

	if (sort_pass == 0) {
		// Pass 0: Initialize indices to identity AND compute depths (first time setup)
		sort_indices[idx] = idx;
		sort_depths[idx] = compute_depth(splats[idx].position);
	} else if (sort_pass == 1) {
		// Pass 1: Update depths only (keep sorted indices for refinement)
		// Depth is stored at the INDEX position, not the splat position
		// This allows sorting to work correctly with the indirection
		uint splat_idx = sort_indices[idx];
		sort_depths[idx] = compute_depth(splats[splat_idx].position);
	} else if (sort_pass == 2) {
		// Odd-even transposition sort - adaptive, good for nearly-sorted data
		// sort_step: 0 = even phase (pairs 0-1, 2-3, 4-5...), 1 = odd phase (pairs 1-2, 3-4, 5-6...)
		uint phase = sort_stage & 1;  // Alternate even/odd each step
		uint pair_start = phase;      // 0 for even phase, 1 for odd phase

		// Each thread handles one element, check if we're the "left" of a pair
		if ((idx >= pair_start) && ((idx - pair_start) % 2 == 0)) {
			uint partner = idx + 1;
			if (partner < splat_count) {
				float depth_a = sort_depths[idx];
				float depth_b = sort_depths[partner];

				// Back-to-front: larger depth should come first
				if (depth_a < depth_b) {
					// Swap indices
					uint temp_idx = sort_indices[idx];
					sort_indices[idx] = sort_indices[partner];
					sort_indices[partner] = temp_idx;

					// Swap depths
					sort_depths[idx] = depth_b;
					sort_depths[partner] = depth_a;
				}
			}
		}
	} else { // sort_pass >= 3: Bitonic sort (for initial full sort)
		// Bitonic sort step
		// sort_stage goes from 1 to log2(n)
		// sort_step goes from sort_stage down to 1

		uint half_step = 1u << (sort_step - 1);
		uint step_size = 1u << sort_step;

		// Determine partner index
		uint partner;
		bool ascending;

		// Within a block of size step_size, pairs are (0,half_step), (1,half_step+1), etc.
		uint block_idx = idx / step_size;
		uint local_idx = idx % step_size;

		if (local_idx < half_step) {
			partner = idx + half_step;
		} else {
			partner = idx - half_step;
		}

		// Determine sort direction based on stage
		// Each block of 2^stage elements alternates direction
		uint stage_block = idx / (1u << sort_stage);
		ascending = ((stage_block & 1) == 0);

		// Only process if we're the lower index of the pair
		if (idx < partner && partner < splat_count) {
			float depth_a = sort_depths[idx];
			float depth_b = sort_depths[partner];

			// For back-to-front: larger depths should come first
			// So "ascending" means larger values first (descending depth)
			bool should_swap = ascending ? (depth_a < depth_b) : (depth_a > depth_b);

			if (should_swap) {
				// Swap indices
				uint temp_idx = sort_indices[idx];
				sort_indices[idx] = sort_indices[partner];
				sort_indices[partner] = temp_idx;

				// Swap depths
				sort_depths[idx] = depth_b;
				sort_depths[partner] = depth_a;
			}
		}
	}
}
