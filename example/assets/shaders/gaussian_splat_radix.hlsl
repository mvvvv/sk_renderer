// Gaussian splat radix/counting sort
// Fast O(n) sort using depth quantization and counting
//
// Pass 0: Clear histogram
// Pass 1: Build histogram + compute depths
// Pass 2: Prefix sum (exclusive scan)
// Pass 3: Scatter to sorted positions (swap buffers)
// Pass 4: Copy back (if needed after odd number of sorts)

float4x4 view_matrix;
uint     splat_count;
uint     sort_pass;      // 0=clear, 1=histogram, 2=prefix, 3=scatter
float    depth_min;      // Min depth for quantization
float    depth_range;    // Max - min depth

// Use many bins to minimize collisions - 65536 bins across 1000 units = ~0.015 unit resolution
// This is about 1.5cm, enough to separate most splats and minimize atomic races
#define NUM_BINS 65536

struct GaussianSplat {
	float3 position;
	float  opacity;
	float3 sh_dc;
	float  _pad1;
	float3 scale;
	float  _pad2;
	float4 rotation;
	float4 sh_rest[15];
};

StructuredBuffer<GaussianSplat>   splats           : register(t1);

// Ping-pong buffers - all RW for flexibility
RWStructuredBuffer<uint>          indices_a        : register(u2);
RWStructuredBuffer<float>         depths_a         : register(u3);
RWStructuredBuffer<uint>          indices_b        : register(u4);
RWStructuredBuffer<float>         depths_b         : register(u5);

// Histogram buffer
RWStructuredBuffer<uint>          histogram        : register(u6);

// Per-element rank within its bin (for stable sorting)
RWStructuredBuffer<uint>          ranks            : register(u7);

// Compute view-space depth for a splat
float compute_depth(float3 position) {
	float4 p_view = mul(float4(position, 1), view_matrix);
	return -p_view.z;
}

// Quantize depth to bin index (back-to-front: larger depths get LOWER bin indices)
uint depth_to_bin(float depth) {
	float normalized = saturate((depth - depth_min) / depth_range);
	// Invert for back-to-front: depth 1.0 -> bin 0, depth 0.0 -> bin NUM_BINS-1
	uint bin = (uint)((1.0f - normalized) * (NUM_BINS - 1));
	return min(bin, NUM_BINS - 1);
}

// Compute stable depth - adds tiny offset based on index to break ties deterministically
// This makes the sort stable: elements with same geometric depth maintain relative order
float compute_stable_depth(float3 position, uint idx) {
	float depth = compute_depth(position);
	// Add tiny offset based on index - small enough to not affect visual sorting
	// but large enough to break ties deterministically
	// Using 1e-7 gives ~10M unique values per unit of depth
	depth += (float)idx * 1e-7f;
	return depth;
}

[numthreads(256, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
	uint idx = id.x;

	if (sort_pass == 0) {
		// Pass 0: Clear histogram only
		for (uint i = idx; i < NUM_BINS; i += 256 * 1024) {
			histogram[i] = 0;
		}
	}
	else if (sort_pass == 5) {
		// Pass 5: Initialize indices to identity (first time only)
		if (idx < splat_count) {
			indices_a[idx] = idx;
			// Use stable depth with original splat index for deterministic tie-breaking
			depths_a[idx] = compute_stable_depth(splats[idx].position, idx);
		}
	}
	else if (sort_pass == 1) {
		// Pass 1: Build histogram + update depths + store ranks
		// The rank is the order in which this element was added to its bin
		// This makes the sort stable: elements keep their relative order within a bin
		if (idx < splat_count) {
			uint splat_idx = indices_a[idx];
			// Use stable depth with original splat index for deterministic tie-breaking
			float depth = compute_stable_depth(splats[splat_idx].position, splat_idx);
			depths_a[idx] = depth;

			uint bin = depth_to_bin(depth);
			uint rank;
			InterlockedAdd(histogram[bin], 1, rank);
			ranks[idx] = rank;  // Store our rank within the bin
		}
	}
	else if (sort_pass == 2) {
		// Pass 2: Prefix sum - simple sequential (single thread)
		if (idx == 0) {
			uint sum = 0;
			for (uint i = 0; i < NUM_BINS; i++) {
				uint count = histogram[i];
				histogram[i] = sum;
				sum += count;
			}
		}
	}
	else if (sort_pass == 3) {
		// Pass 3: Scatter from A to B using stored ranks (deterministic, stable)
		// Output position = prefix_sum[bin] + rank
		// This is stable because rank was assigned in input order during histogram pass
		if (idx < splat_count) {
			float depth = depths_a[idx];
			uint bin = depth_to_bin(depth);
			uint rank = ranks[idx];

			// histogram now contains prefix sums after pass 2
			uint pos = histogram[bin] + rank;

			indices_b[pos] = indices_a[idx];
			depths_b[pos] = depth;
		}
	}
	else if (sort_pass == 4) {
		// Pass 4: Copy from B back to A (so render shader always reads from A)
		if (idx < splat_count) {
			indices_a[idx] = indices_b[idx];
			depths_a[idx] = depths_b[idx];
		}
	}
}
