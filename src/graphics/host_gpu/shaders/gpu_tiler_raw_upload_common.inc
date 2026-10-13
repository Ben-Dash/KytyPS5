// Compile-time wrappers select 2/4/8-byte pixels and the corresponding raw view.
// Include shared render-target block functions before this file, not a copied swizzle.
// Example4B: RAW_UPLOAD_BYTES4, RAW_UPLOAD_FORMAT r32ui.
// Example8B: RAW_UPLOAD_BYTES8, RAW_UPLOAD_FORMAT rgba16ui.
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0,std430) readonly buffer Input {uint data[];} input_buffer;
#if RAW_UPLOAD_BYTES == 2
// R16Uint: guarded by STORAGE_WRITE_WITHOUT_FORMAT in the native route.
layout(set=0,binding=1) writeonly uniform uimage2D output_image;
#else
layout(set=0,binding=1,RAW_UPLOAD_FORMAT) writeonly uniform uimage2D output_image;
#endif
layout(set=0,binding=2,std140) uniform Push {
  uint src_base;
  uint dst_base;
  uint width;
  uint height;
  uint depth;
  uint surface_z;
  uint pitch_bytes;
  uint slice_bytes;
  uint blocks_per_row;
  uint blocks_per_slice;
  uint tail_x;
  uint tail_y;
  uint tail;
  uint color_transform;
} params;

void main() {
  const uvec3 p=gl_GlobalInvocationID;
  if(p.x>=params.width || p.y>=params.height || p.z!=0u) return;
  // Shared metadata gates already require full2D, no tail, no color conversion.
  // Keep the same checked guest tiled word address as the current detiler.
  const uvec3 block=p/block_extent();
  const uint block_index=block.y*params.blocks_per_row+block.x;
  const uint tiled=block_index*BLOCK_BYTES+
      block_offset(uvec3(p.xy,params.surface_z));
  const uint word=(params.src_base+tiled)>>2u;
  const uint first=input_buffer.data[word];
#if RAW_UPLOAD_BYTES == 2
  const uint shift=((params.src_base+tiled)&2u)*8u;
  imageStore(output_image,ivec2(p.xy),uvec4((first>>shift)&0xffffu,0u,0u,0u));
#elif RAW_UPLOAD_BYTES == 8
  const uint second=input_buffer.data[word+1u];
  // Integer views preserve the encoded half bits, including NaNs/denormals/-0.
  imageStore(output_image,ivec2(p.xy),uvec4(first&0xffffu,first>>16u,
                                         second&0xffffu,second>>16u));
#else
  imageStore(output_image,ivec2(p.xy),uvec4(first,0u,0u,0u));
#endif
}
