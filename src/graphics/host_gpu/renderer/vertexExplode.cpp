#include "graphics/host_gpu/renderer/vertexExplode.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/VertexExplodeLog.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
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
};

struct State {
	std::mutex                                 mutex;
	uint64_t                                   draws = 0;
	std::unordered_map<uint64_t, DrawInfo>     last_draw;   // by shader hash
	std::unordered_set<uint64_t>               reported;    // hash * 4 + kind (GPU records)
	std::unordered_set<uint64_t>               cpu_reported; // hash ^ index address
	std::unordered_map<uint64_t, uint64_t>     index_checked; // index address ^ count -> frame
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

void NoteDraw(BufferCache& cache, const DrawNote& note) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.draws++;
	std::string text;
	for (const auto& stage: note.stages) {
		if (stage.stage.program == nullptr) {
			continue;
		}
		const auto hash = stage.stage.program->shader_hash;
		auto&      info = state.last_draw[hash];
		if (info.frame != note.frame || info.draw == 0) {
			// Described once per frame and shader: the text costs more than the draw.
			if (text.empty()) {
				text = DescribeDraw(note, state.draws);
			}
			info.text = text;
			const auto& first = note.stages.front();
			info.buffer_count = static_cast<uint32_t>(std::clamp(first.buffers_num, 0, 4));
			for (uint32_t i = 0; i < info.buffer_count; i++) {
				info.buffers[i] = first.buffers[i];
			}
		}
		info.frame = note.frame;
		info.draw  = state.draws;
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
		state.hits[kind] += count;
		if (!state.reported.insert(hash * 4u + kind).second) {
			continue;
		}
		state.shaders[kind]++;
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
		            " pos=(%g, %g, %g, %g) | last %s%s\n",
		            on, mesh ? "mesh" : "vs", hash, KindName(kind), frame, count,
		            mesh ? "invocation" : "vertex", record[3], mesh ? "group" : "instance",
		            record[4], x, y, z, w, text.c_str(), off);
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

} // namespace Libs::Graphics::VertexExplode
