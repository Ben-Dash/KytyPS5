#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUZONES_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUZONES_H_

#include "graphics/host_gpu/renderer/drainStats.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <atomic>
#include <cstdint>

// Sites that record GPU work name its zone (see DrainStats::Zone). With zones off a mark costs
// one load; with them on, the render scheduler installs the marker and timestamps each change
// of zone in its recording buffer. Marks on other command buffers are ignored.
namespace Libs::Graphics::GpuZones {

using Marker = void (*)(void* context, vk::CommandBuffer buffer, DrainStats::Zone zone,
                        uint64_t key, uint64_t pixels);

inline Marker g_marker  = nullptr;
inline void*  g_context = nullptr;

// The key for marks that pass none: the texture cache sets the guest address of the image it
// transfers, with DownloadKey set for transfers back to guest memory.
inline thread_local uint64_t t_key       = 0;
constexpr uint64_t           DownloadKey = uint64_t {1} << 63u;

class KeyScope final {
public:
	explicit KeyScope(uint64_t key) noexcept: m_previous(t_key) { t_key = key; }
	~KeyScope() { t_key = m_previous; }
	KeyScope(const KeyScope&)            = delete;
	KeyScope& operator=(const KeyScope&) = delete;

private:
	uint64_t m_previous;
};

[[nodiscard]] inline bool Enabled() {
	return g_marker != nullptr;
}

// KYTY_DEBUG_GPU_CHECKPOINTS=1 (NVIDIA, VK_NV_device_diagnostic_checkpoints): the marker records a
// checkpoint for each mark instead of a timestamp; when the device is lost, the queue's last
// checkpoints name the zone and key (the shader hash) the GPU was in (see ReportDeviceFault).
struct Checkpoint {
	DrainStats::Zone zone = DrainStats::Zone::Unmarked;
	uint64_t         key  = 0;
	// Draws: index/vertex count << 32 | instance count, the vertex shader hash, and the
	// GPU-written indirect arguments' address (t_detail, set before the mark).
	std::array<uint64_t, 3> detail {};
};
inline thread_local std::array<uint64_t, 3> t_detail {};
constexpr uint64_t                         CheckpointRing = uint64_t {1} << 16u;
inline std::array<Checkpoint, CheckpointRing> g_checkpoints {};
inline std::atomic<uint64_t>               g_checkpoint_next {1};
// KYTY_DEBUG_OP_RING=1: the same ring filled on the host only (no GPU command, so the timing of
// a race stays as it is), with the scheduler tick each mark was recorded under. A lost device
// prints the marks of the submissions the GPU had not completed (see ReportDeviceFault).
inline std::array<uint64_t, CheckpointRing> g_checkpoint_ticks {};
inline bool                                 g_op_ring = false;

// `pixels` is a draw's render area; per-pixel costs stay comparable when the game's dynamic
// resolution changes between runs.
inline void Mark(vk::CommandBuffer buffer, DrainStats::Zone zone, uint64_t key = 0,
                 uint64_t pixels = 0) {
	if (g_marker != nullptr) [[unlikely]] {
		g_marker(g_context, buffer, zone, key != 0 ? key : t_key, pixels);
	}
}

} // namespace Libs::Graphics::GpuZones

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUZONES_H_
