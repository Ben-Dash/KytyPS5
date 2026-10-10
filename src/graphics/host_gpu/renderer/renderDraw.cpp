#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/switches.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/gpuZones.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/renderer/meshDispatch.h"
#include "graphics/host_gpu/renderer/meshIndirect.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/vertexExplode.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

// graphicsRun.cpp: prints the GPU thread's last PM4 packets (KYTY_DEBUG_MRT_SHADER).
void DebugDumpRecentPm4();

std::pair<int32_t, uint32_t> ResolveDrawOffsets(uint32_t index_offset,
	                                           const ShaderVertexInputInfo& vs_input_info) {
	auto     vertex_offset   = static_cast<int32_t>(index_offset);
	uint32_t instance_offset = 0;
	if (!vs_input_info.fetch_embedded) {
		return {vertex_offset, instance_offset};
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = *vs_input_info.stage.resources;
	if (index_offset == 0 &&
	    program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			vertex_offset = static_cast<int32_t>(resources.user_data[index]);
		}
	}
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			instance_offset = resources.user_data[index];
		}
	}

	return {vertex_offset, instance_offset};
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

// Debug: KYTY_DEBUG_DRAW_LOG=<pixel shader hash> prints each draw with that pixel shader: its
// geometry, and for a draw with GPU-written arguments the counts the conversion pass wrote, read
// back from the draw-record ring once the GPU is done with them.
namespace DrawLog {

struct Pending {
	const uint8_t*                        output = nullptr;
	uint64_t                              draw   = 0;
	std::chrono::steady_clock::time_point recorded;
};

static uint64_t Hash() {
	static const uint64_t hash = [] {
		const char* text = KYTY_GETENV("KYTY_DEBUG_DRAW_LOG");
		return text != nullptr ? std::strtoull(text, nullptr, 16) : uint64_t {0};
	}();
	return hash;
}

static double Seconds() {
	static const auto start = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static std::vector<Pending>& PendingOutputs() {
	static std::vector<Pending> pending;
	return pending;
}

// Prints the GPU-written counts of draws recorded long enough ago. The ring holds 4 MiB of
// records, far more than a second of draws, so a slot is still intact then.
static void Flush() {
	auto&      pending = PendingOutputs();
	const auto now     = std::chrono::steady_clock::now();
	std::erase_if(pending, [&](const Pending& item) {
		if (now - item.recorded < std::chrono::milliseconds(250)) {
			return false;
		}
		uint32_t record[7] {};
		uint32_t command[3] {};
		std::memcpy(record, item.output + MeshIndirectArgs::RecordOffset, sizeof(record));
		std::memcpy(command, item.output + MeshIndirectArgs::CommandOffset, sizeof(command));
		// A plain (not mesh-emulated) draw's record is its VkDrawIndexedIndirectCommand.
		std::printf("draw-log gpu: draw=%" PRIu64 " index_count=%u first_instance=%u groups=%u "
		            "instances=%u z=%u plain_instances=%u plain_first_index=%u "
		            "plain_vertex_offset=%d plain_first_instance=%u\n",
		            item.draw, record[0], record[2], command[0], command[1], command[2], record[1],
		            record[2], static_cast<int32_t>(record[3]), record[4]);
		return true;
	});
}

// FNV-1a fingerprints of what can differ between draws of one shader pair.
static uint32_t HashWords(const uint32_t* words, size_t count, uint32_t hash = 2166136261u) {
	for (size_t i = 0; i < count; i++) {
		hash = (hash ^ words[i]) * 16777619u;
	}
	return hash;
}

static uint32_t HashDescriptors(const std::vector<ShaderRecompiler::IR::DescriptorValue>& values) {
	uint32_t hash = 2166136261u;
	for (const auto& value: values) {
		hash = HashWords(value.dwords.data(), value.dword_count, hash);
	}
	return hash;
}

static void PrintStageKeys(const char* tag, const ShaderStageRuntime& stage) {
	if (!stage) {
		return;
	}
	const auto& r = *stage.resources;
	std::printf(" %s_ud=%08x %s_buf=%08x %s_img=%08x %s_smp=%08x %s_srt=%08x", tag,
	            HashWords(r.user_data.data(), r.user_data.size()), tag, HashDescriptors(r.buffers),
	            tag, HashDescriptors(r.images), tag, HashDescriptors(r.samplers), tag,
	            HashWords(r.flattened_srt.data(), r.flattened_srt.size()));
}

// KYTY_DEBUG_DRAW_STATS=1 prints, every 5 s, the draws per second and the rendering restarts and
// barriers they caused, overall and for the pixel shaders causing the most, to find draw patterns
// that stall the GPU (like the sand trail's).
static bool StatsEnabled() {
	static const bool enabled = KYTY_GETENV("KYTY_DEBUG_DRAW_STATS") != nullptr;
	return enabled;
}

struct ShaderTotals {
	uint64_t vertex   = 0;
	uint64_t draws    = 0;
	uint64_t restarts = 0;
	uint64_t barriers = 0;
};

static void Account(uint64_t pixel, uint64_t vertex, uint64_t restarts, uint64_t barriers) {
	static std::unordered_map<uint64_t, ShaderTotals> totals;
	static auto window_start = std::chrono::steady_clock::now();
	auto&       entry        = totals[pixel];
	entry.vertex             = vertex;
	entry.draws++;
	entry.restarts += restarts;
	entry.barriers += barriers;
	const auto now = std::chrono::steady_clock::now();
	if (now - window_start < std::chrono::seconds(5)) {
		return;
	}
	const double seconds = std::chrono::duration<double>(now - window_start).count();
	std::vector<std::pair<uint64_t, ShaderTotals>> rows(totals.begin(), totals.end());
	ShaderTotals all;
	for (const auto& [hash, row]: rows) {
		all.draws += row.draws;
		all.restarts += row.restarts;
		all.barriers += row.barriers;
	}
	std::printf("draw-stats: %.1fs draws/s=%.0f restarts/s=%.0f barriers/s=%.0f shaders=%zu\n",
	            seconds, all.draws / seconds, all.restarts / seconds, all.barriers / seconds,
	            rows.size());
	const auto print_top = [&](const char* order, auto&& key) {
		std::ranges::sort(rows, [&](const auto& a, const auto& b) { return key(a.second) > key(b.second); });
		for (size_t i = 0; i < std::min<size_t>(rows.size(), 6); i++) {
			const auto& [hash, row] = rows[i];
			std::printf("draw-stats   by-%s ps=%016" PRIx64 " vs=%016" PRIx64
			            " draws/s=%.0f restarts/s=%.0f barriers/s=%.0f\n",
			            order, hash, row.vertex, row.draws / seconds, row.restarts / seconds,
			            row.barriers / seconds);
		}
	};
	print_top("stalls", [](const ShaderTotals& row) { return row.restarts + row.barriers; });
	print_top("draws", [](const ShaderTotals& row) { return row.draws; });
	totals.clear();
	window_start = now;
}

} // namespace DrawLog

// Debug: KYTY_DEBUG_DEPTH_OVERRIDE=<pixel shader hash>:<letters>[,...] turns tests off for the
// draws with that pixel shader: d the depth test and write, s the stencil test, b the depth bounds
// test. It tells a defect from rejected fragments (the test's inputs are wrong) from one in the
// fragments themselves.
enum : uint32_t { DepthOverrideDepth = 1, DepthOverrideStencil = 2, DepthOverrideBounds = 4 };

static uint32_t DebugDepthOverride(uint64_t pixel_hash) {
	static const std::vector<std::pair<uint64_t, uint32_t>> entries = [] {
		std::vector<std::pair<uint64_t, uint32_t>> list;
		for (const char* text = KYTY_GETENV("KYTY_DEBUG_DEPTH_OVERRIDE"); text != nullptr;) {
			char*      end  = nullptr;
			const auto hash = std::strtoull(text, &end, 16);
			if (end == text || *end != ':') {
				break;
			}
			uint32_t tests = 0;
			for (text = end + 1; *text != '\0' && *text != ','; text++) {
				tests |= *text == 'd' ? DepthOverrideDepth
				         : *text == 's' ? DepthOverrideStencil
				         : *text == 'b' ? DepthOverrideBounds
				                        : 0u;
			}
			list.emplace_back(hash, tests);
			text = *text == ',' ? text + 1 : nullptr;
		}
		return list;
	}();
	for (const auto& [hash, tests]: entries) {
		if (hash == pixel_hash) {
			return tests;
		}
	}
	return 0;
}

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
					const auto& r  = vs_input_info.resources[ai];
					const auto& rd = vs_input_info.resources_dst[ai];
					if (rd.buffer_index != bi) {
						continue;
					}
					const auto offset = static_cast<uint32_t>(r.Base48() - b.addr);
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < vs_input_info.resources_num; ai++) {
			const auto& r  = vs_input_info.resources[ai];
			const auto& rd = vs_input_info.resources_dst[ai];
			if (rd.buffer_index != bi) {
				continue;
			}
			LOGF("DrawInputState[%u]: attr[%d] offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, static_cast<uint32_t>(r.Base48() - b.addr), rd.register_start,
			     rd.registers_num, rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

// KYTY_DEBUG_RESOLUTION=1 prints, every 3 s, the viewport sizes drawn most often (with the size of
// their render target) and the GPU timestamp scale, to see the render resolution a game with dynamic
// resolution picks.
// The info display's counts (see HudStats): draws, and the viewport size drawn most often in the
// last second that is not square (shadow maps are).
static void HudCountDraw(float width, float height) {
	static std::map<uint64_t, uint64_t> counts;
	static uint64_t                     draws        = 0;
	static auto                         window_start = std::chrono::steady_clock::now();
	const auto w = static_cast<uint32_t>(std::lround(std::fabs(width)));
	const auto h = static_cast<uint32_t>(std::lround(std::fabs(height)));
	draws++;
	if (w >= 640 && h >= 360 && w != h) {
		counts[uint64_t {w} << 32u | h]++;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - window_start < std::chrono::seconds(1)) {
		return;
	}
	const auto top = std::ranges::max_element(
	    counts, [](const auto& a, const auto& b) { return a.second < b.second; });
	g_hud.render_size.store(top != counts.end() ? top->first : 0, std::memory_order_relaxed);
	g_hud.draws_per_second.store(static_cast<uint64_t>(
	                                 static_cast<double>(draws) /
	                                 std::chrono::duration<double>(now - window_start).count()),
	                             std::memory_order_relaxed);
	counts.clear();
	draws        = 0;
	window_start = now;
}

static void DebugResolution(float width, float height, uint32_t target_width,
                            uint32_t target_height) {
	if (g_hud.active.load(std::memory_order_relaxed)) [[unlikely]] {
		HudCountDraw(width, height);
	}
	static const bool enabled = KYTY_GETENV("KYTY_DEBUG_RESOLUTION") != nullptr;
	if (!enabled) [[likely]] {
		return;
	}
	static std::map<std::array<uint32_t, 4>, uint64_t> counts;
	static auto window_start = std::chrono::steady_clock::now();
	const auto  w            = static_cast<uint32_t>(std::lround(std::fabs(width)));
	const auto  h            = static_cast<uint32_t>(std::lround(std::fabs(height)));
	if (w >= 640 && h >= 360) {
		counts[{w, h, target_width, target_height}]++;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - window_start < std::chrono::seconds(3)) {
		return;
	}
	std::vector<std::pair<std::array<uint32_t, 4>, uint64_t>> rows(counts.begin(), counts.end());
	std::ranges::sort(rows, [](const auto& a, const auto& b) { return a.second > b.second; });
	std::printf("resolution: scale=%u%%", Config::GetGpuTimestampScalePercent());
	for (size_t i = 0; i < std::min<size_t>(rows.size(), 5); i++) {
		const auto& [key, count] = rows[i];
		std::printf("  %ux%u in %ux%u: %" PRIu64, key[0], key[1], key[2], key[3], count);
	}
	std::printf("\n");
	std::fflush(stdout);
	counts.clear();
	window_start = now;
}

// KYTY_DEBUG_STATE_FILTER=0 records every draw's full graphics state, for A/B runs.
static bool GraphicsStateFilterEnabled() {
	static const bool enabled = [] {
		const char* text = KYTY_GETENV("KYTY_DEBUG_STATE_FILTER");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	return enabled;
}

static void SetGraphicsDynamicParams(const CommandBuffer& buffer, vk::CommandBuffer vk_buffer,
                                     const ShaderVertexInputInfo& vs_input_info,
                                     const RenderDepthInfo& depth, const RenderState& rendering) {
	KYTY_PROFILER_FUNCTION();

	const auto& ctx = buffer.GetRegisters();
	const auto&        vp  = ctx.GetScreenViewport();
	const vk::Extent2D framebuffer_extent {rendering.width, rendering.height};
	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	DebugResolution(viewports[0].width, viewports[0].height, rendering.width, rendering.height);
	// Consecutive draws mostly repeat their state: only changed values are recorded (see
	// CommandBuffer::GraphicsState).
	auto& known = buffer.GetGraphicsState();
	if (!GraphicsStateFilterEnabled()) {
		buffer.InvalidateGraphicsState();
	}
	static_assert(CommandBuffer::GraphicsState::ViewportSlots == viewport_slots);
	if (known.viewport_count != viewport_count ||
	    !std::equal(viewports.begin(), viewports.begin() + viewport_count,
	                known.viewports.begin())) {
		vk_buffer.setViewportWithCount(viewport_count, viewports.data());
		known.viewport_count = viewport_count;
		known.viewports      = viewports;
	}
	if (known.scissor_count != viewport_count ||
	    !std::equal(scissors.begin(), scissors.begin() + viewport_count, known.scissors.begin())) {
		vk_buffer.setScissorWithCount(viewport_count, scissors.data());
		known.scissor_count = viewport_count;
		known.scissors      = scissors;
	}

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	const vk::Bool32 depth_test  = depth.depth_test_enable ? VK_TRUE : VK_FALSE;
	const vk::Bool32 depth_write = depth.depth_write_enable ? VK_TRUE : VK_FALSE;

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	const vk::Bool32 depth_bias   = depth_bias_enable ? VK_TRUE : VK_FALSE;
	const vk::Bool32 stencil_test = depth.stencil_test_enable ? VK_TRUE : VK_FALSE;

	const bool fixed_valid = known.fixed_valid;
	if (!fixed_valid || known.line_width != line_width) {
		vk_buffer.setLineWidth(line_width);
	}
	if (!fixed_valid || known.blend_constants != blend_constants) {
		vk_buffer.setBlendConstants(blend_constants.data());
	}
	if (!fixed_valid || known.depth_test != depth_test) {
		vk_buffer.setDepthTestEnable(depth_test);
	}
	if (!fixed_valid || known.depth_write != depth_write) {
		vk_buffer.setDepthWriteEnable(depth_write);
	}
	if (!fixed_valid || known.depth_compare != depth.depth_compare_op) {
		vk_buffer.setDepthCompareOp(depth.depth_compare_op);
	}
	if (!fixed_valid || known.depth_bias != depth_bias) {
		vk_buffer.setDepthBiasEnable(depth_bias);
	}
	if (!fixed_valid || known.stencil_test != stencil_test) {
		vk_buffer.setStencilTestEnable(stencil_test);
	}
	if (DynamicDepthBoundsEnabled()) {
		vk_buffer.setDepthBoundsTestEnable(depth.depth_bounds_test_enable ? VK_TRUE : VK_FALSE);
		if (depth.depth_bounds_test_enable) {
			vk_buffer.setDepthBounds(depth.depth_min_bounds, depth.depth_max_bounds);
		}
	}
	known.fixed_valid     = true;
	known.line_width      = line_width;
	known.blend_constants = blend_constants;
	known.depth_test      = depth_test;
	known.depth_write     = depth_write;
	known.depth_compare   = depth.depth_compare_op;
	known.depth_bias      = depth_bias;
	known.stencil_test    = stencil_test;

	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		const std::array bias {constant_factor, poly_offset.clamp, slope_factor};
		if (!known.bias_valid || known.bias != bias) {
			vk_buffer.setDepthBias(constant_factor, poly_offset.clamp, slope_factor);
			known.bias_valid = true;
			known.bias       = bias;
		}
	}

	if (depth.stencil_test_enable) {
		const auto set_stencil = [&](vk::StencilFaceFlagBits face, const vk::StencilOpState& state,
		                             vk::StencilOpState& recorded) {
			if (known.stencil_valid && recorded == state) {
				return;
			}
			vk_buffer.setStencilOp(face, state.failOp, state.passOp, state.depthFailOp, state.compareOp);
			vk_buffer.setStencilCompareMask(face, state.compareMask);
			vk_buffer.setStencilWriteMask(face, state.writeMask);
			vk_buffer.setStencilReference(face, state.reference);
			recorded = state;
		};
		set_stencil(vk::StencilFaceFlagBits::eFront, depth.stencil_front, known.stencil[0]);
		set_stencil(vk::StencilFaceFlagBits::eBack, depth.stencil_back, known.stencil[1]);
		known.stencil_valid = true;
	}

#if defined(__APPLE__)
	// MoltenVK has no VK_EXT_color_write_enable; the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
#else
	std::array<vk::Bool32, RENDER_COLOR_ATTACHMENTS_MAX> enable {};
	for (uint32_t slot = 0; slot < rendering.num_color_attachments; slot++) {
		enable[slot] = rendering.color_attachments[slot].image_view != nullptr;
	}
	if (rendering.num_color_attachments != 0) {
		if (known.color_write_count != rendering.num_color_attachments ||
		    !std::equal(enable.begin(), enable.begin() + rendering.num_color_attachments,
		                known.color_write.begin())) {
			vk_buffer.setColorWriteEnableEXT(rendering.num_color_attachments, enable.data());
			known.color_write_count = rendering.num_color_attachments;
			known.color_write       = enable;
		}
	} else {
		// A pipeline without color attachments keeps color write enables static; binding it
		// replaces the recorded ones.
		known.color_write_count = 0;
	}
#endif
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return vs.es_regs.data_addr != 0;
}

// KYTY_DEBUG_DRAW_REPEAT=1 prints, every 120 flips, how many draws a record kept across frames
// could replay (the Demon's Souls fork's draw records, keyed by the shader addresses and the user
// data registers): the share whose key the previous frame drew, one of the last four frames drew,
// and two of them drew (a record stored on the second sighting), and as an upper bound the share
// whose shader addresses alone the previous frame drew. Unlike KYTY_DEBUG_FRAME_REPEAT it counts
// draws, not stages, and needs no KYTY_DEBUG_DRAW_PHASES.
static bool DrawRepeatEnabled() {
	static const bool enabled = KYTY_GETENV("KYTY_DEBUG_DRAW_REPEAT") != nullptr;
	return enabled;
}

struct DrawRepeat {
	static constexpr size_t Frames = 4;

	std::mutex                                       mutex;
	std::array<std::unordered_set<uint64_t>, Frames> keys;
	std::array<std::unordered_set<uint64_t>, Frames> shaders;
	size_t                                           current         = 0;
	uint64_t                                         draws           = 0;
	uint64_t                                         previous        = 0;
	uint64_t                                         recent          = 0;
	uint64_t                                         second_sighting = 0;
	uint64_t                                         shaders_only    = 0;
	uint64_t                                         flips           = 0;
};
static DrawRepeat g_draw_repeat;

static uint64_t HashUserSgprs(const HW::UserSgprInfo& sgprs, uint64_t hash) {
	for (uint32_t i = 0; i < sgprs.count && i < static_cast<uint32_t>(HW::UserSgprInfo::SGPRS_MAX);
	     i++) {
		hash = (hash ^ sgprs.value[i]) * 0x100000001b3ull;
	}
	return (hash ^ sgprs.count) * 0x100000001b3ull;
}

static void NoteDrawRepeat(const HW::Shader& sh_ctx) {
	const auto& vs = sh_ctx.GetVs();
	const auto& ps = sh_ctx.GetPs();
	uint64_t    shader_key = 0xcbf29ce484222325ull;
	for (const uint64_t addr: {vs.es_regs.data_addr, vs.gs_regs.data_addr, vs.hs_regs.data_addr,
	                           ps.ps_regs.data_addr}) {
		shader_key = (shader_key ^ addr) * 0x100000001b3ull;
	}
	uint64_t key = HashUserSgprs(vs.hs_user_sgpr, shader_key);
	key          = HashUserSgprs(vs.gs_user_sgpr, key);
	key          = HashUserSgprs(ps.ps_user_sgpr, key);

	auto&           state = g_draw_repeat;
	std::lock_guard lock(state.mutex);
	const size_t    last  = (state.current + DrawRepeat::Frames - 1) % DrawRepeat::Frames;
	uint32_t        seen  = 0;
	for (size_t i = 0; i < DrawRepeat::Frames; i++) {
		if (i != state.current && state.keys[i].contains(key)) {
			seen++;
		}
	}
	state.draws++;
	state.previous += state.keys[last].contains(key) ? 1 : 0;
	state.recent += seen != 0 ? 1 : 0;
	state.second_sighting += seen >= 2 ? 1 : 0;
	state.shaders_only += state.shaders[last].contains(shader_key) ? 1 : 0;
	state.keys[state.current].insert(key);
	state.shaders[state.current].insert(shader_key);
}

void DrawRepeatFlip() {
	if (!DrawRepeatEnabled()) {
		return;
	}
	auto&           state = g_draw_repeat;
	std::lock_guard lock(state.mutex);
	if (++state.flips % 120 == 0 && state.draws != 0) {
		const auto percent = [&](uint64_t count) {
			return 100.0 * static_cast<double>(count) / static_cast<double>(state.draws);
		};
		std::printf("draw-repeat: draws/frame=%llu key in previous frame=%.1f%% in last 4 frames=%.1f%% "
		            "in 2 of them=%.1f%% shaders only, previous frame=%.1f%%\n",
		            static_cast<unsigned long long>(state.draws / 120), percent(state.previous),
		            percent(state.recent), percent(state.second_sighting),
		            percent(state.shaders_only));
		std::fflush(stdout);
		state.draws = state.previous = state.recent = state.second_sighting = state.shaders_only = 0;
	}
	state.current = (state.current + 1) % DrawRepeat::Frames;
	state.keys[state.current].clear();
	state.shaders[state.current].clear();
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop;
}

// The constructor leaves vertex_info unconstructed: GetGraphicsPrograms constructs the entries it
// uses (ResetVertexInputInfo) before any use. Constructing all three on every draw cost the GPU
// thread about 1.5% in Sky Garden. Likewise only color_info's first color_constructed entries are
// constructed (PrepareDrawRenderState constructs each before resolving a target into it): a draw
// uses one or two of the eight, and each carries a whole image description.
struct DrawRenderState {
	DrawRenderState() { std::construct_at(&color_info[0]); }
	RenderDepthInfo       depth_info {};
	union {
		RenderColorInfo color_info[RENDER_COLOR_ATTACHMENTS_MAX];
	};
	uint32_t              color_constructed = 1;
	uint32_t              color_count       = 0;
	bool                  ps_active         = true;
	union {
		std::array<ShaderVertexInputInfo, 3> vertex_info;
	};
	ShaderPixelInputInfo  ps_input_info {};
	PipelineCache::GraphicsPrograms programs {};
};
static_assert(std::is_trivially_destructible_v<ShaderVertexInputInfo>);
static_assert(std::is_trivially_destructible_v<RenderColorInfo>);

struct DrawCallInfo {
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;
	// Guest address of GPU-written DrawIndexedIndirectArgs. When set, the counts above are
	// placeholders and the mesh draw is built on the GPU (see MeshIndirectArgs).
	uint64_t             indirect_args  = 0;

	[[nodiscard]] bool IsIndexed() const { return debug_op == CommandBufferDebugOp::DrawIndex; }
	[[nodiscard]] const char* Name() const { return IsIndexed() ? "DrawIndex" : "DrawIndexAuto"; }
};

// KYTY_VERIFY_DEPTH_REUSE=1: a draw that keeps its depth target's view acquires it anyway and
// reports what the full acquisition changed or returned differently (see IsDepthTargetCurrent).
static bool VerifyDepthReuse() {
	static const bool enabled = KYTY_GETENV("KYTY_VERIFY_DEPTH_REUSE") != nullptr;
	return enabled;
}

static void CheckDepthReuse(TextureCache& cache, const RenderDepthInfo& depth,
                            vk::ImageView kept) {
	const auto& image           = cache.GetImage(depth.image_id);
	const bool  gpu_modified    = image.IsGpuModified();
	const bool  depth_target    = image.usage.depth_target;
	const bool  buffer_modified = image.IsBufferModified();
	const bool  cpu_dirty       = image.IsCpuDirty();
	const auto  stencil         = image.info.stencil;
	const auto  metadata        = image.info.metadata;
	const auto  meta_generation = cache.SurfaceMetaGeneration();
	const auto  set_generation  = cache.ImageGeneration(depth.image_id);
	const auto  view            = cache.FindDepthTarget(depth.image_id, depth.desc);
	const auto& after           = cache.GetImage(depth.image_id);
	const char* field           = nullptr;
	if (view != kept) {
		field = "view";
	} else if (after.IsGpuModified() != gpu_modified || after.usage.depth_target != depth_target) {
		field = "gpu-modified or usage";
	} else if (after.IsBufferModified() != buffer_modified || after.IsCpuDirty() != cpu_dirty) {
		field = "dirty state";
	} else if (!(after.info.stencil == stencil) || !(after.info.metadata == metadata)) {
		field = "stencil or metadata";
	} else if (cache.SurfaceMetaGeneration() != meta_generation ||
	           cache.ImageGeneration(depth.image_id) != set_generation) {
		field = "surface metadata or image set";
	}
	static std::atomic<uint64_t> checked {0};
	static std::atomic<uint64_t> missed {0};
	const auto count = checked.fetch_add(1, std::memory_order_relaxed) + 1;
	if (field != nullptr && missed.fetch_add(1, std::memory_order_relaxed) < 32) {
		std::printf("depth-reuse verify: 0x%016" PRIx64 " kept view would have missed a change: %s\n",
		            image.info.data.address, field);
	}
	if (count % 100000 == 0) {
		std::printf("depth-reuse verify: reuses=%" PRIu64 " missed=%" PRIu64 "\n", count,
		            missed.load(std::memory_order_relaxed));
		std::fflush(stdout);
	}
}

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 vk::ImageAspectFlags& feedback_aspects,
                                                 std::span<PreparedBindings* const> stages) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	feedback_aspects = {};
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto owner = cache.m_slot_images.try_get(target.image_id);
		if (owner == nullptr || (!owner->registered && !owner->info.data.Empty()) ||
		    owner->binding.needs_rebind) {
			EXIT("color target changed after render-state discovery\n");
		}
		// A clean target the previous draw acquired keeps its view (see IsRenderTargetCurrent).
		auto&         acquired = m_color_target_sources.at(target.target_slot);
		vk::ImageView image_view;
		if (TargetReuseEnabled() && acquired.view_image == target.image_id &&
		    acquired.view_info == target.desc.view_info &&
		    cache.IsRenderTargetCurrent(target.image_id, acquired.view_generation)) {
			image_view = acquired.view;
		} else {
			const auto generation    = cache.ImageGeneration(target.image_id);
			image_view               = cache.FindRenderTarget(target.image_id, target.desc);
			acquired.view            = image_view;
			acquired.view_image      = target.image_id;
			acquired.view_info       = target.desc.view_info;
			acquired.view_generation = generation;
		}
		auto& image = cache.GetImage(target.image_id);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		state.num_color_attachments = std::max(state.num_color_attachments, target.target_slot + 1);
		auto& attachment            = state.color_attachments[target.target_slot];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		// A clean depth target the previous draw acquired keeps its view (see
		// IsDepthTargetCurrent). KYTY_DEBUG_AB=depthreuse acquires it every draw in every other
		// window.
		static const bool depth_ab = AbSelected("depthreuse");
		auto&             acquired = m_depth_target_source;
		vk::ImageView     image_view;
		if (TargetReuseEnabled() && !(depth_ab && AbFeatureOff()) &&
		    acquired.view_image == depth.image_id && acquired.view_info == depth.desc.view_info &&
		    cache.IsDepthTargetCurrent(depth.image_id, acquired.view_generation,
		                               acquired.view_meta_generation, depth.desc)) {
			image_view = acquired.view;
			if (VerifyDepthReuse()) [[unlikely]] {
				CheckDepthReuse(cache, depth, image_view);
			}
		} else {
			// Both generations from before the acquisition: its own changes make the next draw
			// acquire again rather than trust a state it did not check.
			const auto generation      = cache.ImageGeneration(depth.image_id);
			const auto meta_generation = cache.SurfaceMetaGeneration();
			image_view                 = cache.FindDepthTarget(depth.image_id, depth.desc);
			acquired.view              = image_view;
			acquired.view_image        = depth.image_id;
			acquired.view_info         = depth.desc.view_info;
			acquired.view_generation   = generation;
			acquired.view_meta_generation = meta_generation;
		}
		const auto& metadata   = depth.desc.info.metadata;
		if (TextureCache::MetaClearLogEnabled()) [[unlikely]] {
			TextureCache::t_meta_clear_source = {
			    "depth target DB clear", m_context.GetGraphics().presented_frames.load(std::memory_order_relaxed),
			    0};
		}
		if (metadata.kind == ImageMetadataKind::Htile && depth.depth_clear_enable &&
		    !cache.ClearMeta(metadata.range.address)) {
			EXIT("failed to acquire HTile metadata for a depth clear\n");
		}
		const bool meta_clear =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.depth_load_clear_enable = depth.depth_clear_enable || meta_clear;
		if (meta_clear && !depth.depth_clear_enable && TextureCache::MetaClearLogEnabled()) [[unlikely]] {
			TextureCache::t_meta_clear_source.source = "pending metadata clear";
			TextureCache::LogMetaClear("depth bind becomes LoadOpClear", metadata.range.address,
			                           static_cast<int32_t>(depth.desc.view_info.base_layer),
			                           std::bit_cast<uint32_t>(depth.depth_clear_value), // clear value bits
			                           depth.desc.info.data.address);
		}
		if (meta_clear &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		const auto draw_writes = depth.AttachmentWriteAspects();
		vk::ImageAspectFlags sampled_aspects;
		for (const auto* stage: stages) {
			for (const auto& binding: stage->images) {
				if (binding.image_id != depth.image_id ||
				    binding.desc.type != TextureCache::BindingType::Texture) continue;
				const auto native =
				    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
				EXIT_IF(native == image.views.end());
				sampled_aspects |= native->info.aspect;
				feedback_aspects |= DepthFeedbackAspects(draw_writes, depth.desc.view_info,
				                                         native->info);
			}
		}
		if (feedback_aspects && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			EXIT("depth attachment feedback loop is not supported by the host\n");
		}
		auto layout = depth_attachment_layout(depth);
		if (sampled_aspects & ~DepthReadableAspects(layout)) {
			layout = m_context.GetGraphics().attachment_feedback_loop_enabled
			             ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
			             : vk::ImageLayout::eGeneral;
		}
		// The attachment store writes even when guest depth/stencil tests do not.
		auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		              vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		// A draw that samples the depth image without writing what it samples also reads it as a
		// texture: request that access now, as the descriptors will. Otherwise the state flips
		// between attachment and attachment-plus-shader-read, and each flip records a barrier and
		// ends the rendering instance, twice per draw (Astro Bot draws its sand trail as hundreds of
		// two-point draws a frame that sample depth). Entering this state from any other still
		// records the barrier that orders earlier depth writes before the reads.
		if (sampled_aspects && !feedback_aspects) {
			access |= vk::AccessFlagBits2::eShaderRead;
		}
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

// Slots the render target and shader masks let a draw write, whatever their export format.
static uint32_t DrawColorWriteMask(const HW::Context& ctx) {
	const auto& sh_regs    = ctx.GetShaderRegisters();
	const auto  write_mask = ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask;
	uint32_t    mask       = 0;
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if (render_target_mask_slot(write_mask, slot) != 0) {
			mask |= 1u << slot;
		}
	}
	return mask;
}

// The writable slots that also have a nonzero SPI_SHADER_COL_FORMAT: whether the pixel shader
// runs for its colour outputs, and which export mappings it is compiled with.
static uint32_t DrawColorOutputMask(const HW::Context& ctx) {
	const auto& sh_regs     = ctx.GetShaderRegisters();
	const auto  write_mask  = DrawColorWriteMask(ctx);
	uint32_t    output_mask = 0;
	for (uint32_t index = 0; index < RENDER_COLOR_ATTACHMENTS_MAX; index++) {
		const auto slot = ShaderPixelExportTarget(sh_regs.m_cbShaderMask, index);
		if (slot >= RENDER_COLOR_ATTACHMENTS_MAX) {
			break;
		}
		if (sh_regs.target_output_mode[index] != 0 &&
		    (write_mask & (1u << slot)) != 0) {
			output_mask |= 1u << slot;
		}
	}
	return output_mask;
}

// Slots that write the same surface as a lower writable slot, each with its own channels: ASTRO
// BOT's foliage interaction stamps write RG (blended) through MRT0 and BA through MRT1 of one
// surface. Vulkan leaves two attachments on one image undefined (NVIDIA lost the stamps), so such
// a draw runs once per slot, each pass binding one attachment and keeping its blend and mask.
// KYTY_DEBUG_MRT_SPLIT=0 binds them together as before.
static uint32_t AliasedColorSlots(const HW::Context& ctx) {
	static const bool enabled = [] {
		const char* text = KYTY_GETENV("KYTY_DEBUG_MRT_SPLIT");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	if (!enabled) {
		return 0;
	}
	const auto writable = DrawColorWriteMask(ctx);
	uint32_t   aliased  = 0;
	for (uint32_t later = 1; later < RENDER_COLOR_ATTACHMENTS_MAX; later++) {
		if ((writable & (1u << later)) == 0) {
			continue;
		}
		const auto later_addr = ctx.GetRenderTarget(later).base.addr;
		const auto later_mask = render_target_mask_slot(ctx.GetRenderTargetMask(), later);
		for (uint32_t first = 0; first < later; first++) {
			if ((writable & (1u << first)) != 0 && later_addr != 0 &&
			    ctx.GetRenderTarget(first).base.addr == later_addr &&
			    (render_target_mask_slot(ctx.GetRenderTargetMask(), first) & later_mask) == 0) {
				aliased |= 1u << later;
				break;
			}
		}
	}
	return aliased;
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

// KYTY_CB_METADATA_MATERIALIZE=0 drops fast-clear-eliminate and DCC-decompress operations as before.
static bool MetadataColorMaterializeEnabled() {
	static const bool enabled = [] {
		const char* value = KYTY_GETENV("KYTY_CB_METADATA_MATERIALIZE");
		return value == nullptr || std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

bool RenderExecutor::ConsumeMetadataColorOperation(CommandBuffer& buffer,
                                                   uint32_t       render_target_slice_offset) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	// These special modes run color-buffer metadata or decompression operations. The shader is a
	// vehicle for that operation, and its exported color must not be applied as a normal draw.
	// Kyty stores expanded Vulkan images rather than compressed guest surfaces, so no equivalent
	// hardware pass is emitted.
	if (mode != static_cast<uint8_t>(CbColorMode::EliminateFastClear) &&
	    mode != static_cast<uint8_t>(CbColorMode::FmaskDecompress) &&
	    mode != static_cast<uint8_t>(CbColorMode::DccDecompress)) {
		return false;
	}
	// On hardware an eliminate or decompress writes the cleared values into the surface, for
	// readers that carry no metadata (a texture without CMASK or DCC, buffer reads). A pending
	// fast clear is otherwise only written when a draw binds the target, so a surface that is
	// cleared, eliminated and sampled without a draw in between would show its old contents
	// (ASTRO BOT: interaction and water inputs). Resolving the target as an attachment writes it.
	if (mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	    !MetadataColorMaterializeEnabled()) {
		return true;
	}
	const auto target_mask = ctx.GetRenderTargetMask();
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if (rt.base.addr == 0 || (slot != 0 && render_target_mask_slot(target_mask, slot) == 0)) {
			continue;
		}
		const bool dcc   = rt.info.dcc_compression_enable && rt.dcc_addr.addr != 0;
		const bool cmask = mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) &&
		                   rt.info.cmask_fast_clear_enable && rt.cmask.addr != 0;
		if ((dcc || cmask) && rt.attrib3.tile_mode == Prospero::TileMode::kRenderTarget &&
		    rt.attrib.num_samples == 0 && rt.attrib.num_fragments == 0) {
			RenderColorInfo target {};
			ResolveRenderColorTarget(buffer, target, render_target_slice_offset, slot, true, false);
			if (StampTraceEnabled() && target.image_id) [[unlikely]] {
				StampTraceEvent('E', target.desc.info.data.address);
			}
		}
	}
	return true;
}

struct DrawEmitInfo {
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(int binding, const ShaderVertexInputInfo& info) {
	const auto& buffer = info.buffers[binding];
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < info.resources_num; i++) {
		if (info.resources_dst[i].buffer_index != binding) {
			continue;
		}
		const auto& resource = info.resources[i];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? resource.Base48() - buffer.addr +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

// Trivial on purpose: AcquireVertexBuffers keeps RES_MAX of them on the stack and writes only
// the ones it uses (zeroing all of them showed as memset on the GPU thread).
struct VertexBufferRange {
	uint64_t base_address;
	uint64_t requested_end;
	uint64_t acquired_end;
	Buffer*  buffer;
	uint64_t buffer_offset;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	// Only the first `count` entries are set.
	std::array<vk::Buffer, MaxBuffers>     buffers;
	std::array<vk::DeviceSize, MaxBuffers> offsets;
	std::array<vk::DeviceSize, MaxBuffers> sizes;
	uint32_t                               count = 0;
};

// Fills `prepared` in place: it is three RES_MAX arrays, which a returned copy moved per draw.
static void AcquireVertexBuffers(CommandBuffer& buffer, const ShaderVertexInputInfo& vs_input_info,
                                 PreparedVertexBuffers& prepared) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<uint64_t, ShaderVertexInputInfo::RES_MAX>          sizes;
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges;
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vs_input_info);
		sizes[i]           = size;
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size, 0, nullptr, 0};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges;
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end, 0, nullptr, 0};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		const auto [host, offset] = cache.ObtainBuffer(range.base_address, size, false);
		range.buffer        = host;
		range.buffer_offset = offset;
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = sizes[i];
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			prepared.sizes[i]   = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->buffer->Handle();
		prepared.offsets[i] = range->buffer_offset + vertex.addr - range->base_address;
		prepared.sizes[i]   = std::min(size, range->acquired_end - vertex.addr);
	}
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kPolygon:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kPatch:
			if (!Config::TessellationEnabled()) return false;
			[[fallthrough]];
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kRectListLegacy:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer,
                                    const DrawIndexBufferSource& source) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}

	const auto element_size = source.guest_element_size;
	const auto index_mask   = UINT32_MAX >> ((4 - element_size) * 8);
	const auto reset_index  = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	const auto restart_index = reset_index & index_mask;
	if (restart_index == index_mask) {
		// Use native restart; the 8-bit path widens its marker to 0xffff.
		return true;
	}

	// A game can set a custom reset value without using it in the index buffer.
	// Keep restart off in that case; fail if we actually find the value.
	// Scan before preparing draw resources: readback can restart the command buffer.
	EXIT_NOT_IMPLEMENTED(source.address == 0);
	const auto* indices = reinterpret_cast<const uint8_t*>(source.address);
	for (uint64_t offset = 0; offset < source.size; offset += element_size) {
		uint32_t index = 0;
		std::memcpy(&index, indices + offset, element_size);
		EXIT_NOT_IMPLEMENTED(index == restart_index);
	}
	return false;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw,
                           uint32_t color_output_mask, DrawRenderState& state) {
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		const auto& rt = ctx.GetRenderTarget(slot);
		if ((color_output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			target_export_mapping[slot] =
			    TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
			                                 rt.info.channel_order)
			        .export_mapping;
		}
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "GetGraphicsPrograms");
	}
	DrainStats::SlowLookupTimer compile_timer(DrainStats::Kind::ShaderCompile);
	g_draw_phases.Mark(DrawPhaseTimer::Setup);
	state.programs = pipeline_cache.GetGraphicsPrograms(
	    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
	    target_export_mapping, state.ps_active, state.vertex_info, state.ps_input_info,
	    Config::AsyncPipelinesEnabled() ? ProgramWait::Defer : ProgramWait::Wait);
}

// KYTY_DRAW_CHAIN=0 resolves every indexed draw's targets; KYTY_DEBUG_AB=chain alternates.
static bool DrawChainEnabled() {
	static const bool enabled = [] {
		const char* text = KYTY_GETENV("KYTY_DRAW_CHAIN");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	static const bool ab = AbSelected("chain");
	return enabled && !(ab && AbFeatureOff());
}

// KYTY_VERIFY_DRAW_CHAIN=1: a draw taking the chain's targets also resolves them and reports
// what came out differently.
static bool VerifyDrawChain() {
	static const bool enabled = KYTY_GETENV("KYTY_VERIFY_DRAW_CHAIN") != nullptr;
	return enabled;
}

static void ReportDrawChainCheck(const DrawRenderState& taken, const DrawRenderState& resolved) {
	const char* field = nullptr;
	if (taken.color_count != resolved.color_count) {
		field = "color count";
	}
	for (uint32_t i = 0; field == nullptr && i < taken.color_count; i++) {
		const auto& a = taken.color_info[i];
		const auto& b = resolved.color_info[i];
		if (a.image_id != b.image_id || a.target_slot != b.target_slot ||
		    !(a.desc.view_info == b.desc.view_info) || !(a.desc.info.data == b.desc.info.data) ||
		    a.guest_mip_level != b.guest_mip_level || a.guest_array_layer != b.guest_array_layer) {
			field = "color target";
		}
	}
	const auto& a = taken.depth_info;
	const auto& b = resolved.depth_info;
	if (field == nullptr &&
	    (a.image_id != b.image_id || (a.image_id && !(a.desc.view_info == b.desc.view_info)) ||
	     a.depth_test_enable != b.depth_test_enable || a.depth_write_enable != b.depth_write_enable ||
	     a.depth_compare_op != b.depth_compare_op || a.depth_clear_enable != b.depth_clear_enable ||
	     a.depth_clear_value != b.depth_clear_value ||
	     a.depth_bounds_test_enable != b.depth_bounds_test_enable ||
	     a.stencil_test_enable != b.stencil_test_enable ||
	     a.stencil_clear_enable != b.stencil_clear_enable || !(a.stencil_front == b.stencil_front) ||
	     !(a.stencil_back == b.stencil_back))) {
		field = "depth target";
	}
	static std::atomic<uint64_t> checked {0};
	static std::atomic<uint64_t> missed {0};
	const auto                   count = checked.fetch_add(1, std::memory_order_relaxed) + 1;
	if (field != nullptr && missed.fetch_add(1, std::memory_order_relaxed) < 32) {
		std::printf("draw-chain verify: taken targets differ: %s\n", field);
	}
	if (count % 100000 == 0) {
		std::printf("draw-chain verify: draws=%" PRIu64 " missed=%" PRIu64 "\n", count,
		            missed.load(std::memory_order_relaxed));
		std::fflush(stdout);
	}
}

// Records the targets an indexed draw resolved, when each came from a lookup ResolveRender*Target
// can repeat (its source has a generation and holds the same image).
void RenderExecutor::RecordDrawChain(uint32_t slice_offset, uint32_t mrt_mask,
                                     const DrawRenderState& state) {
	auto& chain = m_draw_chain;
	chain.valid = false;
	if (!DrawChainEnabled()) {
		return;
	}
	for (uint32_t i = 0; i < state.color_count; i++) {
		const auto& target = state.color_info[i];
		const auto& source = m_color_target_sources.at(target.target_slot);
		if (source.generation == 0 || source.target.image_id != target.image_id ||
		    (source.overlap && !TextureCache::OverlapTargetsEnabled())) {
			return;
		}
		chain.colors[i]            = target;
		chain.color_base_layers[i] = source.metadata_base_layer;
	}
	if (state.depth_info.image_id) {
		const auto& source = m_depth_target_source;
		if (source.generation == 0 || source.image_id != state.depth_info.image_id ||
		    (source.overlap && !TextureCache::OverlapTargetsEnabled())) {
			return;
		}
		chain.depth_base_layer = source.metadata_base_layer;
	}
	chain.depth        = state.depth_info;
	chain.color_count  = state.color_count;
	chain.cx_writes    = HW::g_context_register_writes;
	chain.slice_offset = slice_offset;
	chain.color_filter = m_color_slot_filter;
	chain.mrt_mask     = mrt_mask;
	chain.targets      = m_context.GetTextureCache().CurrentTargetState();
	chain.valid        = true;
}

// The chain's targets for a draw after it: no context register changed, so the same registers
// would resolve again, and with the same target state (no image registered or unregistered, the
// same tick and GC tick) every lookup would find the same image and touch nothing; only a color
// clear could still have to be applied (IsColorClearCurrent).
static bool ChainStatsEnabled();

// KYTY_DEBUG_CHAIN_STATS=1: why TakeDrawChain refused, every 5 s ("chain-refused").
static void NoteChainOutcome(uint32_t reason) {
	static constexpr std::array<const char*, 9> Names {"taken",   "invalid", "cx-write", "slots",
	                                                   "image-set", "tick",  "gc-tick",  "clear",
	                                                   "disabled"};
	static std::array<uint64_t, Names.size()> counts {};
	static auto                               start = std::chrono::steady_clock::now();
	counts[reason]++;
	if (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
		return;
	}
	start = std::chrono::steady_clock::now();
	uint64_t total = 0;
	for (const auto n: counts) {
		total += n;
	}
	std::string line = fmt::format("chain-refused: draws={}", total);
	for (size_t i = 0; i < Names.size(); i++) {
		line += fmt::format(" {}%={:.1f}", Names[i], 100.0 * static_cast<double>(counts[i]) / static_cast<double>(total));
	}
	std::printf("%s\n", line.c_str());
	std::fflush(stdout);
	counts.fill(0);
}

bool RenderExecutor::TakeDrawChain(uint32_t slice_offset, uint32_t mrt_mask, DrawRenderState& state) {
	auto&      chain = m_draw_chain;
	const bool stats = g_draw_phases.Active() && ChainStatsEnabled();
	const auto refuse = [&](uint32_t reason) {
		if (stats) [[unlikely]] {
			NoteChainOutcome(reason);
		}
		return false;
	};
	if (!DrawChainEnabled()) {
		return refuse(8);
	}
	if (!chain.valid) {
		return refuse(1);
	}
	if (chain.cx_writes != HW::g_context_register_writes) {
		return refuse(2);
	}
	if (chain.slice_offset != slice_offset || chain.color_filter != m_color_slot_filter ||
	    chain.mrt_mask != mrt_mask) {
		return refuse(3);
	}
	auto& cache = m_context.GetTextureCache();
	if (const auto current = cache.CurrentTargetState(); !(current == chain.targets)) {
		return refuse(current.image_set != chain.targets.image_set ? 4
		              : current.tick != chain.targets.tick         ? 5
		                                                           : 6);
	}
	for (uint32_t i = 0; i < chain.color_count; i++) {
		const auto& target = chain.colors[i];
		if (!cache.IsColorClearCurrent(target.image_id, target.desc, chain.color_base_layers[i])) {
			return refuse(7);
		}
	}
	if (chain.depth.image_id &&
	    !cache.IsColorClearCurrent(chain.depth.image_id, chain.depth.desc, chain.depth_base_layer)) {
		return refuse(7);
	}
	if (stats) [[unlikely]] {
		NoteChainOutcome(0);
	}
	for (uint32_t i = 0; i < chain.color_count; i++) {
		if (i == state.color_constructed) {
			std::construct_at(&state.color_info[state.color_constructed++]);
		}
		state.color_info[i] = chain.colors[i];
		BindRenderTarget(chain.colors[i].image_id);
	}
	state.color_count = chain.color_count;
	state.depth_info  = chain.depth;
	if (chain.depth.image_id) {
		BindRenderTarget(chain.depth.image_id);
	}
	return true;
}

bool RenderExecutor::PrepareDrawRenderState(CommandBuffer& buffer, const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
	                                        DrawRenderState& state) {
	const auto& shader_regs       = buffer.GetRegisters().GetShaderRegisters();
	const auto  color_output_mask = DrawColorOutputMask(buffer.GetRegisters());
	state.ps_active = buffer.GetShaders().GetPs().ps_regs.data_addr != 0 &&
	                  (color_output_mask != 0 ||
	                   PixelShaderHasDepthOrCoverageSideEffects(shader_regs));
	RefreshShaders(buffer, draw, color_output_mask, state);
	if (state.programs.pending) {
		// Asynchronous pipelines: a shader is still translating; skip the draw until it is ready.
		return false;
	}
	// A stage the recompiler skipped (unsupported instruction or resources) has no program: skip
	// the draw.
	for (uint32_t stage = 0; stage < state.programs.VertexStageCount(); stage++) {
		if (!state.programs.vertex[stage]) {
			return false;
		}
	}
	if (state.ps_active && !state.programs.pixel) {
		return false;
	}
	if (DebugSkipShader(state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0,
	                    DebugShaderKind::Pixel, draw.index_count > 6 || draw.indirect_args != 0) ||
	    DebugSkipShader(state.vertex_info[0].stage.program->shader_hash, DebugShaderKind::Vertex))
	    [[unlikely]] {
		return false;
	}
	uint32_t mrt_mask = 0;
	if (state.ps_active) {
		for (const auto& output: state.ps_input_info.stage.program->info.outputs) {
			if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt) {
				mrt_mask |= 1u << output.location;
			}
		}
	}
	// Attachments follow the write masks, not the export format: in Astro Bot's Sky Garden the tall
	// grass beside the path (an alpha-tested shader, so it runs anyway) writes a slot whose format
	// register reads 0 at the draw, and binding only formatted slots dropped that grass.
	// The hardware ID buffer slot has no shader export but is written (see id_buffer_slot).
	const uint32_t id_slot_mask = state.ps_active && state.ps_input_info.id_buffer_slot != UINT32_MAX
	                                  ? 1u << state.ps_input_info.id_buffer_slot
	                                  : 0u;
	mrt_mask &= (DrawColorWriteMask(buffer.GetRegisters()) | id_slot_mask) & m_color_slot_filter;
	// KYTY_DEBUG_MRT_SHADER=<pixel shader hash>: print that shader's colour slots (target address,
	// target and shader write masks, export format, blend) for its first 16 draws, to see how a draw
	// that binds one surface to several slots splits the channels.
	static const uint64_t mrt_shader = [] {
		const char* text = KYTY_GETENV("KYTY_DEBUG_MRT_SHADER");
		return text != nullptr ? std::strtoull(text, nullptr, 16) : uint64_t {0};
	}();
	if (mrt_shader != 0 && state.ps_active &&
	    state.ps_input_info.stage.program->shader_hash == mrt_shader) [[unlikely]] {
		static std::atomic<uint32_t> printed {0};
		if (printed.fetch_add(1) < 16) {
			const auto& ctx     = buffer.GetRegisters();
			const auto& sh_regs = ctx.GetShaderRegisters();
			std::string outputs;
			for (const auto& output: state.ps_input_info.stage.program->info.outputs) {
				if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt) {
					outputs += fmt::format(" mrt{}", output.index);
				}
			}
			std::printf("mrt-shader: ps=%016" PRIx64 " rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
			            " bound=0x%02" PRIx32 " exports:%s\n",
			            mrt_shader, ctx.GetRenderTargetMask(), sh_regs.m_cbShaderMask, mrt_mask,
			            outputs.c_str());
			for (uint32_t slot = 0; slot < 8; slot++) {
				const auto& rt = ctx.GetRenderTarget(slot);
				const auto& bc = ctx.GetBlendControl(slot);
				if (rt.base.addr == 0 && sh_regs.target_output_mode[slot] == 0) {
					continue;
				}
				std::printf("mrt-shader:   slot=%u addr=0x%010" PRIx64 " target_mask=0x%x shader_mask=0x%x"
				            " out_mode=%u fmt=%u nfmt=%u order=%u %ux%u blend=%d src=%u dst=%u op=%u"
				            " fast_clear=%d dcc=%d cmask=0x%010" PRIx64 " dcc_addr=0x%010" PRIx64
				            " clear=%08x/%08x color_mode=%u\n",
				            slot, rt.base.addr, render_target_mask_slot(ctx.GetRenderTargetMask(), slot),
				            render_target_mask_slot(sh_regs.m_cbShaderMask, slot),
				            sh_regs.target_output_mode[slot], static_cast<uint32_t>(rt.info.format),
				            static_cast<uint32_t>(rt.info.channel_type),
				            static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
				            rt.attrib2.height + 1, bc.enable ? 1 : 0, bc.color_srcblend,
				            bc.color_destblend, bc.color_comb_fcn, rt.info.cmask_fast_clear_enable ? 1 : 0,
				            rt.info.dcc_compression_enable ? 1 : 0, rt.cmask.addr, rt.dcc_addr.addr,
				            rt.clear_word0.word0, rt.clear_word1.word1,
				            static_cast<uint32_t>(ctx.GetColorControl().mode));
			}
			std::fflush(stdout);
			if (printed.load() <= 3) {
				DebugDumpRecentPm4();
			}
		}
	}
	const auto resolve_targets = [&](DrawRenderState& out) {
		if (draw.IsIndexed()) {
			LogDrawPhase(draw.Name(), "ResolveRenderColorTarget");
		}
		for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
			if ((mrt_mask & (1u << slot)) != 0) {
				if (out.color_count == out.color_constructed) {
					std::construct_at(&out.color_info[out.color_constructed++]);
				}
				ResolveRenderColorTarget(buffer, out.color_info[out.color_count],
				                         render_target_slice_offset, slot);
				if (out.color_info[out.color_count].image_id) {
					out.color_count++;
				}
			}
		}
		if (draw.IsIndexed()) {
			LogDrawPhase(draw.Name(), "ResolveRenderDepthTarget");
		}
		ResolveRenderDepthTarget(buffer, out.depth_info);
	};
	if (draw.IsIndexed() && TakeDrawChain(render_target_slice_offset, mrt_mask, state)) {
		if (VerifyDrawChain()) [[unlikely]] {
			auto check = std::make_unique<DrawRenderState>();
			resolve_targets(*check);
			ReportDrawChainCheck(state, *check);
		}
	} else {
		resolve_targets(state);
		if (draw.IsIndexed()) {
			RecordDrawChain(render_target_slice_offset, mrt_mask, state);
		}
	}
	if (const auto tests = DebugDepthOverride(
	        state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0);
	    tests != 0) [[unlikely]] {
		auto& depth = state.depth_info;
		if ((tests & DepthOverrideDepth) != 0) {
			depth.depth_test_enable  = false;
			depth.depth_write_enable = false;
		}
		if ((tests & DepthOverrideStencil) != 0) {
			depth.stencil_test_enable = false;
		}
		if ((tests & DepthOverrideBounds) != 0) {
			depth.depth_bounds_test_enable = false;
		}
	}
	g_draw_phases.Mark(DrawPhaseTimer::Targets);

	// KYTY_DEBUG_DEPTHONLY=1: one line per distinct depth-only draw setup (no colour output or no
	// pixel shader bound on the host): whether the guest set a pixel shader at all, its kill and
	// export controls, and the alpha-to-mask register (not emulated). Ghost of Yotei: grass tufts
	// in the sun shadow cascade drawn as solid blocks.
	static const bool depthonly_log = KYTY_GETENV("KYTY_DEBUG_DEPTHONLY") != nullptr;
	if (depthonly_log && state.depth_info.image_id && (!state.ps_active || color_output_mask == 0))
	    [[unlikely]] {
		const auto& ctx        = buffer.GetRegisters();
		const auto& db         = shader_regs.db_shader_control;
		const auto  guest_ps   = buffer.GetShaders().GetPs().ps_regs.data_addr;
		const auto  ps_hash    = state.ps_active && state.ps_input_info.stage.program != nullptr
		                             ? state.ps_input_info.stage.program->shader_hash
		                             : 0;
		const auto  vs_hash    = state.vertex_info[0].stage.program != nullptr
		                             ? state.vertex_info[0].stage.program->shader_hash
		                             : 0;
		uint32_t    out_modes  = 0;
		for (uint32_t i = 0; i < 8; i++) {
			out_modes |= (static_cast<uint32_t>(shader_regs.target_output_mode[i]) & 0xfu) << (i * 4u);
		}
		static std::mutex                   depthonly_mutex;
		static std::unordered_set<uint64_t> depthonly_seen;
		uint64_t key = 0xcbf29ce484222325ull;
		for (const uint64_t part: {guest_ps, ps_hash, vs_hash, uint64_t {db.shader_kill_enable},
		                           uint64_t {db.shader_z_export_enable}, uint64_t {ctx.GetRenderTargetMask()},
		                           uint64_t {ctx.GetColorControl().mode}, uint64_t {out_modes},
		                           uint64_t {ctx.GetAlphaToMaskRaw()},
		                           state.depth_info.desc.info.data.address}) {
			key = (key ^ part) * 0x100000001b3ull;
		}
		const std::lock_guard lock(depthonly_mutex);
		if (depthonly_seen.size() < 4096 && depthonly_seen.insert(key).second) {
			std::printf("depthonly: depth=0x%016llx guest_ps=0x%llx host_ps=%016llx ps_active=%d "
			            "vs=%016llx kill=%d z_export=%d mask_export=%d exec_noop=%d a2m_disable=%d "
			            "z_behavior=%u cb_target_mask=0x%08x cb_shader_mask=0x%08x color_mode=%u "
			            "col_format=0x%08x db_alpha_to_mask=0x%08x color_outputs=0x%x\n",
			            static_cast<unsigned long long>(state.depth_info.desc.info.data.address),
			            static_cast<unsigned long long>(guest_ps), static_cast<unsigned long long>(ps_hash),
			            state.ps_active ? 1 : 0, static_cast<unsigned long long>(vs_hash),
			            db.shader_kill_enable ? 1 : 0, db.shader_z_export_enable ? 1 : 0,
			            db.shader_mask_export_enable ? 1 : 0, db.shader_execute_on_noop ? 1 : 0,
			            db.alpha_to_mask_disable ? 1 : 0, static_cast<uint32_t>(db.shader_z_behavior),
			            ctx.GetRenderTargetMask(), shader_regs.m_cbShaderMask,
			            static_cast<uint32_t>(ctx.GetColorControl().mode), out_modes,
			            ctx.GetAlphaToMaskRaw(), color_output_mask);
			std::fflush(stdout);
		}
	}
	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		// Guest descriptor bounds must survive allocation merging in the cache.
		vk_buffer.bindVertexBuffers2(0, prepared.count, prepared.buffers.data(),
		                             prepared.offsets.data(), prepared.sizes.data(), nullptr);
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

// Whether a stage stores to buffers or writes storage images: data later work may read. Buffer
// atomics alone do not count. They are counters and feedback, such as the per-object maximum
// Astro Bot's geometry shaders record, which a skipped draw only leaves out for a few frames.
static bool StoresData(const ShaderStageRuntime& runtime) {
	const auto& info = runtime.program->info;
	return std::ranges::any_of(info.buffers, [](const auto& buffer) { return buffer.stored; }) ||
	       std::ranges::any_of(info.images, [](const auto& image) { return image.written; });
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
	                             const DrawRenderState& state, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!draw.IsIndexed() && !Prospero::IsRectList(buffer.GetUserConfig().GetPrimType())) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.Name(), state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vertex_info[0], index_type_and_size,
	                  draw.index_count, index_addr);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const DrawCallInfo& draw, const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
		case Prospero::PrimitiveType::kPolygon:
		case Prospero::PrimitiveType::kRectListLegacy:
		case Prospero::PrimitiveType::kPatch:
			if (draw.IsIndexed()) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count, emit.first_vertex,
				               emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (draw.IsIndexed()) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count, i + emit.first_vertex,
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

// KYTY_DEBUG_IMAGE_USERS=<guest address, hex> prints, every 5 s, each kind of draw that binds an
// image over that address: the pixel and vertex shaders, how the image is bound (texture,
// storage, color or depth target, with the stage and slot), and its format, size and view. It
// finds the shaders reading a given surface and whether a draw also renders to it.
static uint64_t ImageUsersAddress() {
	static const uint64_t address = [] {
		const char* text = KYTY_GETENV("KYTY_DEBUG_IMAGE_USERS");
		return text != nullptr ? std::strtoull(text, nullptr, 16) : uint64_t {0};
	}();
	return address;
}

// KYTY_DEBUG_IMAGE_USERS_SHADER=<hash>[,<hash>...]: every image and buffer binding of the draws
// and dispatches with one of these pixel, vertex or compute shaders is listed too, whatever its
// address.
static const std::vector<uint64_t>& ImageUsersShaders() {
	static const std::vector<uint64_t> shaders = [] {
		std::vector<uint64_t> list;
		const char*           text = KYTY_GETENV("KYTY_DEBUG_IMAGE_USERS_SHADER");
		while (text != nullptr && *text != '\0') {
			char* end = nullptr;
			list.push_back(std::strtoull(text, &end, 16));
			text = (end != nullptr && *end == ',') ? end + 1 : nullptr;
		}
		return list;
	}();
	return shaders;
}

bool ImageUsersEnabled() {
	return ImageUsersAddress() != 0 || !ImageUsersShaders().empty() || FramePassesRecording();
}

// KYTY_DEBUG_FRAME_PASSES (see debug.h): frames still to record, 0 while idle.
static uint32_t g_frame_passes_left = 0;
static uint64_t g_frame_pass_index  = 0;
static uint64_t g_frame_pass_repeats = 0;
static std::string g_frame_pass_previous;

bool FramePassesRecording() noexcept {
	static const bool enabled = KYTY_GETENV("KYTY_DEBUG_FRAME_PASSES") != nullptr;
	return enabled && g_frame_passes_left != 0;
}

static void FlushFramePassRepeats() {
	if (g_frame_pass_repeats != 0) {
		std::printf("frame-pass   x%" PRIu64 " more\n", g_frame_pass_repeats);
		g_frame_pass_repeats = 0;
	}
	g_frame_pass_previous.clear();
}

void FramePassesFlip() noexcept {
	static const char* trigger = KYTY_GETENV("KYTY_DEBUG_FRAME_PASSES");
	if (trigger == nullptr) {
		return;
	}
	static const uint32_t count = [] {
		const char* text = KYTY_GETENV("KYTY_DEBUG_FRAME_PASSES_COUNT");
		return text != nullptr ? std::max(1u, static_cast<uint32_t>(std::strtoul(text, nullptr, 10))) : 2u;
	}();
	static uint64_t flips = 0;
	flips++;
	if (g_frame_passes_left != 0) {
		FlushFramePassRepeats();
		g_frame_passes_left--;
		std::printf("frame-pass flip %" PRIu64 "%s\n", flips, g_frame_passes_left == 0 ? " end" : "");
		std::fflush(stdout);
		return;
	}
	std::error_code error;
	if (flips % 15 != 0 || !std::filesystem::exists(trigger, error)) {
		return;
	}
	std::filesystem::remove(trigger, error);
	g_frame_passes_left = count;
	g_frame_pass_index  = 0;
	std::printf("frame-pass flip %" PRIu64 " begin\n", flips);
	std::fflush(stdout);
}

// One line per draw or dispatch while KYTY_DEBUG_FRAME_PASSES records: t = sampled texture,
// s = storage image (<stage>.<slot>), c = color target, d = depth target, smp = the guest sampler
// descriptor's four dwords, wb = a storage buffer the shader writes. An image is its guest request
// (address, host format, size, mips and layers of the view) and, if the cache image holding it
// differs, "in" that image; g1 = the cache image holds GPU-written contents.
static void NoteFramePass(TextureCache& cache, std::span<PreparedBindings* const> stages,
                          std::span<const RenderColorInfo> colors, const RenderDepthInfo* depth,
                          uint64_t pixel_hash, uint64_t vertex_hash) {
	std::string line = depth == nullptr ? fmt::format("cs={:016x}", vertex_hash)
	                                    : fmt::format("ps={:016x} vs={:016x}", pixel_hash, vertex_hash);
	const auto image_text = [&](ImageId id, const TextureCache::ImageDesc& desc) {
		const auto& request = desc.info;
		const auto& view    = desc.view_info;
		std::string text = fmt::format("0x{:x} {} {}x{} m{}+{} l{}+{}", request.data.address,
		                               vk::to_string(request.pixel_format), request.extent.width,
		                               request.extent.height, view.base_level, view.level_count,
		                               view.base_layer, view.layer_count);
		if (!id) {
			return text + " none";
		}
		const auto& image = cache.GetImage(id);
		if (image.info.data.address != request.data.address ||
		    image.info.extent.width != request.extent.width ||
		    image.info.extent.height != request.extent.height ||
		    image.info.pixel_format != request.pixel_format) {
			text += fmt::format(" in 0x{:x} {} {}x{}", image.info.data.address,
			                    vk::to_string(image.info.pixel_format), image.info.extent.width,
			                    image.info.extent.height);
		}
		return text + fmt::format(" g{}", image.IsGpuModified() ? 1 : 0);
	};
	for (const auto* stage: stages) {
		const auto& program    = *stage->runtime->program;
		const auto  stage_type = static_cast<uint32_t>(program.stage);
		for (uint32_t i = 0; i < stage->images.size(); i++) {
			const auto& binding = stage->images[i];
			line += fmt::format(" | {}{}.{} {}",
			                    binding.desc.type == TextureCache::BindingType::Storage ? "s" : "t",
			                    stage_type, i, image_text(binding.image_id, binding.desc));
		}
		const auto& samplers = stage->runtime->resources->samplers;
		for (uint32_t i = 0; i < samplers.size(); i++) {
			const auto sampler = DecodeNativeDescriptor<ShaderSamplerResource>(samplers[i]);
			line += fmt::format(" | smp{}.{} {:08x} {:08x} {:08x} {:08x}", stage_type, i,
			                    sampler.fields[0], sampler.fields[1], sampler.fields[2],
			                    sampler.fields[3]);
		}
		const auto buffer_count = std::min(program.info.buffers.size(), stage->buffer_sources.size());
		for (uint32_t i = 0; i < buffer_count; i++) {
			const auto& source = stage->buffer_sources[i];
			if (program.info.buffers[i].written && source.size != 0) {
				line += fmt::format(" | wb{}.{} 0x{:x}+0x{:x}", stage_type, i, source.address,
				                    source.size);
			}
		}
	}
	for (const auto& color: colors) {
		line += fmt::format(" | c{} {}", color.target_slot, image_text(color.image_id, color.desc));
	}
	if (depth != nullptr && depth->image_id) {
		line += fmt::format(" | d test={} write={} {}", depth->depth_test_enable ? 1 : 0,
		                    depth->depth_write_enable ? 1 : 0, image_text(depth->image_id, depth->desc));
	}
	g_frame_pass_index++;
	if (line == g_frame_pass_previous) {
		g_frame_pass_repeats++;
		return;
	}
	FlushFramePassRepeats();
	std::printf("frame-pass %" PRIu64 " %s\n", g_frame_pass_index, line.c_str());
	g_frame_pass_previous = std::move(line);
}

void NoteImageUsers(TextureCache& cache, std::span<PreparedBindings* const> stages,
                    std::span<const RenderColorInfo> colors, const RenderDepthInfo* depth,
                    uint64_t pixel_hash, uint64_t vertex_hash) {
	if (FramePassesRecording()) {
		NoteFramePass(cache, stages, colors, depth, pixel_hash, vertex_hash);
		if (ImageUsersAddress() == 0 && ImageUsersShaders().empty()) {
			return;
		}
	}
	static std::map<std::string, uint64_t> counts;
	static auto window_start = std::chrono::steady_clock::now();
	const auto  address      = ImageUsersAddress();
	const auto& shaders      = ImageUsersShaders();
	// KYTY_DEBUG_IMAGE_USERS_VERTEX=1 lists every draw whose vertex-side stages sample images too:
	// rare, and how foliage and water read interaction or displacement maps.
	static const bool vertex_images = KYTY_GETENV("KYTY_DEBUG_IMAGE_USERS_VERTEX") != nullptr;
	const bool        reads_in_vertex =
	    vertex_images && pixel_hash != 0 && std::ranges::any_of(stages, [](const PreparedBindings* stage) {
		    return stage->runtime->program->stage != ShaderType::Pixel && !stage->images.empty();
	    });
	const bool every = reads_in_vertex || std::ranges::find(shaders, pixel_hash) != shaders.end() ||
	                   std::ranges::find(shaders, vertex_hash) != shaders.end();
	// KYTY_DEBUG_IMAGE_USERS_TRACE=1 also prints each draw's or dispatch's uses in order, one line
	// per call, with a run of identical calls collapsed into a count: the order of the passes
	// sharing the surface (image-churn lines from the texture cache interleave).
	static const bool trace = KYTY_GETENV("KYTY_DEBUG_IMAGE_USERS_TRACE") != nullptr;
	static std::string previous_call;
	static uint64_t    repeats = 0;
	std::string        call;
	// The binding's guest request (desc.info) and the cache image it resolved to (id), which can
	// be a larger image holding the request.
	const auto note = [&](const char* role, uint32_t stage, uint32_t slot, ImageId id,
	                      const TextureCache::ImageDesc& desc, const char* extra) {
		if (!id) {
			return;
		}
		const auto& image     = cache.GetImage(id);
		const auto& request   = desc.info.data;
		const bool  requested = address >= request.address && address < request.End();
		if (!every && !requested &&
		    (address < image.info.data.address || address >= image.info.data.End())) {
			return;
		}
		const auto& view = desc.view_info;
		call += fmt::format(" | {} {}/{} {} {}x{}", role, stage, slot,
		                    vk::to_string(image.info.pixel_format), desc.info.extent.width,
		                    desc.info.extent.height);
		counts[fmt::format("ps={:016x} vs={:016x} {} stage={} slot={}{} req=0x{:x}+0x{:x} {} "
		                   "guest_fmt={} tile={} {}x{} view={}+{} layers {}+{} -> image=0x{:x}+0x{:x} {} "
		                   "tile={} {}x{} cpu_dirty={} buf_mod={} gpu_mod={}",
		                   pixel_hash, vertex_hash, role, stage, slot, extra, request.address,
		                   request.size, vk::to_string(desc.info.pixel_format),
		                   static_cast<uint32_t>(desc.info.guest_format),
		                   static_cast<uint32_t>(desc.info.tile_mode), desc.info.extent.width,
		                   desc.info.extent.height, view.base_level, view.level_count,
		                   view.base_layer, view.layer_count, image.info.data.address,
		                   image.info.data.size, vk::to_string(image.info.pixel_format),
		                   static_cast<uint32_t>(image.info.tile_mode), image.info.extent.width,
		                   image.info.extent.height, image.IsCpuDirty() ? 1 : 0, image.IsBufferModified() ? 1 : 0,
		                   image.IsGpuModified() ? 1 : 0)]++;
	};
	for (const auto* stage: stages) {
		const auto& program    = *stage->runtime->program;
		const auto  stage_type = static_cast<uint32_t>(program.stage);
		// Storage buffers over the address: a written one makes the texture cache drop the GPU
		// contents of every image there (InvalidateMemoryFromGPU). The chosen shaders' other
		// buffers (mostly constant ranges, one line each) only with KYTY_DEBUG_IMAGE_USERS_BUFFERS=1.
		static const bool every_buffer = [] {
			const char* text = KYTY_GETENV("KYTY_DEBUG_IMAGE_USERS_BUFFERS");
			return text != nullptr && std::strcmp(text, "0") != 0;
		}();
		const auto buffer_count = std::min(program.info.buffers.size(), stage->buffer_sources.size());
		for (uint32_t i = 0; i < buffer_count; i++) {
			const auto& source = stage->buffer_sources[i];
			if (source.size != 0 &&
			    ((every && every_buffer) ||
			     (address >= source.address && address < source.address + source.size))) {
				const auto& resource = program.info.buffers[i];
				// The guest descriptor's own range fields, next to the range bound for it.
				const auto descriptor =
				    i < stage->runtime->resources->buffers.size()
				        ? DecodeNativeDescriptor<ShaderBufferResource>(
				              stage->runtime->resources->buffers[i])
				        : ShaderBufferResource {};
				counts[fmt::format("ps={:016x} vs={:016x} buffer stage={} slot={} range=0x{:x}+0x{:x}"
				                   " written={} stored={} atomic={} stride={} records=0x{:x} oob={}"
				                   " swizzle={} fmt={}",
				                   pixel_hash, vertex_hash, stage_type, i, source.address, source.size,
				                   resource.written ? 1 : 0, resource.stored ? 1 : 0,
				                   resource.atomic ? 1 : 0, descriptor.Stride(), descriptor.NumRecords(),
				                   descriptor.OutOfBounds(), descriptor.SwizzleEnabled() ? 1 : 0,
				                   descriptor.RawFormat())]++;
			}
		}
		for (uint32_t i = 0; i < stage->images.size(); i++) {
			const auto& binding = stage->images[i];
			note(binding.desc.type == TextureCache::BindingType::Storage ? "storage" : "texture",
			     stage_type, i, binding.image_id, binding.desc, "");
		}
	}
	for (const auto& color: colors) {
		note("color", 0, color.target_slot, color.image_id, color.desc, "");
	}
	if (depth != nullptr) {
		const auto extra = fmt::format(
		    " test={} write={} clear={} load_clear={} htile={}", depth->depth_test_enable ? 1 : 0,
		    depth->depth_write_enable ? 1 : 0, depth->depth_clear_enable ? 1 : 0,
		    depth->depth_load_clear_enable ? 1 : 0,
		    depth->desc.info.metadata.kind == ImageMetadataKind::Htile ? 1 : 0);
		note("depth", 0, 0, depth->image_id, depth->desc, extra.c_str());
	}
	if (trace && !call.empty()) {
		call = fmt::format("ps={:016x} vs={:016x}{}", pixel_hash, vertex_hash, call);
		if (call == previous_call) {
			repeats++;
		} else {
			if (repeats != 0) {
				std::printf("image-trace   x%" PRIu64 " more\n", repeats);
			}
			std::printf("image-trace %s\n", call.c_str());
			previous_call = std::move(call);
			repeats       = 0;
		}
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - window_start < std::chrono::seconds(5)) {
		return;
	}
	window_start = now;
	std::printf("image-users 0x%" PRIx64 ": %zu kinds\n", address, counts.size());
	for (const auto& [key, count]: counts) {
		std::printf("image-users   %6" PRIu64 " %s\n", count, key.c_str());
	}
	std::fflush(stdout);
	counts.clear();
}

// KYTY_DEBUG_MESH_RESTART=0 draws a mesh-emulated strip or fan with primitive restart as one
// draw, reading its restart markers as vertices, as before restart was handled there.
static bool MeshRestartSplitEnabled() {
	static const bool enabled = [] {
		const char* text = KYTY_GETENV("KYTY_DEBUG_MESH_RESTART");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	return enabled;
}

// Calls segment(first, count) for each run of the `count` guest indices at `address` between
// restart markers (all bits set, the only marker ResolvePrimitiveRestart leaves enabled), in order.
template <typename Segment>
static void ForEachRestartSegment(uint64_t address, uint32_t element_size, uint32_t count,
                                  Segment&& segment) {
	const auto scan = [&](const auto* indices) {
		using Index         = std::remove_cvref_t<decltype(*indices)>;
		constexpr auto mark = static_cast<Index>(~Index {0});
		uint32_t       first = 0;
		for (uint32_t i = 0; i < count; i++) {
			if (indices[i] == mark) {
				if (i > first) {
					segment(first, i - first);
				}
				first = i + 1;
			}
		}
		if (count > first) {
			segment(first, count - first);
		}
	};
	switch (element_size) {
		case 1: scan(reinterpret_cast<const uint8_t*>(address)); break;
		case 2: scan(reinterpret_cast<const uint16_t*>(address)); break;
		case 4: scan(reinterpret_cast<const uint32_t*>(address)); break;
		default: EXIT("unsupported index size for primitive restart: %u\n", element_size);
	}
}

// Whether a draw may read buffer bytes that pending writes changed: any buffer binding, vertex,
// index or argument range overlapping them, or a shader reading memory through addresses. Only
// atomics on both sides need no barrier: atomics on the same memory are coherent without one.
bool RenderExecutor::ReadsPendingWrites(std::span<PreparedBindings* const> stages,
                                        const ShaderVertexInputInfo&       vertex_input,
                                        const DrawIndexBufferSource&       index_source,
                                        const DrawCallInfo&                draw) const {
	const auto overlaps = [&](uint64_t begin, uint64_t size, bool atomic_only) {
		const auto end = begin + size;
		return std::ranges::any_of(m_pending_writes, [&](const PendingWrite& write) {
			return begin < write.end && write.begin < end && !(atomic_only && write.atomic_only);
		});
	};
	for (const auto* stage: stages) {
		const auto& program = *stage->runtime->program;
		if (program.info.uses_dma) {
			return true;
		}
		const auto count = std::min(program.info.buffers.size(), stage->buffer_sources.size());
		for (size_t i = 0; i < count; i++) {
			const auto& source   = stage->buffer_sources[i];
			const auto& resource = program.info.buffers[i];
			if (source.address != 0 && source.size != 0 &&
			    overlaps(source.address, source.size,
			             resource.atomic && !resource.stored && !resource.loaded)) {
				return true;
			}
		}
	}
	for (int i = 0; i < vertex_input.buffers_num; i++) {
		const auto& vertex = vertex_input.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(i, vertex_input);
		if (vertex.addr != 0 && size != 0 && overlaps(vertex.addr, size, false)) {
			return true;
		}
	}
	return (index_source.address != 0 && index_source.size != 0 &&
	        overlaps(index_source.address, index_source.size, false)) ||
	       (draw.indirect_args != 0 &&
	        overlaps(draw.indirect_args, MeshIndirectArgs::ArgumentsSize, false));
}

// Adds the buffer ranges a draw's shaders write to the pending writes. Draws usually repeat the
// same few ranges, so an existing entry is reused.
void RenderExecutor::RecordPendingWrites(std::span<PreparedBindings* const> stages) {
	for (const auto* stage: stages) {
		const auto& program = *stage->runtime->program;
		const auto  count   = std::min(program.info.buffers.size(), stage->buffer_sources.size());
		for (size_t i = 0; i < count; i++) {
			const auto& source   = stage->buffer_sources[i];
			const auto& resource = program.info.buffers[i];
			if (!resource.written || source.address == 0 || source.size == 0) {
				continue;
			}
			const PendingWrite write {source.address, source.address + source.size,
			                          resource.atomic && !resource.stored};
			const auto same = std::ranges::find_if(m_pending_writes, [&](const PendingWrite& other) {
				return other.begin == write.begin && other.end == write.end;
			});
			if (same == m_pending_writes.end()) {
				m_pending_writes.push_back(write);
			} else {
				same->atomic_only = same->atomic_only && write.atomic_only;
			}
		}
	}
}

// KYTY_DEBUG_BIND_STATS: counts draws that write depth and no color, and their runs with the same
// pipeline, vertex program and depth target (see BindStats). Dispatches between draws do not end
// a run.
static void NoteDepthOnly(DrawRenderState& state, const void* pipeline) {
	auto&      stats = g_bind_stats;
	const auto end_run = [&] {
		const auto length = stats.run_length;
		const auto bucket = length < 2 ? 0 : length < 4 ? 1 : length < 8 ? 2 : length < 16 ? 3
		                                  : length < 64 ? 4 : 5;
		stats.run_draws[bucket] += length;
		stats.run_length = 0;
	};
	stats.draws++;
	if (state.color_count != 0 || !state.depth_info.image_id) {
		if (stats.run_length != 0) {
			stats.run_breaks[3]++;
			end_run();
		}
		return;
	}
	g_draw_phases.SetDepthOnly();
	stats.depth_draws++;
	stats.depth_no_ps += state.ps_active ? 0 : 1;
	stats.depth_ps_images +=
	    state.ps_active && !state.ps_input_info.stage.program->info.images.empty() ? 1 : 0;
	bool vs_images = false;
	for (uint32_t i = 0; i < state.programs.VertexStageCount(); i++) {
		vs_images = vs_images || !state.vertex_info[i].stage.program->info.images.empty();
	}
	stats.depth_vs_images += vs_images ? 1 : 0;
	const void*    program = state.vertex_info[0].stage.program;
	const uint64_t target  = state.depth_info.desc.info.data.address;
	if (stats.run_length != 0 && stats.run_pipeline == pipeline && stats.run_program == program &&
	    stats.run_target == target) {
		stats.run_length++;
		return;
	}
	if (stats.run_length != 0) {
		stats.run_breaks[stats.run_pipeline != pipeline  ? 0
		                 : stats.run_program != program ? 1
		                                                : 2]++;
		end_run();
	}
	stats.run_length   = 1;
	stats.run_pipeline = pipeline;
	stats.run_program  = program;
	stats.run_target   = target;
}

// KYTY_DEBUG_CHAIN_STATS=1 (with KYTY_DEBUG_DRAW_PHASES): how many indexed draws follow the
// previous one without a context register write between them, and how many of those also keep
// its target images and programs: what a draw-state chain could reuse.
static bool ChainStatsEnabled() {
	static const bool enabled = KYTY_GETENV("KYTY_DEBUG_CHAIN_STATS") != nullptr;
	return enabled;
}

// KYTY_DEBUG_CX_WRITES=1: the context registers written before indexed draws (see
// HW::ContextWriteLog), every 5 s for all draws and for depth-only ones: how many draws had a write
// since the previous draw, how many a write that changed a value, and the registers written most.
static std::vector<uint32_t> g_cx_writes_pending;

// A draw that ends early (not drawn) leaves its writes to the next one.
static void TakeContextWrites() {
	auto& log = HW::g_context_write_log;
	g_cx_writes_pending.insert(g_cx_writes_pending.end(), log.since_draw.begin(), log.since_draw.end());
	log.since_draw.clear();
}

static void NoteContextWrites(const DrawRenderState& state) {
	struct Totals {
		uint64_t                    draws = 0, written = 0, changed = 0;
		std::array<uint64_t, 0x400> writes {}, changes {};
	};
	static std::array<Totals, 2> totals;
	static auto                  start = std::chrono::steady_clock::now();
	const bool depth = state.color_count == 0 && static_cast<bool>(state.depth_info.image_id);
	for (auto* t: {&totals[0], depth ? &totals[1] : nullptr}) {
		if (t == nullptr) {
			continue;
		}
		t->draws++;
		bool                         any_changed = false;
		std::array<uint8_t, 0x400>   seen {};
		for (const auto entry: g_cx_writes_pending) {
			const auto offset  = entry & ~HW::ContextWriteLog::ChangedBit;
			const bool changed = (entry & HW::ContextWriteLog::ChangedBit) != 0;
			any_changed        = any_changed || changed;
			if ((seen[offset] & 1u) == 0) {
				t->writes[offset]++;
			}
			if (changed && (seen[offset] & 2u) == 0) {
				t->changes[offset]++;
			}
			seen[offset] |= static_cast<uint8_t>(1u | (changed ? 2u : 0u));
		}
		t->written += g_cx_writes_pending.empty() ? 0 : 1;
		t->changed += any_changed ? 1 : 0;
	}
	g_cx_writes_pending.clear();
	if (std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
		return;
	}
	start = std::chrono::steady_clock::now();
	for (uint32_t kind = 0; kind < 2; kind++) {
		auto& t = totals[kind];
		if (t.draws == 0) {
			continue;
		}
		const auto share = [&](uint64_t n) {
			return 100.0 * static_cast<double>(n) / static_cast<double>(t.draws);
		};
		std::vector<uint32_t> order(0x400);
		for (uint32_t i = 0; i < 0x400; i++) {
			order[i] = i;
		}
		std::ranges::sort(order, [&](uint32_t a, uint32_t b) { return t.writes[a] > t.writes[b]; });
		std::string line = fmt::format("cx-writes {}: draws={} written%={:.1f} changed%={:.1f} | reg:written%/changed%",
		                               kind == 0 ? "all" : "depth", t.draws, share(t.written), share(t.changed));
		for (uint32_t i = 0; i < 14 && t.writes[order[i]] != 0; i++) {
			line += fmt::format(" {:03x}:{:.1f}/{:.1f}", order[i], share(t.writes[order[i]]),
			                    share(t.changes[order[i]]));
		}
		std::printf("%s\n", line.c_str());
		t = {};
	}
	std::fflush(stdout);
}

static void NoteChain(const DrawRenderState& state) {
	struct Chain {
		uint64_t writes = UINT64_MAX;
		std::array<ImageId, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
		uint32_t color_count = 0;
		ImageId  depth;
		const void* vertex = nullptr;
		const void* pixel  = nullptr;
		uint64_t draws = 0, chained = 0, same_targets = 0, same_programs = 0, chained_same_targets = 0;
		std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	};
	static Chain c;
	const bool chained = c.writes == HW::g_context_register_writes;
	bool       same_targets = c.color_count == state.color_count && c.depth == state.depth_info.image_id;
	for (uint32_t i = 0; same_targets && i < state.color_count; i++) {
		same_targets = c.colors[i] == state.color_info[i].image_id;
	}
	const void* vertex = state.vertex_info[0].stage.program;
	const void* pixel  = state.ps_active ? state.ps_input_info.stage.program : nullptr;
	c.draws++;
	c.chained += chained ? 1 : 0;
	c.same_targets += same_targets ? 1 : 0;
	c.chained_same_targets += chained && same_targets ? 1 : 0;
	c.same_programs += chained && vertex == c.vertex && pixel == c.pixel ? 1 : 0;
	c.writes      = HW::g_context_register_writes;
	c.color_count = state.color_count;
	for (uint32_t i = 0; i < state.color_count; i++) {
		c.colors[i] = state.color_info[i].image_id;
	}
	c.depth  = state.depth_info.image_id;
	c.vertex = vertex;
	c.pixel  = pixel;
	if (std::chrono::steady_clock::now() - c.start >= std::chrono::seconds(5)) {
		const auto share = [&](uint64_t n) { return 100.0 * static_cast<double>(n) / static_cast<double>(c.draws); };
		std::printf("chain-stats: draws=%" PRIu64 " no-cx-write%%=%.1f same-targets%%=%.1f "
		            "no-cx-write+same-targets%%=%.1f no-cx-write+same-programs%%=%.1f\n",
		            c.draws, share(c.chained), share(c.same_targets), share(c.chained_same_targets),
		            share(c.same_programs));
		std::fflush(stdout);
		c.draws = c.chained = c.same_targets = c.same_programs = c.chained_same_targets = 0;
		c.start = std::chrono::steady_clock::now();
	}
}

// KYTY_PIPELINE_CHAIN=0 looks every draw's pipeline up; KYTY_DEBUG_AB=pipechain alternates.
static bool PipelineChainEnabled() {
	static const bool enabled = [] {
		const char* text = KYTY_GETENV("KYTY_PIPELINE_CHAIN");
		return text == nullptr || std::strcmp(text, "0") != 0;
	}();
	static const bool ab = AbSelected("pipechain");
	return enabled && !(ab && AbFeatureOff());
}

// KYTY_VERIFY_PIPELINE_CHAIN=1: a draw that takes the chain's pipeline looks it up as well, which
// must find the same one.
static bool VerifyPipelineChain() {
	static const bool enabled = KYTY_GETENV("KYTY_VERIFY_PIPELINE_CHAIN") != nullptr;
	return enabled;
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
	                                     bool primitive_restart_enable) {
	auto& ucfg = buffer.GetUserConfig();
	const auto vertex_stages =
	    std::span {state.vertex_info.data(), state.programs.VertexStageCount()};
	const bool draw_logged = DrawLog::Hash() != 0 && state.ps_active &&
	                         state.ps_input_info.stage.program != nullptr &&
	                         state.ps_input_info.stage.program->shader_hash == DrawLog::Hash();
	const bool draw_counted = draw_logged || DrawLog::StatsEnabled();
	std::array<uint64_t, 3> counters_before {};
	struct BarrierLogScope {
		bool logged  = false;
		bool counted = false;
		~BarrierLogScope() {
			if (logged) {
				g_render_debug_counters.log_barriers.store(false, std::memory_order_relaxed);
			}
			if (counted) {
				g_render_debug_counters.counting.store(false, std::memory_order_relaxed);
			}
		}
	} barrier_log_scope {draw_logged, draw_counted};
	if (draw_counted) [[unlikely]] {
		g_render_debug_counters.counting.store(true, std::memory_order_relaxed);
		g_render_debug_counters.log_barriers.store(draw_logged, std::memory_order_relaxed);
		counters_before = {g_render_debug_counters.render_begins.load(std::memory_order_relaxed),
		                   g_render_debug_counters.render_ends.load(std::memory_order_relaxed),
		                   g_render_debug_counters.image_barriers.load(std::memory_order_relaxed)};
	}
	const bool mesh_active = state.vertex_info[0].stage.program->stage == ShaderType::Mesh;
	const bool gpu_args    = draw.indirect_args != 0;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vertex_info[0].mesh;
		EXIT_NOT_IMPLEMENTED(mesh.fast_launch && (draw.IsIndexed() || primitive_restart_enable));
		static std::atomic_bool restart_warned = false;
		if (primitive_restart_enable && (!draw.IsIndexed() || !MeshRestartSplitEnabled()) &&
		    !restart_warned.exchange(true, std::memory_order_relaxed)) {
			std::printf("Warning: primitive restart is not implemented for mesh shaders; "
			            "continuing draw (primitive=%u indexed=%u)\n",
			            static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed());
		}
		if (mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), draw.IsIndexed(), primitive_restart_enable);
		}
		if (!gpu_args) {
			const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
			if (primitives == 0 || draw.instance_count == 0) {
				return;
			}
			mesh_groups = (primitives - 1u) / mesh.primitives_per_group + 1u;
		}
	}
	// The command processor sends GPU-args draws here when the stages select mesh emulation (the
	// same merged-stage bit PrepareProgram checks), or for indexed list draws and non-indexed draws
	// of native primitive types, which draw from the arguments directly.
	EXIT_IF(gpu_args && !draw.IsIndexed() && mesh_active);

	if (mesh_active && draw.IsIndexed()) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed. A GPU-args
		// draw registers the whole index buffer.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address,
		    gpu_args ? index_source.size
		             : static_cast<uint64_t>(draw.index_count) * index_source.guest_element_size);
	}
	if (VertexExplode::Enabled()) [[unlikely]] {
		VertexExplode::DrawNote note;
		note.frame          = m_context.GetGraphics().presented_frames.load(std::memory_order_relaxed);
		note.stages         = vertex_stages;
		note.indexed        = draw.IsIndexed();
		note.gpu_args       = gpu_args;
		note.restart        = primitive_restart_enable;
		note.primitive      = static_cast<uint32_t>(ucfg.GetPrimType());
		note.index_address  = index_source.address;
		note.index_host     = index_source.host_data;
		note.index_size     = index_source.type == vk::IndexType::eUint32 ? 4u : 2u;
		note.index_count    = draw.index_count;
		note.instance_count = draw.instance_count;
		note.vertex_offset  = draw.IsIndexed() ? emit.vertex_offset
		                                       : static_cast<int32_t>(emit.first_vertex);
		note.first_instance = emit.first_instance;
		if (state.ps_active && state.ps_input_info.stage.program != nullptr) {
			note.ps_hash = state.ps_input_info.stage.program->shader_hash;
		}
		note.color_count   = state.color_count;
		note.color_address = state.color_count != 0 ? state.color_info[0].desc.info.data.address : 0;
		note.depth_address = state.depth_info.desc.info.data.address;
		const auto& extent = state.color_count != 0 ? state.color_info[0].desc.info.extent
		                                            : state.depth_info.desc.info.extent;
		note.target_width  = extent.width;
		note.target_height = extent.height;
		VertexExplode::NoteDraw(m_context.GetBufferCache(), note);
	}
	LogDrawPhase(draw.Name(), "PrepareBindings");
	auto&                            bindings = m_graphics_bindings;
	std::array<PreparedBindings*, 4> descriptor_stages {};
	uint32_t                         stage_count = 0;
	for (uint32_t i = 0; i < vertex_stages.size(); i++) {
		auto& prepared = vertex_stages.size() == 1
		                     ? StageRecord(0, state.vertex_info[i].stage, bindings.vertex[i])
		                     : bindings.vertex[i];
		PrepareBindings(state.vertex_info[i].stage, prepared);
		descriptor_stages[stage_count++] = &prepared;
	}
	if (state.ps_active) {
		if (!bindings.pixel) bindings.pixel.emplace();
		auto& prepared = StageRecord(1, state.ps_input_info.stage, *bindings.pixel);
		PrepareBindings(state.ps_input_info.stage, prepared);
		descriptor_stages[stage_count++] = &prepared;
	}
	g_draw_phases.Mark(DrawPhaseTimer::StageBindings);
	const auto stages = std::span {descriptor_stages.data(), stage_count};
	PrepareGraphicsBindings(stages, std::span {state.color_info, state.color_count});
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.Name(), "PrepareVertexBuffers");
		AcquireVertexBuffers(buffer, state.vertex_info[0], vertex_bindings);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	g_draw_phases.Mark(DrawPhaseTimer::GraphicsBindings);
	vk::ImageAspectFlags feedback_aspects;
	const auto rendering = AcquireRenderTargets(buffer, state.color_info, state.color_count,
	                                            state.depth_info, feedback_aspects, stages);
	g_draw_phases.Mark(DrawPhaseTimer::RenderTargets);
	if (ImageUsersEnabled()) [[unlikely]] {
		NoteImageUsers(m_context.GetTextureCache(), stages,
		               std::span<const RenderColorInfo> {state.color_info, state.color_count},
		               &state.depth_info,
		               state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0,
		               state.vertex_info[0].stage.program->shader_hash);
	}
	if (draw.IsIndexed()) {
		LogDrawPhase(draw.Name(), "CreatePipeline");
	}
	// With asynchronous pipelines, a draw whose pipeline is still compiling is skipped, unless
	// its shaders store data that later work may read.
	bool may_defer = Config::AsyncPipelinesEnabled();
	for (const auto& stage: vertex_stages) {
		may_defer = may_defer && !StoresData(stage.stage);
	}
	may_defer = may_defer && !(state.ps_active && StoresData(state.ps_input_info.stage));
	// Target acquisition resolves the actual overlapping read/write aspects for this draw.
	const auto lookup_pipeline = [&]() -> PipelineCache::Pipeline* {
		DrainStats::SlowLookupTimer create_timer(DrainStats::Kind::PipelineCreate);
		return m_context.GetPipelineCache().GetGraphicsPipeline(
		    std::span {state.color_info, state.color_count}, state.depth_info, vertex_stages, buffer,
		    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
		    state.programs, feedback_aspects, may_defer);
	};
	// GetGraphicsPipeline's inputs besides context registers (see PipelineChain): with no context
	// register written since the chain's lookup, equal inputs build an equal key.
	const bool       chain_on = PipelineChainEnabled();
	PipelineChainKey chain_key;
	if (chain_on) {
		const auto& programs = state.programs;
		for (const auto& program: programs.vertex) {
			chain_key.Push(program.id);
		}
		chain_key.Push(state.ps_active ? programs.pixel.id : 0);
		chain_key.Push(static_cast<uint64_t>(topology) | (uint64_t {primitive_restart_enable} << 8u) |
		               (uint64_t {static_cast<uint32_t>(feedback_aspects)} << 16u) |
		               (uint64_t {static_cast<uint32_t>(ucfg.GetPrimType())} << 32u));
		if (state.ps_active) {
			const auto& ps = state.ps_input_info;
			chain_key.Push(uint64_t {static_cast<uint32_t>(ps.alpha_blend_source)} |
			               (uint64_t {ps.ps_sample_shading} << 8u) |
			               (uint64_t {ps.ps_execute_on_noop} << 9u) | (uint64_t {1} << 10u));
		}
		chain_key.Push(state.color_count);
		for (uint32_t i = 0; i < state.color_count; i++) {
			const auto& color = state.color_info[i];
			chain_key.Push(uint64_t {color.target_slot} |
			               (uint64_t {static_cast<uint32_t>(color.desc.view_info.format)} << 8u) |
			               (uint64_t {color.export_mapping.packed} << 40u) |
			               (uint64_t {static_cast<bool>(color.image_id)} << 48u));
			chain_key.Push(color.desc.info.samples);
		}
		const auto& depth = state.depth_info;
		chain_key.Push(uint64_t {static_cast<bool>(depth.image_id)} |
		               (uint64_t {static_cast<uint32_t>(depth.desc.view_info.format)} << 8u) |
		               (uint64_t {depth.desc.info.samples} << 40u) |
		               (uint64_t {depth.depth_bounds_test_enable} << 48u));
		chain_key.Push(uint64_t {std::bit_cast<uint32_t>(depth.depth_min_bounds)} |
		               (uint64_t {std::bit_cast<uint32_t>(depth.depth_max_bounds)} << 32u));
		const auto& vertex = state.vertex_info[0];
		chain_key.Push(uint64_t {mesh_active} |
		               (uint64_t {static_cast<uint32_t>(vertex.buffers_num)} << 8u) |
		               (uint64_t {static_cast<uint32_t>(vertex.resources_num)} << 32u));
		if (!mesh_active) {
			for (int b = 0; b < vertex.buffers_num; b++) {
				chain_key.Push(uint64_t {vertex.buffers[b].stride} |
				               (uint64_t {vertex.buffers[b].fetch_index != 0} << 32u));
			}
			for (int a = 0; a < vertex.resources_num && chain_key.usable; a++) {
				const auto binding = vertex.resources_dst[a].buffer_index;
				if (binding < 0 || binding >= vertex.buffers_num) {
					chain_key.usable = false;
					break;
				}
				const auto offset = static_cast<uint32_t>(vertex.resources[a].Base48() -
				                                          vertex.buffers[binding].addr);
				chain_key.Push(uint64_t {offset} | (uint64_t {static_cast<uint32_t>(binding)} << 32u));
			}
		}
	}
	auto&                    chain          = m_pipeline_chain;
	PipelineCache::Pipeline* found_pipeline = nullptr;
	if (chain_on && chain_key.usable && chain.pipeline != nullptr &&
	    chain.cx_writes == HW::g_context_register_writes && !chain.pipeline->optimize_pending &&
	    chain.key == chain_key) {
		found_pipeline = chain.pipeline;
		if (VerifyPipelineChain()) [[unlikely]] {
			static uint64_t checked = 0;
			static uint64_t differ  = 0;
			checked++;
			if (lookup_pipeline() != found_pipeline && differ++ < 16) {
				std::printf("pipeline-chain verify: the chain's pipeline differs from the lookup's\n");
			}
			if (checked % 500000 == 0) {
				std::printf("pipeline-chain verify: checked=%" PRIu64 " differ=%" PRIu64 "\n", checked, differ);
				std::fflush(stdout);
			}
		}
	} else {
		found_pipeline  = lookup_pipeline();
		chain.pipeline  = chain_on && chain_key.usable ? found_pipeline : nullptr;
		chain.cx_writes = HW::g_context_register_writes;
		if (chain.pipeline != nullptr) {
			chain.key.size = chain_key.size;
			std::copy_n(chain_key.words.begin(), chain_key.size, chain.key.words.begin());
		}
	}
	if (found_pipeline == nullptr) {
		return;
	}
	auto& pipeline = *found_pipeline;
	g_draw_phases.Mark(DrawPhaseTimer::Pipeline);
	if (g_draw_phases.Active() && BindStatsEnabled()) [[unlikely]] {
		NoteDepthOnly(state, &pipeline);
	}

	// Mesh shaders load their draw parameters from a record by address (see
	// EmitMeshDrawParameter). Write each slice's record now: the ring can wait and restart the
	// scheduler, which must not happen once recording starts below.
	thread_local std::vector<std::pair<MeshDispatchSlice, vk::DeviceAddress>> mesh_slices;
	mesh_slices.clear();
	auto&                        records       = m_context.GetBufferCache().GetDrawRecordBuffer();
	uint64_t                     gpu_output    = 0;
	const uint8_t*               gpu_output_mapped = nullptr;
	std::pair<Buffer*, uint64_t> gpu_arguments = {nullptr, 0};
	if (gpu_args) {
		// The GPU writes this slot; the CPU only reserves it.
		const auto [mapped, offset] = records.Map(MeshIndirectArgs::OutputSize, 64);
		EXIT_IF(mapped == nullptr);
		records.Commit();
		gpu_output        = offset;
		gpu_output_mapped = mapped;
		gpu_arguments = m_context.GetBufferCache().ObtainBuffer(
		    draw.indirect_args,
		    draw.IsIndexed() ? MeshIndirectArgs::ArgumentsSize : sizeof(vk::DrawIndirectCommand),
		    false);
	} else if (mesh_active) {
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		const auto& mesh   = state.vertex_info[0].mesh;
		// Dispatches drawing `index_count` indices from `index_address` as one draw.
		const auto record_dispatches = [&](uint32_t index_count, uint64_t index_address) {
			const auto primitives = mesh.InputPrimitiveCount(index_count);
			if (primitives == 0) {
				return;
			}
			ForEachMeshDispatch(
			    (primitives - 1u) / mesh.primitives_per_group + 1u, draw.instance_count,
			    limits.maxMeshWorkGroupCount[0], limits.maxMeshWorkGroupCount[1],
			    limits.maxMeshWorkGroupTotalCount, [&](const MeshDispatchSlice& slice) {
				    const uint32_t draw_data[] {
				        index_count,
				        draw.IsIndexed() ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
				        emit.first_instance + slice.instance_offset,
				        index_source.guest_element_size,
				        static_cast<uint32_t>(index_address),
				        static_cast<uint32_t>(index_address >> 32u),
				        slice.group_offset};
				    static_assert(std::size(draw_data) ==
				                  ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
				    const auto offset = records.Copy(draw_data, sizeof(draw_data), 16);
				    mesh_slices.emplace_back(slice, records.BufferDeviceAddress() + offset);
			    });
		};
		// Indices the GPU wrote may not have reached guest memory, where the split reads them.
		const bool gpu_indices =
		    primitive_restart_enable && draw.IsIndexed() &&
		    m_context.GetBufferCache().IsRegionGpuModified(
		        index_source.address, uint64_t {draw.index_count} * index_source.guest_element_size);
		if (gpu_indices) {
			static std::atomic_bool gpu_indices_warned = false;
			if (!gpu_indices_warned.exchange(true, std::memory_order_relaxed)) {
				std::printf("Warning: mesh draw with primitive restart reads GPU-written indices; "
				            "drawn without restart (primitive=%u)\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
		}
		if (primitive_restart_enable && draw.IsIndexed() && !gpu_indices &&
		    MeshRestartSplitEnabled()) {
			// The mesh shader assembles strips and fans from the draw's first index on (a fan's
			// center is its vertex 0, a strip's winding follows its primitive number) and knows
			// nothing of restart. So each run of indices between restart markers is drawn as a
			// draw of its own, which restarts both, and the markers are never read as vertices.
			uint32_t segments = 0;
			ForEachRestartSegment(index_source.address, index_source.guest_element_size,
			                      draw.index_count, [&](uint32_t first, uint32_t count) {
				                      record_dispatches(count, index_source.address +
				                                                   uint64_t {first} *
				                                                       index_source.guest_element_size);
				                      segments++;
			                      });
			static std::atomic_uint split_logs = 0;
			if (split_logs.fetch_add(1, std::memory_order_relaxed) < 8) {
				std::printf("mesh restart: prim=%u indices=%u index_bytes=%u split into %u draws\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()), draw.index_count,
				            index_source.guest_element_size, segments);
			}
		} else {
			record_dispatches(draw.index_count, index_source.address);
		}
	}
	g_draw_phases.Mark(DrawPhaseTimer::Records);
	if (draw_logged) [[unlikely]] {
		static uint64_t draws = 0;
		draws++;
		DrawLog::Flush();
		const auto* vertex      = state.vertex_info[0].stage.program;
		const auto  index_limit = index_source.guest_element_size != 0
		                              ? index_source.size / index_source.guest_element_size
		                              : 0;
		std::printf("draw-log: t=%.3f draw=%" PRIu64 " vs=%016" PRIx64
		            " mesh=%u gpu_args=%u indexed=%u prim=%u index_count=%u instances=%u"
		            " index_limit=%" PRIu64 " index_bytes=%u per_group=%u groups=%u extent=%ux%u"
		            " args=0x%" PRIx64 "\n",
		            DrawLog::Seconds(), draws, vertex != nullptr ? vertex->shader_hash : 0,
		            mesh_active, gpu_args, draw.IsIndexed(),
		            static_cast<uint32_t>(ucfg.GetPrimType()), draw.index_count, draw.instance_count,
		            index_limit, index_source.guest_element_size,
		            mesh_active ? state.vertex_info[0].mesh.primitives_per_group : 0u, mesh_groups,
		            state.color_count > 0 ? state.color_info[0].Extent().width : 0u,
		            state.color_count > 0 ? state.color_info[0].Extent().height : 0u,
		            draw.indirect_args);
		if (gpu_args) {
			DrawLog::PendingOutputs().push_back(
			    {gpu_output_mapped, draws, std::chrono::steady_clock::now()});
		}
	}

	// Earlier draws' buffer writes may still wait for their barrier (see
	// CommandBuffer::DeferShaderWriteBarrier). When this draw may read them, it ends rendering
	// first, which records the barrier.
	if (!buffer.HasPendingShaderWrites()) {
		m_pending_writes.clear();
	} else if (ReadsPendingWrites(stages, state.vertex_info[0], index_source, draw)) {
		m_context.GetCommandScheduler().EndRendering();
		m_pending_writes.clear();
	}

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	if (DebugFullBarriers()) [[unlikely]] {
		m_context.GetCommandScheduler().EndRendering();
		RecordFullBarrier(buffer.Handle());
	}
	auto vk_buffer = buffer.Handle();
	SetDrawDebugPhase(buffer, submit_id, draw, draw.IsIndexed() ? 0x100u : 0x200u);
	if (gpu_args) {
		// Build the draw's command (and for mesh emulation its record) from the GPU-written
		// arguments. This compute pass and its barriers must precede rendering and the
		// graphics bindings below.
		m_context.GetCommandScheduler().EndRendering();
		if (!draw.IsIndexed()) {
			// The guest layout is VkDrawIndirectCommand's; copy it into an indirect-capable slot.
			vk::MemoryBarrier2 before {};
			before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
			before.srcAccessMask = vk::AccessFlagBits2::eMemoryWrite;
			before.dstStageMask  = vk::PipelineStageFlagBits2::eCopy;
			before.dstAccessMask =
			    vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;
			vk::DependencyInfo dependency {};
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers    = &before;
			vk_buffer.pipelineBarrier2(dependency);
			const vk::BufferCopy region {gpu_arguments.second, gpu_output,
			                             sizeof(vk::DrawIndirectCommand)};
			vk_buffer.copyBuffer(gpu_arguments.first->Handle(), records.Handle(), 1, &region);
			vk::MemoryBarrier2 after {};
			after.srcStageMask         = vk::PipelineStageFlagBits2::eCopy;
			after.srcAccessMask        = vk::AccessFlagBits2::eTransferWrite;
			after.dstStageMask         = vk::PipelineStageFlagBits2::eDrawIndirect;
			after.dstAccessMask        = vk::AccessFlagBits2::eIndirectCommandRead;
			dependency.pMemoryBarriers = &after;
			vk_buffer.pipelineBarrier2(dependency);
		} else if (m_mesh_indirect == nullptr) {
			m_mesh_indirect = std::make_unique<MeshIndirectArgs>(m_context.GetGraphics());
		}
		const auto index_limit =
		    draw.IsIndexed()
		        ? static_cast<uint32_t>(index_source.size / index_source.guest_element_size)
		        : 0u;
		if (!draw.IsIndexed()) {
			// Copied above.
		} else if (mesh_active) {
			const auto& mesh      = state.vertex_info[0].mesh;
			const auto& limits    = m_context.GetGraphics().mesh_shader_properties;
			const auto  max_total = std::max(1u, limits.maxMeshWorkGroupTotalCount);
			m_mesh_indirect->Record(
			    vk_buffer, *gpu_arguments.first, gpu_arguments.second, records, gpu_output,
			    {.index_bytes          = index_source.guest_element_size,
			     .index_address        = index_source.address,
			     .index_limit          = index_limit,
			     .primitive_size       = mesh.InputPrimitiveSize(),
			     .primitive_step       = mesh.InputPrimitiveStep(),
			     .primitives_per_group = mesh.primitives_per_group,
			     .max_groups = std::min(std::max(1u, limits.maxMeshWorkGroupCount[0]), max_total),
			     .max_instances = std::max(1u, limits.maxMeshWorkGroupCount[1]),
			     .max_total     = max_total});
		} else {
			m_mesh_indirect->Record(vk_buffer, *gpu_arguments.first, gpu_arguments.second, records,
			                        gpu_output, {.index_limit = index_limit, .plain = true});
		}
	}
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (state.ps_active && !draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline, stages);
	if (!mesh_active) {
		CommitIndexBuffer(vk_buffer, index_binding);
	}
	if (state.ps_active && state.ps_input_info.id_buffer_slot != UINT32_MAX && !mesh_active) {
		// Layouts reserve this dword for graphics stages (BindingLayout::reserve_object_id); a stage
		// whose shader data covers it would read the object id instead.
		for (const auto* stage: stages) {
			const auto& layout = stage->runtime->program->bindings;
			if (layout.UsesPushData() &&
			    layout.push_data_start_dword + layout.ShaderDataDwords() >
			        ShaderRecompiler::IR::PushData::ObjectIdDword) {
				static std::atomic<uint32_t> warned {0};
				if (warned.fetch_add(1, std::memory_order_relaxed) < 8u) {
					std::printf("Warning: push-data: object id overwrites shader data (hash %016" PRIx64
					            " start %u dwords %u)\n",
					            stage->runtime->program->shader_hash, layout.push_data_start_dword,
					            layout.ShaderDataDwords());
				}
			}
		}
		// The draw's VGT_OBJECT_ID for the ID buffer (the last push dword).
		const uint32_t object_id = buffer.GetUserConfig().GetObjectId();
		vk_buffer.pushConstants(pipeline.pipeline_layout, pipeline.push_constant_stages,
		                        ShaderRecompiler::IR::PushData::ObjectIdDword * sizeof(uint32_t),
		                        sizeof(object_id), &object_id);
	}
	g_draw_phases.Mark(DrawPhaseTimer::Commit);

	SetGraphicsDynamicParams(buffer, vk_buffer, vertex_stages.back(), state.depth_info, rendering);
	auto& recorded_state = buffer.GetGraphicsState();
	if (m_context.GetGraphics().attachment_feedback_loop_dynamic_enabled &&
	    (!recorded_state.feedback_valid || recorded_state.feedback != feedback_aspects)) {
		vk_buffer.setAttachmentFeedbackLoopEnableEXT(feedback_aspects);
		recorded_state.feedback_valid = true;
		recorded_state.feedback       = feedback_aspects;
	}

	LogDrawPhase(draw.Name(), "BeginRendering");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	if (GpuZones::Enabled()) [[unlikely]] {
		const auto* program =
		    state.ps_active ? state.ps_input_info.stage.program : state.vertex_info[0].stage.program;
		const auto* vertex = state.vertex_info[0].stage.program;
		GpuZones::t_detail = {(uint64_t {draw.index_count} << 32u) | draw.instance_count,
		                      vertex != nullptr ? vertex->shader_hash : 0, draw.indirect_args};
		GpuZones::Mark(vk_buffer,
		               DrainStats::t_predicated ? DrainStats::Zone::GameDrawPredicated
		                                        : DrainStats::Zone::GameDraw,
		               program != nullptr ? program->shader_hash : 0,
		               uint64_t {rendering.width} * rendering.height * rendering.num_layers);
	}
	if (recorded_state.pipeline != pipeline.pipeline) {
		vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
		recorded_state.pipeline = pipeline.pipeline;
	}
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	if (mesh_active) {
		// Replay the draw as sliced dispatches; each slice carries its own group and
		// instance offsets (in its parameter record) so the mesh shader sees the same
		// inputs as one oversized dispatch would have provided.
		if (gpu_args) {
			const auto     record = records.BufferDeviceAddress() + gpu_output +
			                    MeshIndirectArgs::RecordOffset;
			const uint32_t address[] {static_cast<uint32_t>(record),
			                          static_cast<uint32_t>(record >> 32u)};
			vk_buffer.pushConstants(pipeline.pipeline_layout,
			                        vk::ShaderStageFlagBits::eMeshEXT |
			                            vk::ShaderStageFlagBits::eFragment,
			                        0, sizeof(address), address);
			vk_buffer.drawMeshTasksIndirectEXT(records.Handle(),
			                                   gpu_output + MeshIndirectArgs::CommandOffset, 1,
			                                   sizeof(vk::DrawMeshTasksIndirectCommandEXT));
		}
		for (const auto& [slice, record]: mesh_slices) {
			const uint32_t address[] {static_cast<uint32_t>(record),
			                          static_cast<uint32_t>(record >> 32u)};
			vk_buffer.pushConstants(pipeline.pipeline_layout,
			                        vk::ShaderStageFlagBits::eMeshEXT |
			                            vk::ShaderStageFlagBits::eFragment,
			                        0, sizeof(address), address);
			vk_buffer.drawMeshTasksEXT(slice.group_count, slice.instance_count, 1);
		}
	} else {
		if (gpu_args && !draw.IsIndexed()) {
			vk_buffer.drawIndirect(records.Handle(), gpu_output, 1, sizeof(vk::DrawIndirectCommand));
		} else if (gpu_args) {
			vk_buffer.drawIndexedIndirect(records.Handle(), gpu_output + MeshIndirectArgs::RecordOffset, 1,
			                              sizeof(vk::DrawIndexedIndirectCommand));
		} else {
			EmitDrawPrimitives(ucfg, vk_buffer, draw, emit);
		}
	}

	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	vk::PipelineStageFlags shader_write_stages = {};
	for (const auto& stage: vertex_stages) {
		if (HasShaderBufferWrites(stage.stage)) {
			shader_write_stages |= ShaderPipelineStages(NativeShaderStage(stage.logical_stage));
		}
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		// Games write per-draw feedback (atomic maxima, counters) from thousands of draws a frame.
		// PS5 orders such writes only through the game's own sync packets, so the barrier waits
		// until something may read them instead of restarting rendering after every draw.
		buffer.DeferShaderWriteBarrier(shader_write_stages);
		RecordPendingWrites(stages);
		// Bound the per-draw overlap checks.
		if (m_pending_writes.size() > 64) {
			m_context.GetCommandScheduler().EndRendering();
			m_pending_writes.clear();
		}
	}
	m_context.GetBufferCache().OnCommandRecorded();
	g_draw_phases.Mark(DrawPhaseTimer::Record);
	LogDrawPhase(draw.Name(), "DrawComplete");
	if (!draw.IsIndexed()) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
	if (draw_logged) [[unlikely]] {
		std::printf(
		    "draw-log end: render_begins=%" PRIu64 " render_ends=%" PRIu64
		    " image_barriers=%" PRIu64 " shader_writes=%u\n",
		    g_render_debug_counters.render_begins.load(std::memory_order_relaxed) - counters_before[0],
		    g_render_debug_counters.render_ends.load(std::memory_order_relaxed) - counters_before[1],
		    g_render_debug_counters.image_barriers.load(std::memory_order_relaxed) -
		        counters_before[2],
		    static_cast<uint32_t>(static_cast<bool>(shader_write_stages)));
		std::printf("draw-log keys: pipeline=%p vertex_offset=%d first_vertex=%u first_instance=%u",
		            static_cast<const void*>(&pipeline), emit.vertex_offset, emit.first_vertex,
		            emit.first_instance);
		DrawLog::PrintStageKeys("vs", state.vertex_info[0].stage);
		if (state.ps_active) {
			DrawLog::PrintStageKeys("ps", state.ps_input_info.stage);
		}
		std::printf("\n");
	}
	if (DrawLog::StatsEnabled()) [[unlikely]] {
		const auto* pixel  = state.ps_active ? state.ps_input_info.stage.program : nullptr;
		const auto* vertex = state.vertex_info[0].stage.program;
		DrawLog::Account(
		    pixel != nullptr ? pixel->shader_hash : 0, vertex != nullptr ? vertex->shader_hash : 0,
		    g_render_debug_counters.render_begins.load(std::memory_order_relaxed) - counters_before[0],
		    g_render_debug_counters.image_barriers.load(std::memory_order_relaxed) -
		        counters_before[2]);
	}
}

RenderExecutor::RenderExecutor(RenderContext& context): m_context(context) {}

RenderExecutor::~RenderExecutor() = default;

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();
	g_draw_phases.Begin();
	if (HW::g_context_write_log.enabled) [[unlikely]] {
		TakeContextWrites();
	}

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	{
		DrawPhaseTimer::ProbeScope probe(g_draw_phases, DrawPhaseTimer::PendingOps);
		m_context.GetCommandScheduler().PopPendingOperations(false);
	}
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || args.instance_count == 0) {
		g_pm4_ops.outcome = Pm4OpTimer::Empty;
		return;
	}

	// A color metadata operation, depth copy or resolve instead of a draw.
	const auto operation =
	    ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset)
	        ? Pm4OpTimer::MetadataOp
	    : DepthStencilCopy(buffer)            ? Pm4OpTimer::DepthCopy
	    : ResolveColorTargets(buffer, args.render_target_slice_offset) ? Pm4OpTimer::Resolve
	                                                                   : Pm4OpTimer::Drawn;
	if (operation != Pm4OpTimer::Drawn) {
		g_pm4_ops.outcome = operation;
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		g_pm4_ops.outcome = Pm4OpTimer::NoShader;
		return;
	}
	if (DrawRepeatEnabled()) [[unlikely]] {
		NoteDrawRepeat(sh_ctx);
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndex():Shader:\n");
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	// The register checks passed for the chain's draw, and no context register changed since.
	if (!(m_draw_chain.valid && m_draw_chain.cx_writes == HW::g_context_register_writes &&
	      DrawChainEnabled())) {
		hw_check(buffer);
	}

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		g_pm4_ops.outcome = Pm4OpTimer::NoShader;
		return;
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_source.type               = vk::IndexType::eUint16;
			index_source.guest_element_size = 2;
			break;
		case Prospero::IndexType::kIndex32:
			index_source.type               = vk::IndexType::eUint32;
			index_source.guest_element_size = 4;
			break;
		case Prospero::IndexType::kIndex8:
			index_source.guest_element_size = 1;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}
	// A GPU-args draw reads its indices on the GPU; register the whole index buffer.
	index_source.size = static_cast<uint64_t>(args.indirect_args != 0 ? args.index_limit : args.index_count) *
	                    index_source.guest_element_size;
	// GPU-args draws are mesh-emulated, which does not implement restart; skip the CPU scan.
	const bool primitive_restart =
	    args.indirect_args == 0 && ResolvePrimitiveRestart(buffer, index_source);

	std::vector<uint16_t> expanded_indices;
	if (index_source.guest_element_size == 1 && args.indirect_args == 0) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
		index_source.host_data = expanded_indices.data();
		index_source.size      = expanded_indices.size() * sizeof(uint16_t);
	}

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndex, args.index_count,
	                        args.instance_count, args.first_instance, args.indirect_args};
	// One pass, or one per group of slots when the draw binds one surface to several slots.
	const auto aliased    = AliasedColorSlots(buffer.GetRegisters());
	uint64_t   pixel_hash = 0;
	for (const uint32_t filter: {~aliased, aliased}) {
		if (filter == 0) {
			continue;
		}
		m_color_slot_filter = filter;
		DrawRenderState state;
		const bool      prepared =
		    PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state);
		m_color_slot_filter = ~0u;
		if (!prepared) {
			g_pm4_ops.outcome = Pm4OpTimer::NotPrepared;
			ResetBindings();
			return;
		}
		if (StampTraceEnabled()) [[unlikely]] {
			for (uint32_t i = 0; i < state.color_count; i++) {
				StampTraceDraw(state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0,
				               state.color_info[i].desc.info.data.address);
			}
		}

		if (g_draw_phases.Active() && ChainStatsEnabled()) [[unlikely]] {
			NoteChain(state);
		}
		if (HW::g_context_write_log.enabled) [[unlikely]] {
			NoteContextWrites(state);
		}
		LogDrawStateIfNeeded(buffer, draw, state, args.index_type_and_size,
		                     args.index_addr);

		const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
		const auto [vertex_offset, instance_offset] =
		    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
		             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);

		DrawEmitInfo emit {};
		emit.vertex_offset  = vertex_offset + args.base_vertex;
		emit.first_instance = instance_offset;

		ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source,
		                    primitive_restart);
		ResetBindings();
		pixel_hash = state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0;
	}
	g_draw_phases.End(pixel_hash);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();
	g_draw_phases.Begin(true);

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	{
		DrawPhaseTimer::ProbeScope probe(g_draw_phases, DrawPhaseTimer::PendingOps);
		m_context.GetCommandScheduler().PopPendingOperations(false);
	}
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.vertex_count == 0 || args.instance_count == 0) {
		g_pm4_ops.outcome = Pm4OpTimer::Empty;
		return;
	}

	// A color metadata operation, depth copy or resolve instead of a draw.
	const auto operation =
	    ConsumeMetadataColorOperation(buffer, args.render_target_slice_offset)
	        ? Pm4OpTimer::MetadataOp
	    : DepthStencilCopy(buffer)            ? Pm4OpTimer::DepthCopy
	    : ResolveColorTargets(buffer, args.render_target_slice_offset) ? Pm4OpTimer::Resolve
	                                                                   : Pm4OpTimer::Drawn;
	if (operation != Pm4OpTimer::Drawn) {
		g_pm4_ops.outcome = operation;
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		g_pm4_ops.outcome = Pm4OpTimer::NoShader;
		return;
	}
	if (DrawRepeatEnabled()) [[unlikely]] {
		NoteDrawRepeat(sh_ctx);
	}

	if (graphics_debug_dump_enabled()) {
		LOGF("GraphicsRenderDrawIndexAuto():Shader:\n");
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {CommandBufferDebugOp::DrawIndexAuto, args.vertex_count,
	                         args.instance_count, args.first_instance, args.indirect_args};

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, topology)) {
		g_pm4_ops.outcome = Pm4OpTimer::NoShader;
		ResetBindings();
		return;
	}
	// One pass, or one per group of slots when the draw binds one surface to several slots.
	const auto aliased    = AliasedColorSlots(buffer.GetRegisters());
	uint64_t   pixel_hash = 0;
	for (const uint32_t filter: {~aliased, aliased}) {
		if (filter == 0) {
			continue;
		}
		m_color_slot_filter = filter;
		DrawRenderState state;
		const bool      prepared =
		    PrepareDrawRenderState(buffer, draw, args.render_target_slice_offset, state);
		m_color_slot_filter = ~0u;
		if (!prepared) {
			g_pm4_ops.outcome = Pm4OpTimer::NotPrepared;
			ResetBindings();
			return;
		}
		if (StampTraceEnabled()) [[unlikely]] {
			for (uint32_t i = 0; i < state.color_count; i++) {
				StampTraceDraw(state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0,
				               state.color_info[i].desc.info.data.address);
			}
		}

		const bool rect_list = Prospero::IsRectList(ucfg.GetPrimType());
		if (rect_list && state.vertex_info[0].buffers_num == 0 &&
		    state.vertex_info[0].stage.program->param_export_mask == 0 &&
		    state.ps_input_info.input_num != 0) {
			if (graphics_debug_dump_enabled()) {
				LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
				     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
				     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
				     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
			}
			g_pm4_ops.outcome = Pm4OpTimer::RectListSkip;
			ResetBindings();
			return;
		}

		LogDrawStateIfNeeded(buffer, draw, state, 0, nullptr);

		const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
		const auto [vertex_offset, instance_offset] =
		    indirect ? std::pair<int32_t, uint32_t> {0, args.first_instance}
		             : ResolveDrawOffsets(ucfg.GetIndexOffset(), state.vertex_info[0]);
		DrawEmitInfo emit {};
		emit.first_vertex =
		    static_cast<uint32_t>(vertex_offset + static_cast<int32_t>(args.first_vertex));
		emit.first_instance = instance_offset;

		DrawIndexBufferSource index_source {};
		ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source, false);
		ResetBindings();
		pixel_hash = state.ps_active ? state.ps_input_info.stage.program->shader_hash : 0;
	}
	g_draw_phases.End(pixel_hash);
}

bool RenderExecutor::ResolveColorTargets(CommandBuffer& buffer, uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source, {src.guest_mip_level, 1, src.guest_array_layer, 1},
	                    {dst.guest_mip_level, 1, dst.guest_array_layer, 1});
	return true;
}

} // namespace Libs::Graphics
