#include "graphics/host_gpu/renderer/dispatchSnapshot.h"

#include "common/assert.h"
#include "common/switches.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vulkan/vulkan_format_traits.hpp>
#include <xxhash.h>

namespace Libs::Graphics {
bool BindlessImagesActive(); // pipelineCache.cpp
}

namespace Libs::Graphics::DispatchSnapshot {
namespace {

using Json = nlohmann::ordered_json;

struct Settings {
	bool                  enabled = false;
	std::vector<uint64_t> hashes;
	int64_t               frame = -1;
	uint32_t              max   = 0;
	uint32_t              ring  = 0; // KYTY_DISPATCH_SNAPSHOT_RING: keep only the last n snapshots
	std::filesystem::path directory;
};

const Settings& GetSettings() {
	static const Settings settings = [] {
		Settings s;
		const char* text = KYTY_GETENV("KYTY_DISPATCH_SNAPSHOT");
		if (text == nullptr || *text == '\0' || std::strcmp(text, "0") == 0) {
			return s;
		}
		// "1" (the launcher's check box): what dispatch_snapshot.txt names, else frame 600.
		static std::string from_file;
		if (std::strcmp(text, "1") == 0) {
			std::ifstream file("dispatch_snapshot.txt");
			std::getline(file, from_file);
			while (!from_file.empty() && std::isspace(static_cast<unsigned char>(from_file.back()))) {
				from_file.pop_back();
			}
			if (from_file.empty()) from_file = "frame:600";
			text = from_file.c_str();
		}
		const std::string_view value(text);
		if (value.starts_with("frame:")) {
			s.frame = std::strtoll(text + 6, nullptr, 10);
		} else {
			for (const char* p = text; p != nullptr && *p != '\0';) {
				char*      end  = nullptr;
				const auto hash = std::strtoull(p, &end, 16);
				if (end == p) break;
				s.hashes.push_back(hash);
				p = *end == ',' ? end + 1 : nullptr;
			}
		}
		const char* max = KYTY_GETENV("KYTY_DISPATCH_SNAPSHOT_MAX");
		if (const char* ring = KYTY_GETENV("KYTY_DISPATCH_SNAPSHOT_RING"); ring != nullptr) {
			s.ring = static_cast<uint32_t>(std::strtoul(ring, nullptr, 10));
		}
		s.max = max != nullptr ? static_cast<uint32_t>(std::strtoul(max, nullptr, 10))
		                       : (s.frame >= 0 ? 4000u : 8u);
		const char* dir = KYTY_GETENV("KYTY_DISPATCH_SNAPSHOT_DIR");
		s.directory     = dir != nullptr && *dir != '\0' ? dir : "_snapshots";
		s.enabled       = s.frame >= 0 || !s.hashes.empty();
		std::printf("dispatch-snapshot: on (%s), max %u, dir %s\n", text, s.max,
		            s.directory.string().c_str());
		std::fflush(stdout);
		return s;
	}();
	return settings;
}

// One readback: a staging buffer the copies land in, and where its bytes go in the JSON.
struct Readback {
	std::unique_ptr<Buffer> staging;
	uint64_t                size = 0;
	Json*                   target = nullptr; // gets "blob" and "xxh3"
};

class Collector {
public:
	Collector(RenderContext& context): m_context(context) {}

	vk::CommandBuffer Command() { return m_context.GetCommandScheduler().Current().Handle(); }

	// Copies [offset, offset + size) of a host buffer; false (and a note in `target`) when it
	// cannot.
	void AddBuffer(vk::Buffer source, uint64_t offset, uint64_t size, Json& target) {
		target["size"] = size;
		if (source == nullptr) {
			target["null"] = true;
			return;
		}
		if (size == VK_WHOLE_SIZE || size == 0 || size > MaxResourceBytes) {
			target["skipped"] = size == VK_WHOLE_SIZE ? "whole size" : size == 0 ? "empty" : "too large";
			return;
		}
		auto staging = Stage(size);
		if (!m_buffer_barrier_done) {
			// Everything recorded so far (shader writes, uploads) before the copies read.
			vk::MemoryBarrier barrier {};
			barrier.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
			barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
			Command().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
			                          vk::PipelineStageFlagBits::eTransfer, {}, 1, &barrier, 0,
			                          nullptr, 0, nullptr);
			m_buffer_barrier_done = true;
		}
		const vk::BufferCopy copy {offset, 0, size};
		Command().copyBuffer(source, staging->Handle(), 1, &copy);
		m_readbacks.push_back({std::move(staging), size, &target});
	}

	// Copies the view's subresources of the image (each mip of the view, all its layers).
	void AddImage(Image& image, const ImageViewInfo& view, Json& target) {
		const auto& backing = image.backing;
		target["image"]     = {
            {"format", static_cast<int>(backing.format)},
            {"type", static_cast<int>(backing.image_type)},
            {"extent", {backing.extent.width, backing.extent.height, backing.extent.depth}},
            {"mips", backing.mip_levels},
            {"layers", backing.layers},
            {"samples", backing.samples},
            {"flags", static_cast<uint32_t>(backing.flags)},
            {"usage", static_cast<uint32_t>(backing.usage)},
            {"guest_address", image.info.data.address},
            {"guest_type", static_cast<int>(image.info.type)},
        };
		target["view"] = {
		    {"format", static_cast<int>(view.format)},
		    {"type", static_cast<int>(view.type)},
		    {"aspect", static_cast<uint32_t>(view.aspect)},
		    {"base_level", view.base_level},
		    {"level_count", view.level_count},
		    {"min_lod", view.min_lod},
		    {"base_layer", view.base_layer},
		    {"layer_count", view.layer_count},
		    {"mapping",
		     {static_cast<int>(view.mapping.r), static_cast<int>(view.mapping.g),
		      static_cast<int>(view.mapping.b), static_cast<int>(view.mapping.a)}},
		    {"usage", static_cast<uint32_t>(view.usage)},
		};
		if (backing.image == nullptr) {
			target["skipped"] = "no image";
			return;
		}
		if (backing.samples > 1) {
			target["skipped"] = "msaa";
			return;
		}
		vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
		uint32_t             bytes  = vk::blockSize(backing.format);
		auto                 block  = vk::blockExtent(backing.format);
		if (view.aspect & vk::ImageAspectFlagBits::eDepth) {
			aspect = vk::ImageAspectFlagBits::eDepth;
			bytes  = backing.format == vk::Format::eD16Unorm || backing.format == vk::Format::eD16UnormS8Uint ? 2u : 4u;
			block  = {1, 1, 1};
		} else if (view.aspect & vk::ImageAspectFlagBits::eStencil) {
			aspect = vk::ImageAspectFlagBits::eStencil;
			bytes  = 1;
			block  = {1, 1, 1};
		}
		if (bytes == 0) {
			target["skipped"] = "format";
			return;
		}
		const bool volume = backing.image_type == vk::ImageType::e3D;
		std::vector<vk::BufferImageCopy> copies;
		Json                             regions = Json::array();
		uint64_t                         offset  = 0;
		const uint32_t last = std::min(view.base_level + view.level_count, backing.mip_levels);
		for (uint32_t mip = view.base_level; mip < last; mip++) {
			const uint32_t w      = std::max(backing.extent.width >> mip, 1u);
			const uint32_t h      = std::max(backing.extent.height >> mip, 1u);
			const uint32_t d      = volume ? std::max(backing.extent.depth >> mip, 1u) : 1u;
			const uint32_t layers = volume ? 1u : view.layer_count;
			const uint64_t size   = uint64_t {(w + block[0] - 1) / block[0]} *
			                      ((h + block[1] - 1) / block[1]) * d * layers * bytes;
			vk::BufferImageCopy copy {};
			copy.bufferOffset     = offset;
			copy.imageSubresource = {aspect, mip, volume ? 0u : view.base_layer, layers};
			copy.imageExtent      = vk::Extent3D {w, h, d};
			copies.push_back(copy);
			regions.push_back({{"mip", mip},
			                   {"base_layer", volume ? 0u : view.base_layer},
			                   {"layers", layers},
			                   {"extent", {w, h, d}},
			                   {"offset", offset},
			                   {"size", size}});
			offset = (offset + size + 15u) & ~uint64_t {15u};
		}
		if (copies.empty() || offset > MaxResourceBytes) {
			target["skipped"] = copies.empty() ? "no mips" : "too large";
			return;
		}
		target["aspect"]  = static_cast<uint32_t>(aspect);
		target["regions"] = std::move(regions);
		target["size"]    = offset;
		auto staging      = Stage(offset);
		const ImageSubresourceRange range {view.base_level, last - view.base_level,
		                                   volume ? 0u : view.base_layer, volume ? 1u : view.layer_count};
		image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, range,
		              Command());
		Command().copyImageToBuffer(backing.image, vk::ImageLayout::eTransferSrcOptimal,
		                            staging->Handle(), static_cast<uint32_t>(copies.size()),
		                            copies.data());
		m_readbacks.push_back({std::move(staging), offset, &target});
	}

	// Waits for the copies and writes each readback as a blob.
	void Finish(const std::filesystem::path& blobs) {
		if (m_readbacks.empty()) {
			return;
		}
		vk::MemoryBarrier barrier {};
		barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
		Command().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
		                          {}, 1, &barrier, 0, nullptr, 0, nullptr);
		m_context.GetCommandScheduler().FlushAndWait();
		std::error_code error;
		std::filesystem::create_directories(blobs, error);
		for (auto& readback: m_readbacks) {
			readback.staging->Invalidate(0, readback.size);
			const auto bytes = readback.staging->Mapped().first(readback.size);
			const auto hash  = XXH3_64bits(bytes.data(), bytes.size());
			const auto name  = fmt::format("{:016x}", hash);
			const auto path  = blobs / (name + ".bin");
			if (!std::filesystem::exists(path, error)) {
				std::ofstream file(path, std::ios::binary);
				file.write(reinterpret_cast<const char*>(bytes.data()),
				           static_cast<std::streamsize>(bytes.size()));
			}
			(*readback.target)["blob"] = name;
		}
		m_readbacks.clear();
	}

private:
	static constexpr uint64_t MaxResourceBytes = 1ull << 30u;

	std::unique_ptr<Buffer> Stage(uint64_t size) {
		return std::make_unique<Buffer>(m_context.GetGraphics(), m_context.GetCommandScheduler(),
		                                MemoryUsage::Download, 0, AllFlags, size);
	}

	RenderContext&        m_context;
	std::vector<Readback> m_readbacks;
	bool                  m_buffer_barrier_done = false;
};

// The host memory the shader's buffers cover. Several V#s of one dispatch often view the same
// memory (Ghost of Yotei binds one record array through descriptors of different strides), so
// buffers of one host buffer that overlap or lie close together share a region: the replay binds
// them into one buffer, and a write through one view is seen through the others, as live.
struct BufferRegion {
	vk::Buffer buffer;
	uint64_t   begin   = 0;
	uint64_t   end     = 0;
	bool       written = false;
};

std::vector<BufferRegion> BufferRegions(const PreparedBindings&                        bindings,
                                        const ShaderRecompiler::IR::CompiledShaderInfo& program,
                                        std::vector<int>&                               region_of) {
	constexpr uint64_t Gap   = 64u * 1024u;
	constexpr uint64_t Align = 256u;
	std::vector<BufferRegion> regions;
	region_of.assign(bindings.buffers.size(), -1);
	std::vector<size_t> order;
	for (size_t i = 0; i < bindings.buffers.size(); i++) {
		const auto& view = bindings.buffers[i];
		if (view.buffer != nullptr && view.range != 0 && view.range != VK_WHOLE_SIZE) order.push_back(i);
	}
	std::ranges::sort(order, [&](size_t a, size_t b) {
		const auto& x = bindings.buffers[a];
		const auto& y = bindings.buffers[b];
		return std::pair(static_cast<VkBuffer>(x.buffer), x.offset) <
		       std::pair(static_cast<VkBuffer>(y.buffer), y.offset);
	});
	for (const auto i: order) {
		const auto& view    = bindings.buffers[i];
		const bool  written = i < program.info.buffers.size() && program.info.buffers[i].written;
		const auto  begin   = view.offset & ~(Align - 1u);
		if (regions.empty() || regions.back().buffer != view.buffer || begin > regions.back().end + Gap) {
			regions.push_back({view.buffer, begin, view.offset + view.range, written});
		} else {
			auto& last   = regions.back();
			last.end     = std::max(last.end, view.offset + view.range);
			last.written = last.written || written;
		}
		region_of[i] = static_cast<int>(regions.size() - 1);
	}
	return regions;
}

Json DescriptorJson(const ShaderRecompiler::IR::DescriptorValue& value) {
	Json words = Json::array();
	for (uint32_t i = 0; i < value.dword_count && i < value.dwords.size(); i++) {
		words.push_back(value.dwords[i]);
	}
	return words;
}

const ImageViewInfo* FindViewInfo(const Image& image, vk::ImageView view) {
	const auto found = std::ranges::find(image.views, view, &CachedImageView::view);
	return found == image.views.end() ? nullptr : &found->info;
}

void WriteJson(const std::filesystem::path& path, const Json& json) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << json.dump(1);
}

// The open snapshots' JSON, by id, until their outputs complete them.
std::unordered_map<uint32_t, Json>& OpenSnapshots() {
	static std::unordered_map<uint32_t, Json> open;
	return open;
}

} // namespace

namespace {
struct DrawSettings {
	bool                  enabled = false;
	std::vector<uint64_t> hashes;
	int64_t               frame = -1;
	uint32_t              max   = 0;
	std::filesystem::path directory;
};

const DrawSettings& GetDrawSettings() {
	static const DrawSettings settings = [] {
		DrawSettings s;
		const char*  text = KYTY_GETENV("KYTY_DRAW_SNAPSHOT");
		if (text == nullptr || *text == '\0' || std::strcmp(text, "0") == 0) {
			return s;
		}
		// "1" (the launcher's check box): what draw_snapshot.txt names, else frame 600.
		static std::string from_file;
		if (std::strcmp(text, "1") == 0) {
			std::ifstream file("draw_snapshot.txt");
			std::getline(file, from_file);
			while (!from_file.empty() && std::isspace(static_cast<unsigned char>(from_file.back()))) {
				from_file.pop_back();
			}
			if (from_file.empty()) from_file = "frame:600";
			text = from_file.c_str();
		}
		const std::string_view value(text);
		if (value.starts_with("frame:")) {
			s.frame = std::strtoll(text + 6, nullptr, 10);
		} else {
			for (const char* p = text; p != nullptr && *p != '\0';) {
				char*      end  = nullptr;
				const auto hash = std::strtoull(p, &end, 16);
				if (end == p) break;
				s.hashes.push_back(hash);
				p = *end == ',' ? end + 1 : nullptr;
			}
		}
		const char* max = KYTY_GETENV("KYTY_DRAW_SNAPSHOT_MAX");
		s.max = max != nullptr ? static_cast<uint32_t>(std::strtoul(max, nullptr, 10))
		                       : (s.frame >= 0 ? 3000u : 4u);
		const char* dir = KYTY_GETENV("KYTY_DRAW_SNAPSHOT_DIR");
		s.directory     = dir != nullptr && *dir != '\0' ? dir : "_draw_snapshots";
		s.enabled       = s.frame >= 0 || !s.hashes.empty();
		std::printf("draw-snapshot: on (%s), max %u, dir %s\n", text, s.max, s.directory.string().c_str());
		std::fflush(stdout);
		return s;
	}();
	return settings;
}

Json StageGuestJson(const ShaderStageRuntime& stage) {
	const auto& resources = *stage.resources;
	Json        guest;
	guest["user_data"]     = resources.user_data;
	guest["flattened_srt"] = resources.flattened_srt;
	guest["buffers"]       = Json::array();
	for (const auto& value: resources.buffers) guest["buffers"].push_back(DescriptorJson(value));
	guest["images"] = Json::array();
	for (const auto& value: resources.images) guest["images"].push_back(DescriptorJson(value));
	guest["samplers"] = Json::array();
	for (const auto& value: resources.samplers) guest["samplers"].push_back(DescriptorJson(value));
	return guest;
}

Json PreparedBufferJson(const vk::DescriptorBufferInfo& view) {
	const auto handle = static_cast<VkBuffer>(view.buffer);
	uint64_t opaque = 0;
	static_assert(sizeof(handle) <= sizeof(opaque));
	std::memcpy(&opaque, &handle, sizeof(handle));
	return {{"buffer_handle", fmt::format("{:016x}", opaque)},
	        {"offset", view.offset}, {"range", view.range}, {"null", view.buffer == nullptr}};
}

void WriteStageSource(const ShaderStageRuntime& stage, const std::filesystem::path& folder,
                      const std::string& prefix, Json& entry) {
	const auto& program = *stage.program;
	const auto* source  = static_cast<const Source*>(program.snapshot_source.get());
	entry["record"]     = source != nullptr;
	if (source == nullptr) {
		return;
	}
	std::vector<uint8_t> bytes;
	if (ShaderPrecompile::Encode(source->record, bytes)) {
		std::ofstream(folder / (prefix + "_record.bin"), std::ios::binary)
		    .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
	std::ofstream(folder / (prefix + "_live.spv"), std::ios::binary)
	    .write(reinterpret_cast<const char*>(source->spirv.data()),
	           static_cast<std::streamsize>(source->spirv.size() * sizeof(uint32_t)));
}
} // namespace

bool DrawWanted(uint64_t vertex_hash, uint64_t pixel_hash, uint64_t frame) {
	const auto& s = GetDrawSettings();
	if (!s.enabled) [[likely]] {
		return false;
	}
	if (s.frame >= 0) {
		static uint32_t taken  = 0;
		static int64_t  chosen = -1;
		if (chosen < 0 && frame >= static_cast<uint64_t>(s.frame)) {
			chosen = static_cast<int64_t>(frame);
		}
		return chosen >= 0 && frame == static_cast<uint64_t>(chosen) && taken++ < s.max;
	}
	uint64_t key = 0;
	if (std::ranges::find(s.hashes, vertex_hash) != s.hashes.end()) {
		key = vertex_hash;
	} else if (pixel_hash != 0 && std::ranges::find(s.hashes, pixel_hash) != s.hashes.end()) {
		key = pixel_hash;
	} else {
		return false;
	}
	static std::unordered_map<uint64_t, uint32_t> taken;
	return taken[key]++ < s.max;
}

void CaptureDraw(RenderContext& context, const DrawCapture& draw) {
	static uint32_t next_id = 0;
	const auto&     s       = GetDrawSettings();
	const auto&     vs      = draw.vertex_stages[0];
	const uint64_t  vs_hash = vs.stage.program->shader_hash;
	const uint64_t  ps_hash =
	    draw.pixel != nullptr && draw.pixel->program != nullptr ? draw.pixel->program->shader_hash : 0;
	const auto      id     = ++next_id;
	const auto      name   = fmt::format("{:05}_{:016x}_{:016x}_f{}", id, vs_hash, ps_hash, draw.frame);
	const auto      folder = s.directory / name;
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	if (error) {
		std::printf("draw-snapshot: cannot create %s\n", folder.string().c_str());
		return;
	}
	Json meta;
	meta["version"] = 1;
	meta["frame"]   = draw.frame;
	meta["vs"]      = fmt::format("{:016x}", vs_hash);
	meta["ps"]      = fmt::format("{:016x}", ps_hash);
	meta["draw"]    = {{"primitive", draw.primitive},
	                   {"topology", draw.topology},
	                   {"indexed", draw.indexed},
	                   {"gpu_args", draw.gpu_args},
	                   {"restart", draw.restart},
	                   {"index_count", draw.index_count},
	                   {"instance_count", draw.instance_count},
	                   {"first_instance", draw.first_instance},
	                   {"vertex_offset", draw.vertex_offset},
	                   {"first_vertex", draw.first_vertex}};
	meta["targets"] = {{"colors", Json::array()}};
	for (const auto& c: draw.colors) {
		meta["targets"]["colors"].push_back(
		    {{"address", c[0]}, {"format", c[1]}, {"width", c[2]}, {"height", c[3]}});
	}
	meta["targets"]["depth"] = {{"address", draw.depth[0]},
	                            {"format", draw.depth[1]},
	                            {"width", draw.depth[2]},
	                            {"height", draw.depth[3]}};
	// The fetch: guest vertex buffers the fetch reads and its attributes.
	Json fetch;
	fetch["buffers"] = Json::array();
	for (int i = 0; i < vs.buffers_num && i < ShaderVertexInputInfo::RES_MAX; i++) {
		const auto& b = vs.buffers[i];
		fetch["buffers"].push_back({{"address", b.addr},
		                            {"stride", b.stride},
		                            {"num_records", b.num_records},
		                            {"fetch_index", b.fetch_index}});
	}
	fetch["attributes"] = Json::array();
	for (int i = 0; i < vs.resources_num && i < ShaderVertexInputInfo::RES_MAX; i++) {
		const auto& r = vs.resources[i];
		const auto& d = vs.resources_dst[i];
		fetch["attributes"].push_back({{"words", {r.fields[0], r.fields[1], r.fields[2], r.fields[3]}},
		                               {"address", r.Base48()},
		                               {"stride", r.Stride()},
		                               {"num_records", r.NumRecords()},
		                               {"format", static_cast<uint32_t>(r.Format())},
		                               {"register_start", d.register_start},
		                               {"registers", d.registers_num},
		                               {"attr_id", d.attr_id},
		                               {"buffer_index", d.buffer_index}});
	}
	meta["fetch"] = std::move(fetch);
	// Every key the readbacks fill exists before the first one: they keep pointers into meta.
	meta["index_buffer"]   = {{"address", draw.index_address},
	                          {"guest_size", draw.index_guest_size},
	                          {"guest_index_size", draw.guest_index_size},
	                          {"host_index_size", draw.host_index_size}};
	meta["vertex_buffers"] = Json::array();
	for (uint32_t i = 0; i < draw.vertex_buffer_count; i++) {
		meta["vertex_buffers"].push_back({{"index", i}});
	}
	meta["stages"] = Json::array();
	constexpr uint64_t PreparedCaptureBudget = 32ull * 1024u * 1024u;
	meta["prepared_descriptor_capture"] = {
	    {"schema", 1}, {"origin", "prepared bindings before graphics commit"},
	    {"scheduler_tick", context.GetCommandScheduler().CurrentTick()},
	    {"budget_bytes", PreparedCaptureBudget}, {"captured_bytes", 0}, {"skipped_ranges", 0}};
	auto& buffers  = context.GetBufferCache();
	for (uint32_t st = 0; st < draw.binding_count; st++) {
		const auto* prepared = draw.bindings[st];
		const bool  pixel    = st >= draw.vertex_count;
		const auto& runtime  = pixel ? *draw.pixel : draw.vertex_stages[st].stage;
		Json        stage;
		stage["kind"]    = pixel ? "pixel" : (st == 0 ? "vertex" : "vertex-stage");
		stage["shader"]  = fmt::format("{:016x}", runtime.program->shader_hash);
		stage["guest"]   = StageGuestJson(runtime);
		stage["buffers_origin"] = "refound guest cache owner; not the prepared descriptor";
		stage["buffers"] = Json::array();
		for (size_t i = 0; i < prepared->buffer_sources.size(); i++) {
			const auto& src = prepared->buffer_sources[i];
			stage["buffers"].push_back({{"index", i},
			                            {"address", src.address},
			                            {"size", src.size},
			                            {"written", i < runtime.program->info.buffers.size() &&
			                                            runtime.program->info.buffers[i].written}});
		}
		const auto& program = *prepared->runtime->program;
		const auto& layout = program.bindings;
		stage["prepared_program_shader"] = fmt::format("{:016x}", program.shader_hash);
		stage["prepared_program_matches_stage"] = prepared->runtime->program == runtime.program;
		stage["shader_data"] = prepared->shader_data;
		stage["prepared_shader_data_buffer"] = PreparedBufferJson(prepared->shader_data_buffer);
		stage["binding_layout"] = {
		    {"table", layout.table}, {"table_srt_dword", layout.table_srt_dword},
		    {"push_data_start_dword", layout.push_data_start_dword},
		    {"memory_offset_dword", layout.memory_offset_dword},
		    {"memory_offset_count", layout.memory_offset_count},
		    {"buffer_stride_dword", layout.BufferStrideDword()},
		    {"buffer_format_dword", layout.BufferFormatDword()},
		    {"buffer_strides", layout.buffer_strides}, {"buffer_formats", layout.buffer_formats},
		    {"user_data_registers", layout.user_data_registers},
		    {"reserve_object_id", layout.reserve_object_id},
		    {"shader_data_dwords", layout.ShaderDataDwords()}, {"descriptors", Json::array()}};
		for (size_t i = 0; i < layout.descriptors.size(); i++) {
			const auto& binding = layout.descriptors[i];
			stage["binding_layout"]["descriptors"].push_back(
			    {{"layout_index", i}, {"kind", static_cast<uint32_t>(binding.kind)},
			     {"resources", binding.resources}});
		}
		stage["prepared_descriptor_buffers_status"] =
		    layout.table ? "unavailable: table BDA ranges are not retained here" : "prepared ranges";
		stage["prepared_descriptor_buffers"] = Json::array();
		for (size_t i = 0; !layout.table && i < prepared->buffers.size(); i++) {
			auto entry = PreparedBufferJson(prepared->buffers[i]);
			entry["index"] = i;
			const auto dword = static_cast<uint64_t>(layout.memory_offset_dword) + i / 4u;
			entry["memory_adjustment_bytes"] = nullptr;
			if (i < layout.memory_offset_count && dword < prepared->shader_data.size()) {
				entry["memory_adjustment_bytes"] =
				    (prepared->shader_data[dword] >> ((i % 4u) * 8u)) & 0xffu;
			}
			if (i < prepared->buffer_sources.size()) {
				const auto& source = prepared->buffer_sources[i];
				entry["guest_source"] = {{"address", source.address}, {"size", source.size}};
			}
			if (i < program.info.buffers.size()) {
				const auto& resource = program.info.buffers[i];
				entry["resource"] = {{"source", resource.source},
				                     {"packed_stride", resource.packed_stride},
				                     {"descriptor_format", static_cast<uint32_t>(resource.descriptor_format)},
				                     {"descriptor_swizzle", resource.descriptor_swizzle},
				                     {"read", resource.read}, {"written", resource.written},
				                     {"formatted", resource.formatted}};
			}
			stage["prepared_descriptor_buffers"].push_back(std::move(entry));
		}
		stage["flattened_srt"] = Json::object();
		WriteStageSource(runtime, folder, pixel ? std::string("ps") : fmt::format("vs{}", st), stage);
		meta["stages"].push_back(std::move(stage));
	}

	Collector collector(context);
	if (draw.indexed && draw.index_buffer != nullptr && draw.host_index_size != 0) {
		const uint64_t count = draw.gpu_args ? draw.index_guest_size / std::max(draw.guest_index_size, 1u)
		                                     : draw.index_count;
		collector.AddBuffer(draw.index_buffer, draw.index_offset, count * draw.host_index_size,
		                    meta["index_buffer"]);
	}
	for (uint32_t i = 0; i < draw.vertex_buffer_count; i++) {
		collector.AddBuffer(draw.vertex_buffers[i], draw.vertex_offsets[i], draw.vertex_sizes[i],
		                    meta["vertex_buffers"][i]);
	}
	uint64_t prepared_bytes = 0;
	uint32_t prepared_skipped = 0;
	for (uint32_t st = 0; st < draw.binding_count; st++) {
		const auto* prepared = draw.bindings[st];
		auto&       stage    = meta["stages"][st];
		// Copy the exact prepared ranges before the legacy refound observations can discover
		// another owner. No cache lookup, upload or resynchronization selects these bytes.
		for (size_t i = 0; i < stage["prepared_descriptor_buffers"].size(); i++) {
			const auto& view = prepared->buffers[i];
			auto& entry = stage["prepared_descriptor_buffers"][i];
			entry["size"] = view.range;
			const char* skipped = nullptr;
			if (view.buffer == nullptr) skipped = "null descriptor";
			else if (view.range == VK_WHOLE_SIZE) skipped = "whole size";
			else if (view.range == 0) skipped = "empty";
			else if (view.offset > UINT64_MAX - view.range) skipped = "offset/range overflow";
			else if (view.range > PreparedCaptureBudget - prepared_bytes)
				skipped = "prepared descriptor capture budget";
			if (skipped != nullptr) {
				entry["skipped"] = skipped;
				prepared_skipped++;
				continue;
			}
			collector.AddBuffer(view.buffer, view.offset, view.range, entry);
			prepared_bytes += view.range;
		}
	}
	for (uint32_t st = 0; st < draw.binding_count; st++) {
		const auto* prepared = draw.bindings[st];
		auto&       stage    = meta["stages"][st];
		for (size_t i = 0; i < prepared->buffer_sources.size(); i++) {
			const auto& src   = prepared->buffer_sources[i];
			auto&       entry = stage["buffers"][i];
			if (src.address == 0 || src.size == 0) {
				entry["skipped"] = "empty";
				continue;
			}
			const auto found = buffers.RefindBuffer(src.id, src.address, src.size);
			auto&      host  = buffers.GetBuffer(found);
			if (!host.IsInBounds(src.address, src.size)) {
				entry["skipped"] = "not in one buffer";
				continue;
			}
			collector.AddBuffer(host.Handle(), host.Offset(src.address), src.size, entry);
		}
		if (prepared->flattened_srt.buffer != nullptr) {
			collector.AddBuffer(prepared->flattened_srt.buffer, prepared->flattened_srt.offset,
			                    prepared->flattened_srt.range, stage["flattened_srt"]);
		}
	}
	meta["prepared_descriptor_capture"]["captured_bytes"] = prepared_bytes;
	meta["prepared_descriptor_capture"]["skipped_ranges"] = prepared_skipped;
	collector.Finish(s.directory / "blobs");
	WriteJson(folder / "meta.json", meta);
	std::printf("draw-snapshot: %s written\n", name.c_str());
	std::fflush(stdout);
}

bool Enabled() {
	return GetSettings().enabled;
}

namespace {
const std::vector<std::pair<uint64_t, uint64_t>>& WatchRanges() {
	static const auto ranges = [] {
		std::vector<std::pair<uint64_t, uint64_t>> list;
		const char* text = KYTY_GETENV("KYTY_WRITE_WATCH");
		for (const char* p = text; p != nullptr && *p != '\0';) {
			char*      end     = nullptr;
			const auto address = std::strtoull(p, &end, 16);
			if (end == p) break;
			uint64_t size = 4;
			if (*end == '+') size = std::strtoull(end + 1, &end, 16);
			list.emplace_back(address, std::max<uint64_t>(size, 1));
			p = *end == ',' ? end + 1 : nullptr;
		}
		for (const auto& [address, size]: list) {
			std::printf("write-watch: 0x%llx+0x%llx\n", static_cast<unsigned long long>(address),
			            static_cast<unsigned long long>(size));
		}
		std::fflush(stdout);
		return list;
	}();
	return ranges;
}
} // namespace

bool WatchActive() {
	return !WatchRanges().empty();
}

void WatchWrite(const char* source, uint64_t address, uint64_t size, uint64_t detail) {
	if (size == 0) return;
	for (const auto& [watched, length]: WatchRanges()) {
		if (address < watched + length && watched < address + size) {
			static std::atomic<uint32_t> lines {0};
			if (lines.fetch_add(1, std::memory_order_relaxed) < 2000u) {
				std::printf("\x1b[91mwrite-watch: %s writes 0x%llx+0x%llx (watched 0x%llx) detail=0x%llx\x1b[0m\n",
				            source, static_cast<unsigned long long>(address),
				            static_cast<unsigned long long>(size), static_cast<unsigned long long>(watched),
				            static_cast<unsigned long long>(detail));
				std::fflush(stdout);
			}
			return;
		}
	}
}

void WatchDispatch(const ShaderComputeInputInfo& input) {
	const auto& program   = *input.stage.program;
	const auto& resources = *input.stage.resources;
	for (size_t i = 0; i < program.info.buffers.size() && i < resources.buffers.size(); i++) {
		if (!program.info.buffers[i].written || resources.buffers[i].dword_count < 4) continue;
		const auto descriptor = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const auto source     = fmt::format("cs {:016x} buffer {}", program.shader_hash, i);
		WatchWrite(source.c_str(), descriptor.Base48(), descriptor.GetSize(),
		           uint64_t {input.workgroup_counts[0]} | (uint64_t {input.workgroup_counts[1]} << 32u));
	}
}

bool Wanted(uint64_t shader_hash, uint64_t frame) {
	const auto& s = GetSettings();
	if (!s.enabled) [[likely]] {
		return false;
	}
	if (s.frame >= 0) {
		// The first frame from n on that dispatches anything.
		static uint32_t taken  = 0;
		static int64_t  chosen = -1;
		if (chosen < 0 && frame >= static_cast<uint64_t>(s.frame)) {
			chosen = static_cast<int64_t>(frame);
		}
		return chosen >= 0 && frame == static_cast<uint64_t>(chosen) && taken++ < s.max;
	}
	if (std::ranges::find(s.hashes, shader_hash) == s.hashes.end()) {
		return false;
	}
	static std::unordered_map<uint64_t, uint32_t> taken;
	return s.ring != 0 || taken[shader_hash]++ < s.max;
}

Pending CaptureInputs(RenderContext& context, const ShaderComputeInputInfo& input,
                      const PreparedBindings& bindings, std::array<uint32_t, 3> groups,
                      uint64_t frame, vk::Buffer args, uint64_t args_offset) {
	static uint32_t next_id = 0;
	const auto&     s       = GetSettings();
	const auto&     program = *input.stage.program;
	const auto&     resources = *input.stage.resources;
	Pending         pending;
	pending.id        = ++next_id;
	const auto name   = fmt::format("{:05}_{:016x}_f{}", pending.id, program.shader_hash, frame);
	const auto folder = s.directory / name;
	pending.directory = folder.string();
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	if (error) {
		std::printf("dispatch-snapshot: cannot create %s\n", pending.directory.c_str());
		return {};
	}

	Json meta;
	meta["version"]  = 1;
	meta["shader"]   = fmt::format("{:016x}", program.shader_hash);
	meta["frame"]    = frame;
	meta["tick"]     = context.GetCommandScheduler().CurrentTick();
	meta["groups"]   = {groups[0], groups[1], groups[2]};
	meta["indirect"] = args != nullptr;
	meta["threads"]  = {input.threads_num[0], input.threads_num[1], input.threads_num[2]};
	meta["dispatch_threads"] = {input.dispatch_threads_num[0], input.dispatch_threads_num[1],
	                            input.dispatch_threads_num[2]};
	auto& graphics  = context.GetGraphics();
	meta["globals"] = {
	    {"hw_bounds", ShaderRecompiler::HardwareStorageBufferBounds()},
	    {"native_fp16", ShaderRecompiler::NativeFloat16()},
	    {"table_mode", ShaderRecompiler::TableMode()},
	    {"bindless_images", BindlessImagesActive()},
	    {"max_lds", graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize},
	    {"device", std::string(graphics.GetPhysicalDeviceProperties().deviceName.data())},
	};
	meta["flags"] = {
	    {"uses_dma", program.info.uses_dma},
	    {"uses_bindless", !resources.bindless_heaps.empty() || !resources.bindless_sampler_heaps.empty()},
	    {"bindless_images", bindings.bindless_images.size()},
	    {"table", program.bindings.table},
	};
	// The bindless heaps the shader indexes, and how far the host had resolved their keys when it
	// ran (a key not resolved yet samples the placeholder).
	meta["bindless_heaps"] = Json::array();
	for (const auto& use: resources.bindless_heaps) {
		Json entry = {{"base", use.base},
		              {"size", use.size},
		              {"table_offset", use.table_offset},
		              {"record_stride", use.record_stride},
		              {"pointer_offset", use.pointer_offset},
		              {"image", use.image},
		              {"mapping_offset", use.mapping_offset}};
		for (const auto& heap: context.GetBindlessTable().Heaps()) {
			if (heap.base != use.base || heap.table_offset != use.table_offset ||
			    heap.record_stride != use.record_stride || heap.pointer_offset != use.pointer_offset) {
				continue;
			}
			entry["heap_binding"]  = heap.binding;
			entry["heap_region"]   = heap.region;
			entry["heap_entries"]  = heap.entries;
			entry["keys_settled"]  = std::ranges::count_if(heap.settled, [](uint8_t s) { return s != 0; });
			entry["keys_with_slot"] = std::ranges::count_if(heap.slots, [](uint32_t s) { return s != 0; });
			entry["images_resolved"] = heap.resolved.size();
			entry["checked_frame"] = heap.checked_frame;
			break;
		}
		meta["bindless_heaps"].push_back(std::move(entry));
	}
	meta["bindless_patches"] = Json::array();
	for (const auto& patch: bindings.bindless_patches) {
		meta["bindless_patches"].push_back({patch[0], patch[1], patch[2]});
	}
	meta["push_data_start"] = program.bindings.push_data_start_dword;
	meta["shader_data"]     = bindings.shader_data;
	meta["dispatch_thread_dword"] = program.bindings.dispatch_thread_dword;

	// The guest side, for reading the snapshot (not needed to replay it).
	Json guest;
	guest["user_data"]     = resources.user_data;
	guest["flattened_srt"] = resources.flattened_srt;
	guest["buffers"]       = Json::array();
	for (const auto& value: resources.buffers) guest["buffers"].push_back(DescriptorJson(value));
	guest["images"] = Json::array();
	for (const auto& value: resources.images) guest["images"].push_back(DescriptorJson(value));
	guest["samplers"] = Json::array();
	for (const auto& value: resources.samplers) guest["samplers"].push_back(DescriptorJson(value));
	meta["guest"] = std::move(guest);

	// The journal record and the live SPIR-V.
	const auto* source = static_cast<const Source*>(program.snapshot_source.get());
	meta["record"]     = source != nullptr;
	if (source != nullptr) {
		std::vector<uint8_t> bytes;
		if (ShaderPrecompile::Encode(source->record, bytes)) {
			std::ofstream(folder / "record.bin", std::ios::binary)
			    .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		std::ofstream(folder / "live.spv", std::ios::binary)
		    .write(reinterpret_cast<const char*>(source->spirv.data()),
		           static_cast<std::streamsize>(source->spirv.size() * sizeof(uint32_t)));
	}

	Collector collector(context);
	auto&     cache = context.GetTextureCache();
	// Reserve every array first: the collector keeps pointers into them.
	std::vector<int> region_of;
	const auto       regions = BufferRegions(bindings, program, region_of);
	meta["buffer_regions"]   = Json::array();
	for (size_t r = 0; r < regions.size(); r++) {
		meta["buffer_regions"].push_back({{"id", r}, {"written", regions[r].written}});
	}
	meta["buffers"] = Json::array();
	for (size_t i = 0; i < bindings.buffers.size(); i++) {
		const auto& view = bindings.buffers[i];
		Json        entry;
		entry["index"]   = i;
		entry["written"] = i < program.info.buffers.size() && program.info.buffers[i].written;
		entry["size"]    = view.range;
		if (view.buffer == nullptr) {
			entry["null"] = true;
		} else if (region_of[i] < 0) {
			entry["skipped"] = view.range == 0 ? "empty" : "whole size";
		} else {
			entry["region"] = region_of[i];
			entry["offset"] = view.offset - regions[region_of[i]].begin;
		}
		meta["buffers"].push_back(std::move(entry));
	}
	meta["images"] = Json::array();
	for (size_t i = 0; i < bindings.images.size(); i++) {
		const auto& texture  = bindings.images[i];
		const auto  elements = texture.mip_views.empty() ? 1u : static_cast<uint32_t>(texture.mip_views.size());
		for (uint32_t e = 0; e < elements; e++) {
			Json entry;
			entry["index"]   = i;
			entry["element"] = e;
			entry["storage"] = texture.desc.type == TextureCache::BindingType::Storage;
			entry["written"] = i < program.info.images.size() && program.info.images[i].written;
			meta["images"].push_back(std::move(entry));
		}
	}
	meta["samplers"] = Json::array();
	for (size_t i = 0; i < program.info.samplers.size(); i++) {
		const auto& sampler = program.info.samplers[i];
		Json        entry;
		entry["index"] = i;
		if (sampler.snapshot_index < resources.samplers.size()) {
			auto descriptor = DecodeNativeDescriptor<ShaderSamplerResource>(resources.samplers[sampler.snapshot_index]);
			// As NativeSampler (descriptors.cpp).
			if (!sampler.depth_compare) {
				descriptor.fields[0] &= ~(0x7u << 12u);
			}
			if (sampler.force_point_filtering) {
				descriptor.SetPointFiltering();
			}
			entry["words"] = {descriptor.fields[0], descriptor.fields[1], descriptor.fields[2],
			                  descriptor.fields[3]};
		}
		entry["integer_border"] = sampler.integer_border;
		meta["samplers"].push_back(std::move(entry));
	}
	meta["flattened_srt"] = Json::object();
	meta["shader_data_buffer"] = Json::object();
	meta["gds"]           = Json::object();
	meta["args"]          = Json::object();
	// Every key the readbacks fill in exists before the first one: they keep pointers into meta.
	meta["bda_pages"]          = Json::array();
	meta["bda_unmapped_pages"] = 0;
	meta["shared_memory"] = bindings.shared_memory.buffer != nullptr ? bindings.shared_memory.range : 0;
	{
		// Not captured (v1): the replay binds zeroed buffers of these sizes.
		auto& buffers = context.GetBufferCache();
		const auto* page_table = buffers.GetBdaPageTableBuffer();
		const auto* faults     = buffers.GetFaultBuffer();
		meta["bda_page_table"] = page_table != nullptr ? page_table->Size() : 0;
		meta["fault_buffer"]   = faults != nullptr ? faults->Size() : 0;
	}

	for (size_t r = 0; r < regions.size(); r++) {
		collector.AddBuffer(regions[r].buffer, regions[r].begin, regions[r].end - regions[r].begin,
		                    meta["buffer_regions"][r]);
	}
	size_t image_entry = 0;
	for (size_t i = 0; i < bindings.images.size(); i++) {
		const auto& texture  = bindings.images[i];
		const auto  elements = texture.mip_views.empty() ? 1u : static_cast<uint32_t>(texture.mip_views.size());
		auto*       image    = (cache.PeekImage(texture.image_id) != nullptr ? &cache.GetImage(texture.image_id) : nullptr);
		for (uint32_t e = 0; e < elements; e++) {
			auto&      entry = meta["images"][image_entry++];
			const auto view  = texture.mip_views.empty() ? texture.image_view : texture.mip_views[e];
			const auto* info = image != nullptr ? FindViewInfo(*image, view) : nullptr;
			if (info == nullptr) {
				entry["skipped"] = image == nullptr ? "no image" : "view not found";
				continue;
			}
			collector.AddImage(*image, *info, entry);
		}
	}
	const auto add_view = [&](const vk::DescriptorBufferInfo& view, const char* key) {
		if (view.buffer != nullptr) {
			collector.AddBuffer(view.buffer, view.offset, view.range, meta[key]);
		}
	};
	add_view(bindings.flattened_srt, "flattened_srt");
	add_view(bindings.shader_data_buffer, "shader_data_buffer");
	add_view(bindings.gds, "gds");
	if (args != nullptr) {
		collector.AddBuffer(args, args_offset, sizeof(vk::DispatchIndirectCommand), meta["args"]);
	}
	// BDA: the shader reads guest memory by address through the page table, which maps the whole
	// address space and is not captured. The pages around every pointer in its user data and
	// flattened SRT are (plus KYTY_DISPATCH_SNAPSHOT_PAGES=<hex addr>+<hex size>,...), as the
	// cache buffers hold them; the replay reports the pages it read beyond them.
	if (program.info.uses_dma) {
		constexpr uint64_t PageSize = BufferCache::CACHING_PAGESIZE;
		constexpr uint32_t Window   = 4; // pages from each pointer's page
		std::vector<uint64_t> pages;
		const auto add_range = [&](uint64_t address, uint64_t size) {
			for (uint64_t page = address & ~(PageSize - 1u); page < address + size && pages.size() < 8192;
			     page += PageSize) {
				pages.push_back(page);
			}
		};
		const auto scan = [&](const std::vector<uint32_t>& words) {
			for (size_t i = 0; i + 1 < words.size(); i++) {
				const uint64_t value = words[i] | (uint64_t {words[i + 1]} << 32u);
				if (value >= 0x10000u && value < (1ull << 48u)) {
					add_range(value, Window * PageSize);
				}
			}
		};
		scan(resources.user_data);
		scan(resources.flattened_srt);
		if (const char* extra = KYTY_GETENV("KYTY_DISPATCH_SNAPSHOT_PAGES"); extra != nullptr) {
			for (const char* p = extra; p != nullptr && *p != '\0';) {
				char*      end     = nullptr;
				const auto address = std::strtoull(p, &end, 16);
				uint64_t   size    = PageSize;
				if (*end == '+') size = std::strtoull(end + 1, &end, 16);
				add_range(address, size);
				p = *end == ',' ? end + 1 : nullptr;
			}
		}
		std::ranges::sort(pages);
		pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
		// Runs of pages contiguous in one cache buffer become one readback each.
		struct Run {
			uint64_t   address = 0;
			vk::Buffer buffer;
			uint64_t   offset = 0;
			uint64_t   size   = 0;
		};
		std::vector<Run> runs;
		auto&            buffers = context.GetBufferCache();
		uint32_t         unmapped = 0;
		for (const auto page: pages) {
			const auto [buffer, offset] = buffers.PeekPage(page);
			if (buffer == nullptr) {
				unmapped++;
				continue;
			}
			if (!runs.empty() && runs.back().buffer == buffer && runs.back().address + runs.back().size == page &&
			    runs.back().offset + runs.back().size == offset) {
				runs.back().size += PageSize;
			} else {
				runs.push_back({page, buffer, offset, PageSize});
			}
		}
		meta["bda_unmapped_pages"] = unmapped;
		for (const auto& run: runs) {
			meta["bda_pages"].push_back({{"address", run.address}});
		}
		for (size_t r = 0; r < runs.size(); r++) {
			collector.AddBuffer(runs[r].buffer, runs[r].offset, runs[r].size, meta["bda_pages"][r]);
		}
	}
	collector.Finish(s.directory / "blobs");
	meta["outputs"] = nullptr; // Filled in after the dispatch; null: the run ended before.
	WriteJson(folder / "meta.json", meta);
	std::printf("dispatch-snapshot: %s inputs written\n", name.c_str());
	std::fflush(stdout);
	OpenSnapshots()[pending.id] = std::move(meta);
	// Ring mode (hunting the dispatch before a device loss): only the newest snapshots stay; the
	// last one without outputs is where the GPU stopped. Blobs stay (shared, content-addressed).
	if (s.ring != 0) {
		static std::deque<std::filesystem::path> kept;
		kept.push_back(folder);
		while (kept.size() > s.ring) {
			std::filesystem::remove_all(kept.front(), error);
			kept.pop_front();
		}
	}
	return pending;
}

void CaptureOutputs(RenderContext& context, const Pending& pending, const ShaderComputeInputInfo& input,
                    const PreparedBindings& bindings) {
	auto open = OpenSnapshots().find(pending.id);
	if (open == OpenSnapshots().end()) {
		return;
	}
	auto&       meta    = open->second;
	const auto& program = *input.stage.program;
	Json        outputs;
	std::vector<int> region_of;
	const auto       regions = BufferRegions(bindings, program, region_of);
	outputs["regions"] = Json::array();
	outputs["images"]  = Json::array();
	for (size_t r = 0; r < regions.size(); r++) {
		if (regions[r].written) {
			outputs["regions"].push_back({{"id", r}});
		}
	}
	auto& cache = context.GetTextureCache();
	for (size_t i = 0; i < bindings.images.size(); i++) {
		const auto& texture = bindings.images[i];
		if (texture.desc.type != TextureCache::BindingType::Storage ||
		    i >= program.info.images.size() || !program.info.images[i].written) {
			continue;
		}
		const auto elements = texture.mip_views.empty() ? 1u : static_cast<uint32_t>(texture.mip_views.size());
		for (uint32_t e = 0; e < elements; e++) {
			Json entry;
			entry["index"]   = i;
			entry["element"] = e;
			outputs["images"].push_back(std::move(entry));
		}
	}
	Collector collector(context);
	size_t    region_entry = 0;
	for (size_t r = 0; r < regions.size(); r++) {
		if (regions[r].written) {
			collector.AddBuffer(regions[r].buffer, regions[r].begin, regions[r].end - regions[r].begin,
			                    outputs["regions"][region_entry++]);
		}
	}
	size_t image_entry = 0;
	for (size_t i = 0; i < bindings.images.size(); i++) {
		const auto& texture = bindings.images[i];
		if (texture.desc.type != TextureCache::BindingType::Storage ||
		    i >= program.info.images.size() || !program.info.images[i].written) {
			continue;
		}
		auto*      image    = (cache.PeekImage(texture.image_id) != nullptr ? &cache.GetImage(texture.image_id) : nullptr);
		const auto elements = texture.mip_views.empty() ? 1u : static_cast<uint32_t>(texture.mip_views.size());
		for (uint32_t e = 0; e < elements; e++) {
			auto&       entry = outputs["images"][image_entry++];
			const auto  view  = texture.mip_views.empty() ? texture.image_view : texture.mip_views[e];
			const auto* info  = image != nullptr ? FindViewInfo(*image, view) : nullptr;
			if (info == nullptr) {
				entry["skipped"] = "view not found";
				continue;
			}
			collector.AddImage(*image, *info, entry);
		}
	}
	collector.Finish(GetSettings().directory / "blobs");
	meta["outputs"] = std::move(outputs);
	WriteJson(std::filesystem::path(pending.directory) / "meta.json", meta);
	std::printf("dispatch-snapshot: %s complete\n",
	            std::filesystem::path(pending.directory).filename().string().c_str());
	std::fflush(stdout);
	OpenSnapshots().erase(open);
}

} // namespace Libs::Graphics::DispatchSnapshot
