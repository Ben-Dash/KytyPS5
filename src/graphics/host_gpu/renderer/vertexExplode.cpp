#include "graphics/host_gpu/renderer/vertexExplode.h"

#include "common/logging/log.h"
#include "common/switches.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/VertexExplodeLog.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::VertexExplode {

namespace IR = ShaderRecompiler::IR;

namespace {

// What a red line tells about the last draw of a vertex-stage shader.
struct DrawInfo {
	uint64_t    frame = 0;
	uint64_t    draw  = 0;
	std::string text;
	// The first stage's fetch buffers, to print the raw bytes of a reported vertex.
	std::array<ShaderVertexInputBuffer, 4> buffers {};
	uint32_t                                buffer_count = 0;
	// The index buffer, to find a reported vertex index that no index of the buffer can be.
	uint64_t index_address = 0;
	uint32_t index_count   = 0;
	uint32_t index_size    = 0;
	bool     index_gpu_args = false;
	// A copy of the 16-bit indices taken at the draw (up to 64Ki indices; empty: not readable).
	std::vector<uint16_t> indices;
	// Over all draws of the shader in this frame: draws, vertex offsets, indexed and 32-bit draws.
	uint32_t frame_draws = 0, indexed_draws = 0, index32_draws = 0, gpu_args_draws = 0;
	int32_t  min_vertex_offset = 0, max_vertex_offset = 0;
};

// The indices of a shader's CPU-args indexed draws in one frame (copies read without faulting),
// to find where a reported vertex index came from. The last three frames are kept: a record is
// read back a frame or two after its draw.
struct FrameIndices {
	uint64_t                           frame = 0;
	uint32_t                           draws16 = 0, draws32 = 0;
	uint32_t                           gpu_written = 0; // indexed draws whose indices the GPU wrote
	uint32_t                           unreadable  = 0; // other indexed draws not copied
	std::vector<std::vector<uint16_t>> copies16;
	std::vector<std::vector<uint32_t>> copies32;
	size_t                             copied = 0; // indices in the copies
};
constexpr size_t MaxCopiedIndices = 1u << 16u; // per shader and frame

std::atomic<uint32_t> g_index_type_source {0};

const char* IndexTypeSourceName(uint32_t source) {
	switch (source) {
		case 1: return "INDEX_TYPE packet";
		case 2: return "SET_UCONFIG VGT_INDEX_TYPE";
		case 3: return "SET_UCONFIG_REG_INDIRECT";
		default: return "unknown";
	}
}

// A GPU-written 32-bit index buffer to read back at the next safe point.
struct PendingSample {
	uint64_t hash = 0, frame = 0, address = 0;
	uint32_t count = 0, records = 0, source = 0;
};

struct State {
	std::mutex                                 mutex;
	uint64_t                                   draws = 0;
	std::unordered_map<uint64_t, DrawInfo>     last_draw;   // by shader hash
	std::unordered_set<uint64_t>               reported;    // hash * 4 + kind (GPU records)
	std::unordered_set<uint64_t>               cpu_reported; // hash ^ index address
	std::unordered_map<uint64_t, uint64_t>     index_checked; // index address ^ count -> frame
	std::unordered_map<uint64_t, std::array<FrameIndices, 3>> frame_indices; // by shader hash
	std::vector<PendingSample>                 pending_samples;
	std::unordered_map<uint64_t, uint64_t>     last_sample; // hash -> frame
	std::array<uint64_t, 4>                    hits {};      // by kind, all records
	std::array<uint32_t, 4>                    shaders {};   // distinct shaders by kind
	uint64_t                                   cpu_hits         = 0;
	uint64_t                                   cpu_gpu_written  = 0;
	uint64_t                                   total            = 0;
	uint64_t                                   last_summary     = 0;
};

State& GetState() {
	static State state;
	return state;
}

const char* KindName(uint32_t kind) {
	switch (static_cast<IR::VertexExplodeKind>(kind)) {
		case IR::VertexExplodeKind::NonFinite: return "nan/inf";
		case IR::VertexExplodeKind::WZero: return "w~0";
		case IR::VertexExplodeKind::Huge: return "huge";
		case IR::VertexExplodeKind::Zero: return "cull(xyw=0)";
		default: return "?";
	}
}

// Index buffer, fetch buffers and the first V#s of every vertex stage.
std::string DescribeDraw(const DrawNote& note, uint64_t draw) {
	std::string text = fmt::format("draw #{} frame {} prim={} inst={} first_inst={} voff={}", draw,
	                               note.frame, note.primitive, note.instance_count,
	                               note.first_instance, note.vertex_offset);
	text += fmt::format(" ps={:016x} rt={}x{} colors={} c0=0x{:x} depth=0x{:x}", note.ps_hash,
	                    note.target_width, note.target_height, note.color_count,
	                    note.color_address, note.depth_address);
	if (note.indexed) {
		text += fmt::format(" idx=0x{:x} n={} isize={}{}{}", note.index_address, note.index_count,
		                    note.index_size, note.gpu_args ? " gpu-args" : "",
		                    note.restart ? " restart" : "");
	} else {
		text += fmt::format(" verts={}", note.index_count);
	}
	for (const auto& stage: note.stages) {
		for (int i = 0; i < stage.buffers_num && i < 4; i++) {
			const auto& b = stage.buffers[i];
			text += fmt::format(" | vb{} 0x{:x} stride={} recs={}", i, b.addr, b.stride,
			                    b.num_records);
		}
		for (int i = 0; i < stage.resources_num && i < 4; i++) {
			const auto& r = stage.resources[i];
			text += fmt::format(" | attr{} 0x{:x} fmt={} stride={} recs={}", i, r.Base48(),
			                    static_cast<uint32_t>(r.RawFormat()), r.Stride(), r.NumRecords());
		}
		if (stage.stage.resources != nullptr) {
			const auto& buffers = stage.stage.resources->buffers;
			for (size_t i = 0; i < buffers.size() && i < 4; i++) {
				ShaderBufferResource r;
				std::memcpy(r.fields, buffers[i].dwords.data(), sizeof(r.fields));
				text += fmt::format(" | v#{} 0x{:x} fmt={} stride={} recs={}", i, r.Base48(),
				                    static_cast<uint32_t>(r.RawFormat()), r.Stride(),
				                    r.NumRecords());
			}
		}
	}
	return text;
}

bool IsFillPattern(uint32_t value, uint32_t size) {
	const auto byte = value & 0xffu;
	const auto all  = size == 2 ? byte * 0x0101u : byte * 0x01010101u;
	return value == all && value != 0;
}

// The CPU check: the largest index of a CPU-args indexed draw against the fetch buffers.
void CheckIndices(State& state, BufferCache& cache, const DrawNote& note, uint64_t vs_hash) {
	if (!note.indexed || note.gpu_args || note.index_count == 0 || note.stages.empty() ||
	    (note.index_address == 0 && note.index_host == nullptr)) {
		return;
	}
	const auto& stage = note.stages.front();
	if (stage.buffers_num == 0) {
		return;
	}
	const auto key = note.index_address ^ (uint64_t {note.index_count} << 40u) ^ vs_hash;
	if (auto found = state.index_checked.find(key);
	    found != state.index_checked.end() && note.frame - found->second < 60) {
		return;
	}
	state.index_checked[key] = note.frame;
	const uint64_t bytes = uint64_t {note.index_count} * note.index_size;
	if (note.index_host == nullptr && cache.IsRegionGpuModified(note.index_address, bytes)) {
		// The CPU copy of a GPU-written index buffer is stale.
		state.cpu_gpu_written++;
		return;
	}
	// Guest memory is read without faulting (a fault handler may wait for the GPU).
	std::vector<uint8_t> copy;
	const void*          data = note.index_host;
	if (data == nullptr) {
		copy.resize(bytes);
		if (!LibKernel::Memory::TryReadGpuCleanBacking(note.index_address, copy.data(), bytes)) {
			state.cpu_gpu_written++;
			return;
		}
		data = copy.data();
	}
	const uint32_t restart = note.index_size == 2 ? 0xffffu : 0xffffffffu;
	uint32_t       max_index = 0;
	uint32_t       patterns  = 0;
	for (uint32_t i = 0; i < note.index_count; i++) {
		uint32_t index = 0;
		if (note.index_size == 2) {
			index = static_cast<const uint16_t*>(data)[i];
		} else {
			index = static_cast<const uint32_t*>(data)[i];
		}
		if (note.restart && index == restart) {
			continue;
		}
		patterns += IsFillPattern(index, note.index_size) && index > 0xffu ? 1u : 0u;
		max_index = std::max(max_index, index);
	}
	const int64_t last = int64_t {max_index} + note.vertex_offset;
	for (int i = 0; i < stage.buffers_num; i++) {
		const auto& b = stage.buffers[i];
		if (b.stride == 0 || b.num_records == 0 || last < b.num_records) {
			continue;
		}
		state.cpu_hits++;
		if (!state.cpu_reported.insert(vs_hash ^ note.index_address ^ (uint64_t(i) << 56u)).second) {
			continue;
		}
		std::printf("\x1b[91mVtxExplode CPU: vs=%016" PRIx64 " index past vertex buffer: max_index=%u"
		            " (+voff %d = %" PRId64 ") >= vb%d records=%u (stride %u, 0x%" PRIx64
		            ", %" PRIu64 " bytes)%s%s | %s\x1b[0m\n",
		            vs_hash, max_index, note.vertex_offset, last, i, b.num_records, b.stride,
		            b.addr, uint64_t {b.stride} * b.num_records,
		            IsFillPattern(max_index, note.index_size) ? " PATTERN" : "",
		            patterns != 0 ? fmt::format(" ({} fill-pattern indices)", patterns).c_str() : "",
		            DescribeDraw(note, state.draws).c_str());
		std::fflush(stdout);
		LOGF("VtxExplode CPU: vs=%016" PRIx64 " max_index=%u voff=%d vb%d records=%u stride=%u\n",
		     vs_hash, max_index, note.vertex_offset, i, b.num_records, b.stride);
	}
}

} // namespace

bool Enabled() {
	return IR::VertexExplodeCheck().enabled;
}

// KYTY_VTX_EXPLODE_PROBE=<hash>:<sgpr>:<offset>: for draws of that vertex shader, prints the
// dword at <offset> of the buffer whose V# the shader gets in s[<sgpr>..<sgpr>+3] (read like the
// shader's own constant reads, GPU-written bytes read back), with the draw's index size and
// whether its index buffer is GPU-written. 64 lines, then one per 600 draws.
struct Probe {
	uint64_t hash = 0;
	uint32_t sgpr = 0, offset = 0;
};

const Probe& GetProbe() {
	static const Probe probe = [] {
		Probe result;
		if (const char* text = KYTY_GETENV("KYTY_VTX_EXPLODE_PROBE"); text != nullptr) {
			char* end    = nullptr;
			result.hash  = std::strtoull(text, &end, 16);
			if (end != nullptr && *end == ':') {
				result.sgpr = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
			}
			if (end != nullptr && *end == ':') {
				result.offset = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
			}
		}
		return result;
	}();
	return probe;
}

void RunProbe(BufferCache& cache, const DrawNote& note, const ShaderVertexInputInfo& stage) {
	const auto& probe = GetProbe();
	if (probe.hash == 0 || stage.stage.program == nullptr || stage.stage.resources == nullptr ||
	    stage.stage.program->shader_hash != probe.hash) {
		return;
	}
	static uint32_t count = 0;
	count++;
	if (count > 64 && count % 600 != 0) {
		return;
	}
	const auto& program = *stage.stage.program;
	const auto& data    = stage.stage.resources->user_data;
	const auto  first   = probe.sgpr - program.user_data_base;
	if (probe.sgpr < program.user_data_base || first + 4 > data.size()) {
		std::printf("VtxExplode probe: s%u is not in the user data%s", probe.sgpr, "\n");
		return;
	}
	ShaderBufferResource v;
	for (uint32_t i = 0; i < 4; i++) {
		v.fields[i] = data[first + i];
	}
	const uint64_t address = v.Base48() + probe.offset;
	uint32_t       value   = 0xdeadbeefu;
	const bool     gpu     = cache.IsRegionGpuModified(address, 4);
	LibKernel::Memory::ReadGuestOnGpuThread(address, &value, sizeof(value));
	const bool index_gpu = note.indexed && note.index_host == nullptr && note.index_address != 0 &&
	                       cache.IsRegionGpuModified(note.index_address,
	                                                 uint64_t {note.index_count} * note.index_size);
	// The permutation the draw uses (its compiled info is unique per permutation) and its buffer
	// descriptors: a buffer whose V# is empty reads zero in this permutation.
	const auto& buffers = stage.stage.resources->buffers;
	uint32_t    empty   = 0;
	std::string list;
	for (size_t i = 0; i < buffers.size(); i++) {
		ShaderBufferResource b;
		std::memcpy(b.fields, buffers[i].dwords.data(), sizeof(b.fields));
		empty += b.Base48() == 0 && b.NumRecords() == 0 ? 1u : 0u;
		list += fmt::format(" {}:{:x}/{}/{}", i, b.Base48(), b.Stride(), b.NumRecords());
	}
	std::printf("VtxExplode probe #%u: vs=%016" PRIx64 " frame %" PRIu64 " V#(s%u) 0x%" PRIx64
	            " +%u = 0x%08x (GPU-written %d) | indexed %d isize %u idx 0x%" PRIx64
	            " n=%u index GPU-written %d gpu-args %d | permutation %p, %zu buffers, %u empty:%s%s",
	            count, probe.hash, note.frame, probe.sgpr, v.Base48(), probe.offset, value, gpu,
	            note.indexed, note.index_size, note.index_address, note.index_count, index_gpu,
	            note.gpu_args, static_cast<const void*>(&program), buffers.size(), empty,
	            list.c_str(), "\n");
}

void NoteDraw(BufferCache& cache, const DrawNote& note) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.draws++;
	for (const auto& stage: note.stages) {
		RunProbe(cache, note, stage);
	}
	std::string text;
	for (const auto& stage: note.stages) {
		if (stage.stage.program == nullptr) {
			continue;
		}
		const auto hash = stage.stage.program->shader_hash;
		auto&      info = state.last_draw[hash];
		if (info.frame != note.frame || info.draw == 0) {
			info.frame_draws = info.indexed_draws = info.index32_draws = info.gpu_args_draws = 0;
			info.min_vertex_offset = info.max_vertex_offset = note.vertex_offset;
			// Described once per frame and shader: the text costs more than the draw.
			if (text.empty()) {
				text = DescribeDraw(note, state.draws);
			}
			info.text = text;
			const auto& first = note.stages.front();
			info.buffer_count = static_cast<uint32_t>(std::clamp(first.buffers_num, 0, 4));
			info.index_address  = note.indexed && note.index_host == nullptr ? note.index_address : 0;
			info.index_count    = note.index_count;
			info.index_size     = note.index_size;
			info.index_gpu_args = note.gpu_args;
			info.indices.clear();
			if (info.index_address != 0 && note.index_size == 2 && !note.gpu_args &&
			    note.index_count <= 65536u) {
				info.indices.resize(note.index_count);
				if (!LibKernel::Memory::TryReadGpuCleanBacking(note.index_address, info.indices.data(),
				                                               info.indices.size() * 2u)) {
					info.indices.clear();
				}
			}
			for (uint32_t i = 0; i < info.buffer_count; i++) {
				info.buffers[i] = first.buffers[i];
			}
		}
		info.frame = note.frame;
		info.draw  = state.draws;
		info.frame_draws++;
		info.indexed_draws += note.indexed ? 1u : 0u;
		info.index32_draws += note.indexed && note.index_size == 4 ? 1u : 0u;
		info.gpu_args_draws += note.gpu_args ? 1u : 0u;
		info.min_vertex_offset = std::min(info.min_vertex_offset, note.vertex_offset);
		info.max_vertex_offset = std::max(info.max_vertex_offset, note.vertex_offset);
		if (note.indexed && !note.gpu_args && note.index_host == nullptr && note.index_address != 0) {
			auto& slot = state.frame_indices[hash][note.frame % 3u];
			if (slot.frame != note.frame) {
				slot = FrameIndices {};
				slot.frame = note.frame;
			}
			(note.index_size == 4 ? slot.draws32 : slot.draws16)++;
			const uint64_t bytes = uint64_t {note.index_count} * note.index_size;
			if (cache.IsRegionGpuModified(note.index_address, bytes)) {
				slot.gpu_written++;
				auto& last = state.last_sample[hash];
				if (note.index_size == 4 && state.pending_samples.size() < 8 &&
				    (last == 0 || note.frame - last >= 60)) {
					last = note.frame;
					const auto& first = note.stages.front();
					state.pending_samples.push_back(
					    {hash, note.frame, note.index_address, std::min(note.index_count, 4096u),
					     first.buffers_num > 0 ? first.buffers[0].num_records : 0u,
					     g_index_type_source.load(std::memory_order_relaxed)});
				}
			} else if (slot.copied + note.index_count > MaxCopiedIndices) {
				slot.unreadable++;
			} else if (note.index_size == 4) {
				std::vector<uint32_t> copy(note.index_count);
				if (LibKernel::Memory::TryReadGpuCleanBacking(note.index_address, copy.data(), bytes)) {
					slot.copied += copy.size();
					slot.copies32.push_back(std::move(copy));
				} else {
					slot.unreadable++;
				}
			} else {
				std::vector<uint16_t> copy(note.index_count);
				if (LibKernel::Memory::TryReadGpuCleanBacking(note.index_address, copy.data(), bytes)) {
					slot.copied += copy.size();
					slot.copies16.push_back(std::move(copy));
				} else {
					slot.unreadable++;
				}
			}
		}
	}
	if (!note.stages.empty() && note.stages.front().stage.program != nullptr) {
		CheckIndices(state, cache, note, note.stages.front().stage.program->shader_hash);
	}
}

void Report(const uint32_t* region, uint64_t frame) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	const auto      total = region[0];
	state.total += total;
	for (uint32_t slot = 0; slot < IR::ExplodeLogSlots; slot++) {
		const auto count = region[IR::ExplodeLogCounterWord + slot];
		if (count == 0) {
			continue;
		}
		const auto* record = region + IR::ExplodeLogRecordWord + slot * IR::ExplodeLogRecordWords;
		if ((record[0] & 0xffff0000u) != IR::ExplodeLogMarker) {
			continue;
		}
		const auto kind = record[0] & 0xffu;
		const bool mesh = (record[0] & 0x100u) != 0;
		const auto hash = uint64_t {record[1]} | (uint64_t {record[2]} << 32u);
		if (kind >= 4) {
			continue;
		}
		const uint32_t capture_index = record[10] == 1u ? record[3] - record[9] : record[3];
		// KYTY_VTX_EXPLODE_CAPTURE=<hash> (with --rd): the first huge or non-finite position of that
		// shader at a vertex index above 65535 requests one RenderDoc capture.
		static const uint64_t capture_hash = [] {
			const char* text = KYTY_GETENV("KYTY_VTX_EXPLODE_CAPTURE");
			return text != nullptr ? std::strtoull(text, nullptr, 16) : uint64_t {0};
		}();
		static bool capture_requested = false;
		if (capture_hash != 0 && hash == capture_hash && !capture_requested && capture_index > 0xffffu &&
		    (kind == static_cast<uint32_t>(IR::VertexExplodeKind::Huge) ||
		     kind == static_cast<uint32_t>(IR::VertexExplodeKind::NonFinite))) {
			capture_requested = true;
			std::printf("VtxExplode: requesting a RenderDoc capture for vs=%016" PRIx64 "%s", hash,
			            "\n");
			RenderDocRequestCapture();
		}
		state.hits[kind] += count;
		if (!state.reported.insert(hash * 4u + kind).second) {
			continue;
		}
		state.shaders[kind]++;
		std::string base_vertex;
		if (record[10] == 1u) {
			const auto base = static_cast<int32_t>(record[9]);
			base_vertex = fmt::format(" base_vertex={} index={}", base,
			                          static_cast<int64_t>(record[3]) - base);
		}
		const auto  x    = std::bit_cast<float>(record[5]);
		const auto  y    = std::bit_cast<float>(record[6]);
		const auto  z    = std::bit_cast<float>(record[7]);
		const auto  w    = std::bit_cast<float>(record[8]);
		const auto  draw = state.last_draw.find(hash);
		const auto  text = draw != state.last_draw.end() ? draw->second.text : std::string("draw unknown");
		const bool  cull = kind == static_cast<uint32_t>(IR::VertexExplodeKind::Zero);
		const char* on   = cull ? "" : "\x1b[91m";
		const char* off  = cull ? "" : "\x1b[0m";
		std::printf("%sVtxExplode: %s=%016" PRIx64 " kind=%s frame=%" PRIu64 " hits=%u %s=%u %s=%u"
		            "%s pos=(%g, %g, %g, %g) | last %s%s\n",
		            on, mesh ? "mesh" : "vs", hash, KindName(kind), frame, count,
		            mesh ? "invocation" : "vertex", record[3], mesh ? "group" : "instance",
		            record[4], base_vertex.c_str(), x, y, z, w, text.c_str(), off);
		// The reported vertex's bytes in each fetch buffer (as the CPU sees them now).
		if (draw != state.last_draw.end() && !mesh) {
			for (uint32_t i = 0; i < draw->second.buffer_count; i++) {
				const auto& b = draw->second.buffers[i];
				if (b.addr == 0 || b.stride == 0 || record[3] >= b.num_records) {
					continue;
				}
				// Read without faulting: this runs in a deferred scheduler callback, where a fault
				// handler waiting for the GPU would end the session.
				std::array<uint8_t, 32> bytes {};
				const auto size = std::min(b.stride, 32u);
				std::string hex;
				if (LibKernel::Memory::TryReadGpuCleanBacking(
				        b.addr + uint64_t {record[3]} * b.stride, bytes.data(), size)) {
					for (uint32_t k = 0; k < size; k++) {
						hex += fmt::format("{:02x}{}", bytes[k], (k % 4 == 3) ? " " : "");
					}
				} else {
					hex = "(not readable now: GPU-written or not on the GPU thread)";
				}
				std::printf("%s  vb%u[%u] @0x%" PRIx64 ": %s%s\n", on, i, record[3],
				            b.addr + uint64_t {record[3]} * b.stride, hex.c_str(), off);
			}
		}
		// A 16-bit indexed draw whose vertex index is above 65535 (+ the vertex offset in the draw
		// text): look for the index pair it would be if the GPU read the 16-bit indices as 32-bit.
		const uint32_t index = record[10] == 1u ? record[3] - record[9] : record[3];
		if (!mesh && index > 0xffffu) {
			// Every indexed draw of the shader in the last frames: is the index in a 32-bit buffer,
			// or the pair (low, high) at an even position of a 16-bit buffer?
			if (auto found = state.frame_indices.find(hash); found != state.frame_indices.end()) {
				for (const auto& slot: found->second) {
					if (slot.frame == 0) {
						continue;
					}
					uint32_t in32 = 0, pairs16 = 0;
					for (const auto& copy: slot.copies32) {
						in32 += static_cast<uint32_t>(std::count(copy.begin(), copy.end(), index));
					}
					for (const auto& copy: slot.copies16) {
						for (size_t k = 0; k + 1 < copy.size(); k += 2) {
							pairs16 += copy[k] == (index & 0xffffu) && copy[k + 1] == (index >> 16u) ? 1u : 0u;
						}
					}
					std::printf("%s  frame %" PRIu64 ": indexed draws 16-bit %u, 32-bit %u; GPU-written "
					            "%u, not copied %u; index found in 32-bit copies %ux, as 16-bit pair %ux%s%c",
					            on, slot.frame, slot.draws16, slot.draws32, slot.gpu_written,
					            slot.unreadable, in32, pairs16, off, 10);
				}
			}
		}
		if (draw != state.last_draw.end() && !mesh && index > 0xffffu &&
		    draw->second.index_size == 2 && draw->second.index_address != 0 &&
		    !draw->second.index_gpu_args) {
			const auto& info    = draw->second;
			const auto& indices = info.indices;
			std::string result;
			if (!indices.empty()) {
				const auto low  = index & 0xffffu;
				const auto high = index >> 16u;
				uint32_t   even = 0, odd = 0, max_index = 0;
				for (uint32_t k = 0; k < indices.size(); k++) {
					max_index = std::max<uint32_t>(max_index, indices[k]);
					if (k + 1 < indices.size() && indices[k] == low && indices[k + 1] == high) {
						((k % 2u) == 0 ? even : odd)++;
					}
				}
				result = fmt::format("16-bit indices max {}; pair ({}, {}) found {}x at even, {}x "
				                     "at odd positions",
				                     max_index, low, high, even, odd);
			} else {
				result = "no copy of the indices (GPU-written or more than 64Ki)";
			}
			std::printf("%s  index check: index %u (vertex - base vertex) > 65535 in a 16-bit draw (idx 0x%" PRIx64
			            " n=%u): %s; this shader's draws in frame %" PRIu64 ": %u (indexed %u, 32-bit %u, "
			            "gpu-args %u), vertex offsets %d..%d%s\n",
			            on, index, info.index_address, info.index_count, result.c_str(),
			            info.frame, info.frame_draws, info.indexed_draws, info.index32_draws,
			            info.gpu_args_draws, info.min_vertex_offset, info.max_vertex_offset, off);
		}
		LOGF("VtxExplode: %s=%016" PRIx64 " kind=%s frame=%" PRIu64 " hits=%u vertex=%u "
		     "instance=%u pos=(%g, %g, %g, %g) | last %s\n",
		     mesh ? "mesh" : "vs", hash, KindName(kind), frame, count, record[3], record[4], x,
		     y, z, w, text.c_str());
	}
	if (frame - state.last_summary >= 600) {
		state.last_summary = frame;
		std::printf("VtxExplode summary: frame %" PRIu64 " hits=%" PRIu64 " nan/inf=%" PRIu64
		            " (%u shaders) w~0=%" PRIu64 " (%u) huge=%" PRIu64 " (%u) zero/cull=%" PRIu64
		            " (%u) | cpu index hits=%" PRIu64 " gpu-written index buffers skipped=%" PRIu64
		            "\n",
		            frame, state.total, state.hits[0], state.shaders[0], state.hits[1],
		            state.shaders[1], state.hits[2], state.shaders[2], state.hits[3],
		            state.shaders[3], state.cpu_hits, state.cpu_gpu_written);
	}
	std::fflush(stdout);
}

void NoteIndexTypeSource(uint32_t source) {
	g_index_type_source.store(source, std::memory_order_relaxed);
}

void SampleGpuIndices() {
	std::vector<PendingSample> samples;
	{
		auto&           state = GetState();
		std::lock_guard lock(state.mutex);
		samples.swap(state.pending_samples);
	}
	for (const auto& s: samples) {
		std::vector<uint32_t> words(s.count);
		if (!LibKernel::Memory::TryReadBufferBacking(s.address, words.data(), words.size() * 4u)) {
			std::printf("VtxExplode GPU-idx32: vs=%016" PRIx64 " idx=0x%" PRIx64 " not readable\n",
			            s.hash, s.address);
			continue;
		}
		// valid: below the first fetch buffer's records (any value when it has none); pair: two
		// 16-bit halves that are both small and the high half not zero.
		uint32_t valid = 0, pairs = 0, other = 0;
		for (const auto w: words) {
			const auto lo = w & 0xffffu, hi = w >> 16u;
			if (s.records != 0 ? w < s.records : hi == 0) {
				valid++;
			} else if (hi != 0 && (s.records != 0 ? (lo < s.records && hi < s.records)
			                                     : (lo < 0x4000u && hi < 0x4000u))) {
				pairs++;
			} else {
				other++;
			}
		}
		std::string first;
		for (size_t k = 0; k < std::min<size_t>(words.size(), 12); k++) {
			first += fmt::format(" {:08x}", words[k]);
		}
		const bool bad = pairs != 0 || other != 0;
		std::printf("%sVtxExplode GPU-idx32: vs=%016" PRIx64 " frame %" PRIu64 " idx=0x%" PRIx64
		            " n=%u type from %s, vb0 records %u: valid %u, two-16-bit-like %u, other %u;"
		            " first:%s%s\n",
		            bad ? "\x1b[91m" : "", s.hash, s.frame, s.address, s.count,
		            IndexTypeSourceName(s.source), s.records, valid, pairs, other, first.c_str(),
		            bad ? "\x1b[0m" : "");
	}
	if (!samples.empty()) {
		std::fflush(stdout);
	}
}

} // namespace Libs::Graphics::VertexExplode
