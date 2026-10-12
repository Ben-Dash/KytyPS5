#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHSNAPSHOT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHSNAPSHOT_H_

#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"
#include "graphics/host_gpu/renderer/writerHistory.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// KYTY_DISPATCH_SNAPSHOT (diagnostic, off by default): writes compute dispatches to disk so that
// `kyty_emulator --dispatch-replay` runs them again without the game (see dispatchReplay.h).
//   KYTY_DISPATCH_SNAPSHOT=<hash>[,<hash>...]  dispatches of these compute shaders
//   KYTY_DISPATCH_SNAPSHOT=frame:<n>           every dispatch of the first frame >= n that has any
//   KYTY_DISPATCH_SNAPSHOT=1                   what dispatch_snapshot.txt names, else frame:600
//   KYTY_DISPATCH_SNAPSHOT_MAX=<n>             snapshots per shader (8) or per frame (4000)
//   KYTY_DISPATCH_SNAPSHOT_DIR=<dir>           where they go (_snapshots)
//   KYTY_DISPATCH_SNAPSHOT_WRITTEN_BUFFER=<slot>:<bytes>  require this written buffer's declared
//     SRD size (decimal, nonzero bytes); rejected dispatches do not consume capture limits.
//   KYTY_DISPATCH_SNAPSHOT_JOB_RANGE=<count_dw>:<base_dw>:<first_index>:<required_count>
//     require a known 1D job interval from these actual flattened-SRT words to contain the whole
//     requested interval (strict decimal uint32 fields). This is a diagnostic admission hint,
//     not evidence of executed stores. Unknown indirect groups are not admitted.
// A snapshot holds the permutation's journal record and live SPIR-V, the host contents of every
// buffer, image and sampler the dispatch binds, its push data and group counts, and afterwards
// the contents the dispatch wrote. The inputs reach the disk before the dispatch is submitted, so
// a dispatch that loses the device still leaves its snapshot. Each snapshot drains the GPU twice.

namespace Libs::Graphics {

class RenderContext;
struct PreparedBindings;
struct ShaderComputeInputInfo;
struct ShaderVertexInputInfo;
struct ShaderStageRuntime;

namespace DispatchSnapshot {

// What a permutation was compiled from, kept on its CompiledShaderInfo while snapshots are on.
struct Source {
	ShaderPrecompile::PermutationRecord record;
	std::vector<uint32_t>               spirv;
};

[[nodiscard]] bool Enabled();

struct WrittenBufferSelector {
	uint32_t slot = 0;
	uint64_t declared_bytes = 0;
};
// Strict decimal slot:bytes; failure leaves the output unchanged.
[[nodiscard]] bool ParseWrittenBufferSelector(std::string_view text,
                                             WrittenBufferSelector& selector) noexcept;
[[nodiscard]] bool MatchesWrittenBufferSelector(const WrittenBufferSelector& selector,
                                               const ShaderStageRuntime& runtime) noexcept;

struct JobRangeSelector {
	uint32_t count_dword = 0;
	uint32_t base_dword = 0;
	uint32_t first_index = 0;
	uint32_t required_count = 0;
};
// Strict decimal fields; nonzero required count and end <=2^32. Failure leaves output unchanged.
[[nodiscard]] bool ParseJobRangeSelector(std::string_view text,
                                        JobRangeSelector& selector) noexcept;
[[nodiscard]] bool MatchesJobRangeSelector(const JobRangeSelector& selector,
                                          const ShaderComputeInputInfo& input,
                                          std::array<uint32_t, 3> groups) noexcept;

// Whether this dispatch is snapshotted (counts it). Cheap when snapshots are off.
// The runtime-only form has no trusted group dimensions and declines an enabled job filter.
[[nodiscard]] bool Wanted(const ShaderStageRuntime& runtime, uint64_t frame);
[[nodiscard]] bool Wanted(const ShaderComputeInputInfo& input,
                         std::array<uint32_t, 3> groups, uint64_t frame);

struct Pending {
	uint32_t id = 0; // 0: nothing captured.
	std::string directory;
};

// After the bindings are final and before CommitBindings: reads back every input and writes the
// snapshot (meta.json without outputs). `args` is the indirect argument buffer, if any.
[[nodiscard]] Pending CaptureInputs(RenderContext& context, const ShaderComputeInputInfo& input,
                                    const PreparedBindings& bindings,
                                    std::array<uint32_t, 3> groups, uint64_t frame,
                                    vk::Buffer args = nullptr, uint64_t args_offset = 0);

// After the dispatch was recorded: reads back what it wrote and completes meta.json.
void CaptureOutputs(RenderContext& context, const Pending& pending,
                    const ShaderComputeInputInfo& input, const PreparedBindings& bindings);

// KYTY_DRAW_SNAPSHOT (diagnostic, off by default): writes draws to disk for offline reading
// (tools/draw_snapshot_check.py checks indices against the vertex buffers and scans vertex
// data for NaN or huge values, the vertex-explosion case).
//   KYTY_DRAW_SNAPSHOT=<hash>[,<hash>...]  draws whose vertex or pixel shader has one of these
//   KYTY_DRAW_SNAPSHOT=frame:<n>           every draw of the first frame >= n
//   KYTY_DRAW_SNAPSHOT=1                   what draw_snapshot.txt names, else frame:600
//   KYTY_DRAW_SNAPSHOT_MAX=<n>             snapshots per shader (4) or per frame (3000)
//   KYTY_DRAW_SNAPSHOT_DIR=<dir>           where they go (_draw_snapshots)
// A snapshot holds each stage's journal record and SPIR-V (when kept), guest user data and
// descriptors, the host contents of the index buffer, the vertex buffers and every buffer the
// stages bind, the draw parameters and the targets. Images are not captured. Each snapshot
// drains the GPU once.
[[nodiscard]] bool DrawWanted(uint64_t vertex_hash, uint64_t pixel_hash, uint64_t frame);

struct DrawCapture {
	uint64_t frame          = 0;
	uint32_t primitive      = 0; // guest VGT primitive type
	uint32_t topology       = 0; // vk::PrimitiveTopology
	bool     indexed        = false;
	bool     gpu_args       = false;
	bool     restart        = false;
	uint32_t index_count    = 0;
	uint32_t instance_count = 0;
	uint32_t first_instance = 0;
	int32_t  vertex_offset  = 0;
	uint32_t first_vertex   = 0;
	// The guest index buffer and the host one the draw reads.
	uint64_t   index_address    = 0;
	uint64_t   index_guest_size = 0; // bytes the guest buffer spans (GPU args: the whole buffer)
	uint32_t   guest_index_size = 0;
	uint32_t   host_index_size  = 0; // 2 or 4
	vk::Buffer index_buffer     = nullptr;
	uint64_t   index_offset     = 0;
	// Vertex stages (vertex_count of them, the first fetches) and the pixel stage (may be null);
	// `bindings` holds the stages' prepared bindings in the same order, pixel last.
	const ShaderVertexInputInfo* vertex_stages = nullptr;
	uint32_t                     vertex_count  = 0;
	const ShaderStageRuntime*    pixel         = nullptr;
	PreparedBindings* const*     bindings      = nullptr;
	uint32_t                     binding_count = 0;
	// Host vertex buffers bound for the fetch.
	const vk::Buffer*     vertex_buffers      = nullptr;
	const vk::DeviceSize* vertex_offsets      = nullptr;
	const vk::DeviceSize* vertex_sizes        = nullptr;
	uint32_t              vertex_buffer_count = 0;
	// Targets: guest address, host format, width, height; depth address 0: none.
	std::vector<std::array<uint64_t, 4>> colors;
	std::array<uint64_t, 4>              depth {};
};
void CaptureDraw(RenderContext& context, const DrawCapture& draw);

// KYTY_WRITE_WATCH=<hex address>[+<hex size>][,...] (diagnostic, off by default): one red line per
// write that reaches a watched range, naming its source: the command processor's WRITE_DATA,
// COPY_DATA, DMA_DATA, RELEASE_MEM and end-of-pipe writes, and compute dispatches that bind a
// written buffer over it. Finds who writes (or never writes) a value the GPU reads.
[[nodiscard]] bool WatchActive();
void               WatchWrite(const char* source, uint64_t address, uint64_t size, uint64_t detail = 0);
// KYTY_WRITER_HISTORY=1 retains a bounded CPU history of declared writer ranges,
// independent of watched addresses/console limits. Sequence is CPU observation
// order, not GPU execution/completion; no guest reads or GPU commands are added.
[[nodiscard]] bool WriterHistoryEnabled();
[[nodiscard]] WriterHistory::Stats WriterHistoryStats();
[[nodiscard]] WriterHistory::Query QueryWriterHistory(uint64_t address, uint64_t size,
                                                     uint64_t cutoff_sequence);
// Every written buffer; context is before CommitBindings. Indirect group counts
// are CPU input metadata, not a readback of the actual GPU indirect command.
void WatchDispatch(const ShaderComputeInputInfo& input, const PreparedBindings& bindings,
                   uint64_t frame, uint64_t tick, uint64_t submit,
                   std::array<uint32_t, 3> groups, bool indirect = false);

} // namespace DispatchSnapshot
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHSNAPSHOT_H_
