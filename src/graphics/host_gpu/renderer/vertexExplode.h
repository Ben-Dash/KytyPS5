#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_VERTEXEXPLODE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_VERTEXEXPLODE_H_

#include <cstdint>
#include <span>

namespace Libs::Graphics {
struct ShaderVertexInputInfo;
class BufferCache;
class RenderContext;
struct PreparedBindings;
} // namespace Libs::Graphics

// Diagnostics, KYTY_VTX_EXPLODE_CHECK=1: reports exploded vertex positions (records the shaders
// write, ShaderRecompiler/IR/VertexExplodeLog.h) and indexed draws whose largest index lies past
// their vertex buffers, as red console lines, one per shader hash and kind.
namespace Libs::Graphics::VertexExplode {

[[nodiscard]] bool Enabled();

// The draw being recorded: remembered per vertex-stage shader hash, so a record names the last
// draw of its shader. For an indexed CPU-args draw this also compares the largest index with the
// vertex buffers (the CPU check). `index_host` holds expanded 8-bit indices, else nullptr.
struct DrawNote {
	uint64_t                            frame          = 0;
	std::span<const ShaderVertexInputInfo> stages;
	bool                                indexed        = false;
	bool                                gpu_args       = false;
	bool                                restart        = false;
	uint32_t                            primitive      = 0;
	uint64_t                            index_address  = 0;
	const void*                         index_host     = nullptr;
	uint32_t                            index_size     = 0; // bytes per index (2 or 4 on the host)
	uint32_t                            index_count    = 0;
	uint32_t                            instance_count = 0;
	int32_t                             vertex_offset  = 0;
	uint32_t                            first_instance = 0;
	uint64_t                            ps_hash        = 0; // 0: no pixel shader (depth only)
	uint32_t                            color_count    = 0;
	uint64_t                            color_address  = 0; // first color target
	uint64_t                            depth_address  = 0;
	uint32_t                            target_width   = 0;
	uint32_t                            target_height  = 0;
};
void NoteDraw(BufferCache& cache, const DrawNote& note);

// Optional bounded value trace: each token owns a distinct region for the process lifetime.
uint64_t RegisterValueTrace(RenderContext& context, const DrawNote& note, uint64_t submit);
void NoteValueTraceBindings(uint64_t token, std::span<PreparedBindings* const> stages);
void ReportValueTrace(const uint32_t* region, uint64_t frame);

// Prints the records of one read-back explode region (IR::ExplodeLogWords words).
void Report(const uint32_t* region, uint64_t frame);

// Where the command processor last took the index type from: 1 the INDEX_TYPE packet,
// 2 SET_UCONFIG_REG VGT_INDEX_TYPE, 3 SET_UCONFIG_REG_INDIRECT (register pairs read from memory).
void NoteIndexTypeSource(uint32_t source);

// Thread_Gpu, outside scheduler callbacks: reads back the GPU-written 32-bit index buffers that
// NoteDraw queued (a few per frame) and prints what they hold.
void SampleGpuIndices();

} // namespace Libs::Graphics::VertexExplode

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_VERTEXEXPLODE_H_
