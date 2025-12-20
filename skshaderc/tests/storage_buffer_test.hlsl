// Stress test shader for StructuredBuffer element size extraction
// Tests various struct layouts and buffer types

// Simple struct - 2 float3s (should be 28 bytes with padding)
struct Simple {
	float3 position;
	float3 velocity;
};

// Mixed types struct
struct Mixed {
	float4 color;      // 16 bytes
	float3 normal;     // 12 bytes + 4 padding = 16
	float2 uv;         // 8 bytes
	float  scalar;     // 4 bytes
	int    id;         // 4 bytes
	uint   flags;      // 4 bytes
};

// Struct with a matrix
struct WithMatrix {
	float4x4 transform;  // 64 bytes
	float3   position;   // 12 bytes + 4 padding
	float    scale;      // 4 bytes
};

// Struct with fixed array
struct WithArray {
	float4 weights[4];   // 64 bytes
	int    indices[4];   // 16 bytes
	float  value;        // 4 bytes
};

// Nested struct
struct Inner {
	float3 pos;
	float  radius;
};
struct Outer {
	Inner  a;
	Inner  b;
	float4 extra;
};

// Minimal - single scalar
struct SingleFloat {
	float value;
};

// Tightly packed (no float3 padding issues)
struct Packed {
	float4 a;
	float4 b;
	float2 c;
	float2 d;
};

// Output for accumulation
RWStructuredBuffer<float4> output : register(u0);

// Raw primitive type buffers (no struct wrapper)
StructuredBuffer<float4x4>   buf_raw_matrix    : register(t9);
StructuredBuffer<float4>     buf_raw_float4    : register(t10);
StructuredBuffer<float3>     buf_raw_float3    : register(t11);
StructuredBuffer<float>      buf_raw_float     : register(t12);
StructuredBuffer<int4>       buf_raw_int4      : register(t13);
RWStructuredBuffer<float4x4> buf_raw_matrix_rw : register(u4);

// ByteAddressBuffer - raw byte access, no element size concept
ByteAddressBuffer   raw_read  : register(t8);
RWByteAddressBuffer raw_write : register(u3);

// All the test buffers
StructuredBuffer<Simple>      buf_simple      : register(t1);
StructuredBuffer<Mixed>       buf_mixed       : register(t2);
StructuredBuffer<WithMatrix>  buf_matrix      : register(t3);
StructuredBuffer<WithArray>   buf_array       : register(t4);
StructuredBuffer<Outer>       buf_nested      : register(t5);
StructuredBuffer<SingleFloat> buf_scalar      : register(t6);
StructuredBuffer<Packed>      buf_packed      : register(t7);

// RW versions to test both SRV and UAV
RWStructuredBuffer<Simple>    buf_simple_rw   : register(u1);
RWStructuredBuffer<Mixed>     buf_mixed_rw    : register(u2);

uint element_count;

[numthreads(64, 1, 1)]
void cs(uint3 id : SV_DispatchThreadID) {
	if (id.x >= element_count) return;

	float4 result = float4(0, 0, 0, 0);

	// Use all buffers to prevent optimization from removing them
	// Simple
	Simple s = buf_simple[id.x];
	result.xyz += s.position + s.velocity;

	// Mixed
	Mixed m = buf_mixed[id.x];
	result += m.color;
	result.xyz += m.normal;
	result.xy += m.uv;
	result.x += m.scalar + float(m.id) + float(m.flags);

	// WithMatrix
	WithMatrix wm = buf_matrix[id.x];
	result += mul(float4(wm.position, 1), wm.transform);
	result.x += wm.scale;

	// WithArray
	WithArray wa = buf_array[id.x];
	result += wa.weights[0] + wa.weights[1] + wa.weights[2] + wa.weights[3];
	result.x += float(wa.indices[0] + wa.indices[1] + wa.indices[2] + wa.indices[3]);
	result.x += wa.value;

	// Nested (Outer containing 2 Inners)
	Outer o = buf_nested[id.x];
	result.xyz += o.a.pos + o.b.pos;
	result.x += o.a.radius + o.b.radius;
	result += o.extra;

	// SingleFloat
	SingleFloat sf = buf_scalar[id.x];
	result.x += sf.value;

	// Packed
	Packed p = buf_packed[id.x];
	result += p.a + p.b;
	result.xy += p.c + p.d;

	// Raw primitive types (no struct wrapper)
	float4x4 raw_mat = buf_raw_matrix[id.x];
	result += mul(float4(1,1,1,1), raw_mat);
	result += buf_raw_float4[id.x];
	result.xyz += buf_raw_float3[id.x];
	result.x += buf_raw_float[id.x];
	result += float4(buf_raw_int4[id.x]);

	// Write to RW matrix buffer
	buf_raw_matrix_rw[id.x] = raw_mat;

	// RW versions - read and write back modified
	Simple s_rw = buf_simple_rw[id.x];
	s_rw.position += result.xyz * 0.001;
	buf_simple_rw[id.x] = s_rw;

	Mixed m_rw = buf_mixed_rw[id.x];
	m_rw.color += result * 0.001;
	buf_mixed_rw[id.x] = m_rw;

	// ByteAddressBuffer - raw byte access
	// Load 4 floats (16 bytes) from byte offset id.x * 16
	uint byte_offset = id.x * 16;
	result.x += asfloat(raw_read.Load(byte_offset));
	result.y += asfloat(raw_read.Load(byte_offset + 4));
	result.z += asfloat(raw_read.Load(byte_offset + 8));
	result.w += asfloat(raw_read.Load(byte_offset + 12));

	// Write to RWByteAddressBuffer
	raw_write.Store(byte_offset,      asuint(result.x));
	raw_write.Store(byte_offset + 4,  asuint(result.y));
	raw_write.Store(byte_offset + 8,  asuint(result.z));
	raw_write.Store(byte_offset + 12, asuint(result.w));

	// Write final result
	output[id.x] = result;
}
