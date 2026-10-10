#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHSNAPSHOT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHSNAPSHOT_H_

#include "graphics/host_gpu/renderer/pipeline/shaderPrecompile.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// KYTY_DISPATCH_SNAPSHOT (diagnostic, off by default): writes compute dispatches to disk so that
// `kyty_emulator --dispatch-replay` runs them again without the game (see dispatchReplay.h).
//   KYTY_DISPATCH_SNAPSHOT=<hash>[,<hash>...]  dispatches of these compute shaders
//   KYTY_DISPATCH_SNAPSHOT=frame:<n>           every dispatch of the first frame >= n that has any
//   KYTY_DISPATCH_SNAPSHOT=1                   what dispatch_snapshot.txt names, else frame:600
//   KYTY_DISPATCH_SNAPSHOT_MAX=<n>             snapshots per shader (8) or per frame (4000)
//   KYTY_DISPATCH_SNAPSHOT_DIR=<dir>           where they go (_snapshots)
// A snapshot holds the permutation's journal record and live SPIR-V, the host contents of every
// buffer, image and sampler the dispatch binds, its push data and group counts, and afterwards
// the contents the dispatch wrote. The inputs reach the disk before the dispatch is submitted, so
// a dispatch that loses the device still leaves its snapshot. Each snapshot drains the GPU twice.

namespace Libs::Graphics {

class RenderContext;
struct PreparedBindings;
struct ShaderComputeInputInfo;

namespace DispatchSnapshot {

// What a permutation was compiled from, kept on its CompiledShaderInfo while snapshots are on.
struct Source {
	ShaderPrecompile::PermutationRecord record;
	std::vector<uint32_t>               spirv;
};

[[nodiscard]] bool Enabled();

// Whether this dispatch is snapshotted (counts it). Cheap when snapshots are off.
[[nodiscard]] bool Wanted(uint64_t shader_hash, uint64_t frame);

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

} // namespace DispatchSnapshot
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHSNAPSHOT_H_
