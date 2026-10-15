#include "graphics/host_gpu/renderer/temporalDlss.h"

#include "common/switches.h"
#include "graphics/host_gpu/renderer/commandHooks.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/dlss/dlssRgbMerge.h"
#include "graphics/presentation/dlss/dlssTemporalContract.h"
#include "graphics/shader/shader.h"
#include <nlohmann/json.hpp>
#include <vulkan/vulkan_format_traits.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

#if defined(_WIN32)
namespace Libs::Graphics {
namespace {
using Json = nlohmann::json;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Keys(const Json& object, std::initializer_list<std::string_view> allowed) {
    Require(object.is_object(), "temporal metadata object required");
    for (const auto& [key, value] : object.items()) {
        bool known = false;
        for (const auto name : allowed) known |= key == name;
        Require(known, "unknown temporal metadata field (identity selectors are unsupported)");
    }
}
template <size_t N> std::array<uint32_t, N> UInts(const Json& array) {
    Require(array.is_array() && array.size() == N, "temporal metadata array size");
    std::array<uint32_t, N> result{};
    for (size_t i = 0; i < N; ++i) {
        Require(array[i].is_number_unsigned() || array[i].is_number_integer(), "integer required");
        const auto value = array[i].get<int64_t>();
        Require(value >= 0 && uint64_t(value) <= UINT32_MAX, "integer outside uint32 range");
        result[i] = static_cast<uint32_t>(value);
    }
    return result;
}
struct ImageGuard { vk::Format format; std::array<uint32_t, 2> extent; bool written; };
struct Pass {
    uint32_t buffers = 0, samplers = 0;
    std::array<uint32_t, 3> groups{}, threads{};
    std::vector<ImageGuard> images;
};
Pass ParsePass(const Json& json) {
    Keys(json, {"buffer_count", "sampler_count", "groups", "threads", "images"});
    Pass pass;
    pass.buffers = json.at("buffer_count").get<uint32_t>();
    pass.samplers = json.at("sampler_count").get<uint32_t>();
    pass.groups = UInts<3>(json.at("groups"));
    pass.threads = UInts<3>(json.at("threads"));
    Require(pass.buffers <= 64 && pass.samplers <= 32, "temporal resource count limit");
    Require(json.at("images").is_array() && !json.at("images").empty() &&
            json.at("images").size() <= 64, "temporal image count limit");
    for (const auto& image : json.at("images")) {
        Keys(image, {"format", "extent", "written"});
        pass.images.push_back({static_cast<vk::Format>(image.at("format").get<uint32_t>()),
                               UInts<2>(image.at("extent")), image.at("written").get<bool>()});
    }
    return pass;
}
struct Recipe {
    DlssProjectInfo project;
    DlssConfiguration configuration;
    Pass prep, accum;
    uint32_t depth = 0, motion = 0, prep_color = 0, accum_color = 0;
    std::array<uint32_t, 2> outputs{};
    uint32_t cb_slot = 0, cb_bytes = 0, serial_offset = 0, dt_offset = 0, max_inflight = 0;
    std::array<uint32_t, 2> jitter_offsets{};
    std::array<float, 2> jitter_scale{}, motion_scale{};
    float dt_scale_ms = 0, pre_exposure = 0, exposure_scale = 0;
};
const std::optional<Recipe>& Metadata() {
    // Immutable for this process: negotiation and dispatch use the SAME declaration.
    static const auto recipe = []() -> std::optional<Recipe> {
        const char* path = KYTY_GETENV("KYTY_DLSS_TEMPORAL_METADATA");
        if (path == nullptr || *path == '\0') return std::nullopt;
        try {
            std::ifstream file(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
            Require(file && file.tellg() > 0 && file.tellg() <= 65536, "metadata file outside 1..65536 bytes");
            std::string text(static_cast<size_t>(file.tellg()), '\0');
            file.seekg(0); file.read(text.data(), static_cast<std::streamsize>(text.size()));
            Require(bool(file), "metadata read failed");
            const auto json = Json::parse(text);
            Keys(json, {"version", "enabled", "declared_hdr_before_tonemap", "preserve_each_output_alpha",
                "history_feedback", "pre_exposure_assumption", "project", "render", "output", "quality", "preset", "depth_inverted",
                "motion_vectors_jittered", "auto_exposure", "pre_exposure", "exposure_scale", "motion_scale",
                "jitter_scale", "jitter_units", "motion_units", "frame_time_units", "dt_scale_ms",
                "max_inflight", "prep", "accum", "roles", "constants"});
            Require(json.at("version") == 1 && json.at("enabled") == true &&
                json.at("declared_hdr_before_tonemap") == true && json.at("preserve_each_output_alpha") == true &&
                json.at("history_feedback") == "replace_guest_rgb", "explicit HDR/alpha/history declaration required");
            Require(json.at("pre_exposure_assumption") == "explicit_experimental_declaration",
                    "pre-exposure uncertainty must be explicitly acknowledged in metadata");
            Require(json.at("jitter_units") == "render_pixels" &&
                json.at("motion_units") == "normalized_current_minus_previous" &&
                json.at("frame_time_units") == "seconds", "unsupported declared temporal units");
            Recipe r;
            const auto& project = json.at("project");
            Keys(project, {"project_id", "engine_version", "application_data_path", "feature_search_path"});
            r.project.project_id = project.at("project_id").get<std::string>();
            r.project.engine_version = project.at("engine_version").get<std::string>();
            r.project.application_data_path = std::filesystem::u8path(project.at("application_data_path").get<std::string>()).wstring();
            r.project.feature_search_path = std::filesystem::u8path(project.at("feature_search_path").get<std::string>()).wstring();
            Require(!r.project.project_id.empty() && !r.project.engine_version.empty() &&
                    !r.project.application_data_path.empty() && !r.project.feature_search_path.empty(), "explicit NGX project paths required");
            const auto render = UInts<2>(json.at("render")), output = UInts<2>(json.at("output"));
            Require(render[0] && render[1] && output[0] >= render[0] && output[1] >= render[1] &&
                    output[0] <= 16384 && output[1] <= 16384, "unsupported temporal extent");
            r.configuration.render = {render[0], render[1]}; r.configuration.output = {output[0], output[1]};
            const auto quality = json.at("quality").get<std::string>();
            if (quality == "performance") r.configuration.quality = DlssQuality::Performance;
            else if (quality == "quality") r.configuration.quality = DlssQuality::Quality;
            else if (quality == "balanced") r.configuration.quality = DlssQuality::Balanced;
            else if (quality == "dlaa") r.configuration.quality = DlssQuality::Dlaa;
            else throw std::runtime_error("unsupported temporal quality");
            const auto preset = json.at("preset").get<std::string>();
            if (preset == "transformer_m") r.configuration.preset = DlssPreset::TransformerM;
            else if (preset == "default") r.configuration.preset = DlssPreset::Default;
            else throw std::runtime_error("unsupported temporal preset");
            r.configuration.hdr = true; r.configuration.motion_vectors_at_render_resolution = true;
            r.configuration.depth_inverted = json.at("depth_inverted").get<bool>();
            r.configuration.motion_vectors_jittered = json.at("motion_vectors_jittered").get<bool>();
            r.configuration.auto_exposure = json.at("auto_exposure").get<bool>();
            Require(r.configuration.auto_exposure, "this explicit provider requires NGX auto exposure");
            r.pre_exposure = json.at("pre_exposure").get<float>(); r.exposure_scale = json.at("exposure_scale").get<float>();
            r.dt_scale_ms = json.at("dt_scale_ms").get<float>();
            r.jitter_scale = json.at("jitter_scale").get<std::array<float, 2>>();
            r.motion_scale = json.at("motion_scale").get<std::array<float, 2>>();
            for (float v : {r.pre_exposure, r.exposure_scale, r.dt_scale_ms}) Require(std::isfinite(v) && v > 0, "invalid temporal scalar");
            for (float v : r.jitter_scale) Require(std::isfinite(v) && v != 0, "invalid jitter scale");
            for (float v : r.motion_scale) Require(std::isfinite(v) && v != 0, "invalid motion scale");
            r.max_inflight = json.at("max_inflight").get<uint32_t>();
            Require(r.max_inflight && r.max_inflight <= 4, "private frame lease bound outside 1..4");
            r.prep = ParsePass(json.at("prep")); r.accum = ParsePass(json.at("accum"));
            const auto& roles = json.at("roles");
            Keys(roles, {"prep_raw_depth", "prep_raw_motion", "prep_color", "accum_color", "accum_outputs"});
            r.depth = roles.at("prep_raw_depth").get<uint32_t>(); r.motion = roles.at("prep_raw_motion").get<uint32_t>();
            r.prep_color = roles.at("prep_color").get<uint32_t>(); r.accum_color = roles.at("accum_color").get<uint32_t>();
            r.outputs = UInts<2>(roles.at("accum_outputs"));
            Require(r.depth < r.prep.images.size() && r.motion < r.prep.images.size() && r.prep_color < r.prep.images.size() &&
                    r.accum_color < r.accum.images.size() && r.outputs[0] < r.accum.images.size() &&
                    r.outputs[1] < r.accum.images.size() && r.outputs[0] != r.outputs[1], "role index outside declared pass");
            const auto& cb = json.at("constants");
            Keys(cb, {"buffer_slot", "bytes", "jitter_offsets", "serial_offset", "serial_type", "dt_offset"});
            Require(cb.at("serial_type") == "f32_integer", "unsupported temporal serial encoding");
            r.cb_slot = cb.at("buffer_slot").get<uint32_t>(); r.cb_bytes = cb.at("bytes").get<uint32_t>();
            r.jitter_offsets = UInts<2>(cb.at("jitter_offsets")); r.serial_offset = cb.at("serial_offset").get<uint32_t>();
            r.dt_offset = cb.at("dt_offset").get<uint32_t>();
            Require(r.cb_bytes >= 4 && r.cb_bytes <= 1024 && r.cb_slot < r.prep.buffers && r.cb_slot < r.accum.buffers,
                    "constant selection outside bounds");
            for (uint32_t offset : {r.jitter_offsets[0], r.jitter_offsets[1], r.serial_offset, r.dt_offset})
                Require(offset % 4 == 0 && offset <= r.cb_bytes - 4, "constant offset outside declared range");
            return r;
        } catch (const std::exception& error) {
            std::printf("temporal-dlss: metadata rejected: %s\n", error.what()); return std::nullopt;
        }
    }();
    return recipe;
}
struct Counters { std::atomic<uint64_t> inflight{0}, evaluations{0}, merges{0}, completed{0}; };
uint64_t Handle(vk::Image image) { return reinterpret_cast<uint64_t>(static_cast<VkImage>(image)); }
bool NativeDepth(vk::Format f) { return f == vk::Format::eD32Sfloat || f == vk::Format::eD32SfloatS8Uint; }
bool IdentityMapping(const vk::ComponentMapping& mapping) {
    return (mapping.r == vk::ComponentSwizzle::eIdentity || mapping.r == vk::ComponentSwizzle::eR) &&
           (mapping.g == vk::ComponentSwizzle::eIdentity || mapping.g == vk::ComponentSwizzle::eG) &&
           (mapping.b == vk::ComponentSwizzle::eIdentity || mapping.b == vk::ComponentSwizzle::eB) &&
           (mapping.a == vk::ComponentSwizzle::eIdentity || mapping.a == vk::ComponentSwizzle::eA);
}
vk::ImageAspectFlags Aspects(vk::Format f) {
    return f == vk::Format::eD32SfloatS8Uint ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil :
           NativeDepth(f) ? vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth) : vk::ImageAspectFlags(vk::ImageAspectFlagBits::eColor);
}
struct PrivateImage {
    GraphicContext& graphics;
    VulkanImage backing;
    vk::ImageView view{};
    PrivateImage(GraphicContext& g, vk::Format format, DlssExtent extent, bool output) : graphics(g) {
        vk::ImageCreateInfo info{};
        info.imageType = vk::ImageType::e2D; info.format = format; info.extent = vk::Extent3D{extent.width, extent.height, 1};
        info.mipLevels = info.arrayLayers = 1; info.samples = vk::SampleCountFlagBits::e1;
        info.tiling = vk::ImageTiling::eOptimal; info.sharingMode = vk::SharingMode::eExclusive;
        info.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
        if (output) info.usage |= vk::ImageUsageFlagBits::eStorage;
        Require(graphics.CreateImage(info, backing), "private temporal image allocation failed");
        vk::ImageViewCreateInfo vi{}; vi.image = backing.image; vi.viewType = vk::ImageViewType::e2D; vi.format = format;
        vi.subresourceRange = {NativeDepth(format) ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        if (graphics.device.createImageView(&vi, nullptr, &view) != vk::Result::eSuccess) {
            graphics.DeleteImage(backing); backing.image = nullptr;
            throw std::runtime_error("private temporal view creation failed");
        }
    }
    ~PrivateImage() { if (view) graphics.device.destroyImageView(view); if (backing.image) graphics.DeleteImage(backing); }
    void Transit(vk::CommandBuffer command, vk::ImageLayout layout, vk::AccessFlags2 access) {
        vk::ImageMemoryBarrier2 barrier{};
        barrier.srcStageMask = backing.state.pl_stage; barrier.srcAccessMask = backing.state.access_mask;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands; barrier.dstAccessMask = access;
        barrier.oldLayout = backing.state.layout; barrier.newLayout = layout; barrier.image = backing.image;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange = {Aspects(backing.format), 0, 1, 0, 1};
        vk::DependencyInfo dependency{}; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &barrier;
        command.pipelineBarrier2(dependency);
        backing.state = {vk::PipelineStageFlagBits2::eAllCommands, access, layout};
    }
    DlssImage Describe() const {
        DlssImage image{}; image.image = static_cast<VkImage>(backing.image); image.view = static_cast<VkImageView>(view);
        image.format = static_cast<VkFormat>(backing.format); image.extent = {backing.extent.width, backing.extent.height};
        image.subresource = {static_cast<VkImageAspectFlags>(NativeDepth(backing.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
        image.layout = static_cast<VkImageLayout>(backing.state.layout); image.usage = static_cast<VkImageUsageFlags>(backing.usage);
        return image;
    }
};
struct Constants { std::array<float, 2> jitter{}; float dt_ms = 0; uint64_t serial = 0; };
struct Tuple {
    std::shared_ptr<Counters> counters;
    std::array<std::unique_ptr<PrivateImage>, 4> images; // color, raw depth, raw motion, private NGX output.
    Constants constants;
    ImageId color_id;
    uint64_t color_generation = 0, sequence = 0;
    uint32_t guest_frame = 0;
    explicit Tuple(std::shared_ptr<Counters> c) : counters(std::move(c)) { ++counters->inflight; }
    ~Tuple() { --counters->inflight; }
};
} // namespace

struct TemporalDlss::State {
    RenderContext& context;
    std::shared_ptr<DlssVulkan> backend;
    std::shared_ptr<Counters> counters = std::make_shared<Counters>();
    DlssRgbMerge merge;
    const Recipe* recipe = nullptr;
    std::shared_ptr<Tuple> pending, active;
    std::array<ImageId, 2> output_ids{};
    uint64_t sequence = 0, active_sequence = 0;
    std::optional<uint64_t> last_serial;
    std::optional<uint32_t> prep_frame;
    bool disabled = false, merge_ready = false, reset_required = true;
    uint32_t refusals = 0, constant_reads = 0;
    State(RenderContext& c, std::shared_ptr<DlssVulkan> b) : context(c), backend(std::move(b)) {
        if (Metadata() && backend && backend->Initialized() && !backend->FeatureCreated()) recipe = &*Metadata();
    }
    void Refuse(const char* reason) {
        reset_required = true;
        if (refusals++ < 12) std::printf("temporal-dlss: refused %s\n", reason);
    }
    bool Match(const Pass& pass, const ShaderComputeInputInfo& input, const PreparedBindings& bindings,
               const std::array<uint32_t, 3>& groups) {
        if (!input.stage.program || !input.stage.resources || bindings.runtime != &input.stage || groups != pass.groups || bindings.images.size() != pass.images.size() ||
            input.stage.program->info.images.size() != pass.images.size() || input.stage.program->info.buffers.size() != pass.buffers ||
            input.stage.program->info.samplers.size() != pass.samplers) return false;
        for (size_t i = 0; i < 3; ++i) if (input.threads_num[i] != pass.threads[i]) return false;
        auto& cache = context.GetTextureCache();
        for (size_t i = 0; i < pass.images.size(); ++i) {
            const auto* image = cache.PeekImage(bindings.images[i].image_id);
            const auto& guard = pass.images[i];
            if (!image || image->backing.format != guard.format || image->backing.extent.width != guard.extent[0] ||
                image->backing.extent.height != guard.extent[1] || image->backing.extent.depth != 1 ||
                image->backing.samples != 1 || input.stage.program->info.images[i].written != guard.written ||
                (bindings.images[i].desc.type == TextureCache::BindingType::Storage) != guard.written) return false;
        }
        return true;
    }
    Constants ReadConstants(const ShaderComputeInputInfo& input, const PreparedBindings& bindings) {
        const auto& r = *recipe;
        Require(r.cb_slot < bindings.buffer_sources.size() && !input.stage.program->info.buffers[r.cb_slot].written,
                "constant buffer must be declared read-only");
        const auto& source = bindings.buffer_sources[r.cb_slot];
        Require(source.address && source.size == r.cb_bytes, "constant buffer extent mismatch");
        const auto& layout = input.stage.program->bindings;
        Require(!layout.table && r.cb_slot < bindings.buffers.size() && r.cb_slot < layout.memory_offset_count,
                "unsupported constants: actual descriptor/packed adjustment unavailable");
        const auto dword = uint64_t(layout.memory_offset_dword) + r.cb_slot / 4u;
        Require(dword < bindings.shader_data.size(), "constant adjustment outside prepared shader data");
        const auto adjustment = (bindings.shader_data[static_cast<size_t>(dword)] >> ((r.cb_slot % 4u) * 8u)) & 0xffu;
        const auto& view = bindings.buffers[r.cb_slot];
        auto& stream = context.GetBufferCache().ActiveStream();
        Require(view.buffer && view.buffer == stream.Handle() && stream.Usage() == MemoryUsage::Stream &&
                (stream.MemoryProperties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0,
                "unsupported constants: bound buffer is not the immutable active host stream");
        Require(view.range != VK_WHOLE_SIZE && adjustment <= view.range && r.cb_bytes <= view.range - adjustment,
                "constant bytes outside actual descriptor range");
        const auto mapped = stream.Mapped();
        Require(view.offset <= mapped.size() && adjustment <= mapped.size() - view.offset,
                "constant start outside mapped stream");
        const auto start = view.offset + adjustment;
        Require(r.cb_bytes <= mapped.size() - start, "constant bytes outside mapped stream");
        std::array<uint8_t, 1024> bytes{};
        // Read exactly the host-written ring slice the guest shader is bound to.
        // Its reservation remains immutable until scheduler retirement; never read
        // current guest backing or arbitrary mapped device-local storage here.
        std::memcpy(bytes.data(), mapped.data() + static_cast<size_t>(start), r.cb_bytes);
        const auto read = [&](uint32_t offset) { float value; std::memcpy(&value, bytes.data() + offset, 4); return value; };
        Constants result;
        for (size_t i = 0; i < 2; ++i) { result.jitter[i] = read(r.jitter_offsets[i]) * r.jitter_scale[i]; Require(std::isfinite(result.jitter[i]), "non-finite jitter"); }
        result.dt_ms = read(r.dt_offset) * r.dt_scale_ms;
        const float serial = read(r.serial_offset);
        Require(std::isfinite(serial) && serial >= 0 && serial <= 16777216.0f && std::trunc(serial) == serial &&
                std::isfinite(result.dt_ms) && result.dt_ms > 0, "invalid temporal serial/frame time");
        result.serial = static_cast<uint64_t>(serial);
        if (constant_reads++ < 4) std::printf("temporal-dlss: constants=bound-immutable-host-stream bytes=%u adjustment=%u serial=%llu\n",
            r.cb_bytes, adjustment, static_cast<unsigned long long>(result.serial));
        return result;
    }
    Image& Role(const PreparedBindings& bindings, uint32_t index, DlssExtent extent, bool written) {
        auto& image = context.GetTextureCache().GetImage(bindings.images[index].image_id);
        const auto& view = bindings.images[index].desc.view_info;
        Require(image.backing.image && image.backing.mip_levels == 1 && image.backing.layers == 1 &&
                view.base_level == 0 && view.base_layer == 0 && view.level_count == 1 && view.layer_count == 1 &&
                view.min_lod == 0 && IdentityMapping(view.mapping) && view.format == image.backing.format &&
                image.backing.extent == vk::Extent3D{extent.width, extent.height, 1} &&
                view.aspect == (NativeDepth(image.backing.format) ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor) &&
                bindings.images[index].image_view, "role must be an exact identity full image");
        Require(written ? bool(image.backing.usage & vk::ImageUsageFlagBits::eStorage) :
                          bool(image.backing.usage & vk::ImageUsageFlagBits::eTransferSrc), "role usage unsupported");
        return image;
    }
    void Snapshot(const ShaderComputeInputInfo& input, const PreparedBindings& bindings, uint32_t frame) {
        const auto& r = *recipe;
        Require(!prep_frame || *prep_frame != frame, "ambiguous repeated prep in guest frame");
        prep_frame = frame; pending.reset();
        Require(counters->inflight.load() < r.max_inflight, "private frame lease bound reached");
        auto tuple = std::make_shared<Tuple>(counters);
        tuple->constants = ReadConstants(input, bindings); tuple->guest_frame = frame; tuple->sequence = sequence;
        tuple->color_id = bindings.images[r.prep_color].image_id;
        tuple->color_generation = context.GetTextureCache().ImageGeneration(tuple->color_id);
        const std::array<uint32_t, 3> roles{r.prep_color, r.depth, r.motion};
        std::array<Image*, 3> sources{};
        uint64_t bytes = uint64_t(r.configuration.output.width) * r.configuration.output.height * 8;
        for (size_t i = 0; i < 3; ++i) {
            sources[i] = &Role(bindings, roles[i], r.configuration.render, false);
            const auto format = sources[i]->backing.format;
            Require(i == 0 ? format == vk::Format::eR16G16B16A16Sfloat :
                    i == 1 ? (NativeDepth(format) || format == vk::Format::eR32Sfloat) :
                             (format == vk::Format::eR16G16Sfloat || format == vk::Format::eR32G32Sfloat), "unsupported private role format");
            bytes += uint64_t(r.configuration.render.width) * r.configuration.render.height * vk::blockSize(format);
        }
        Require(bytes <= 512ull * 1024 * 1024, "private temporal tuple exceeds 512MiB");
        for (size_t i = 0; i < 3; ++i) tuple->images[i] = std::make_unique<PrivateImage>(context.GetGraphics(), sources[i]->backing.format, r.configuration.render, false);
        tuple->images[3] = std::make_unique<PrivateImage>(context.GetGraphics(), vk::Format::eR16G16B16A16Sfloat, r.configuration.output, true);
        auto& scheduler = context.GetCommandScheduler(); auto& buffer = scheduler.Current(); buffer.EndRendering();
        auto command = buffer.Handle();
        // Register BEFORE copying, including a later partial-recording failure.
        scheduler.DeferOperation([tuple] {});
        for (size_t i = 0; i < 3; ++i) {
            auto& source = *sources[i]; const auto old = source.backing.state;
            Require(old.layout != vk::ImageLayout::eUndefined, "private copy source contents undefined");
            source.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, command);
            tuple->images[i]->Transit(command, vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite);
            vk::ImageCopy copy{};
            copy.srcSubresource = {NativeDepth(source.backing.format) ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copy.dstSubresource = copy.srcSubresource; copy.extent = source.backing.extent;
            command.copyImage(source.backing.image, vk::ImageLayout::eTransferSrcOptimal,
                tuple->images[i]->backing.image, vk::ImageLayout::eTransferDstOptimal, 1, &copy);
            source.Transit(old.layout, old.access_mask, {}, command);
            tuple->images[i]->Transit(command, vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead);
        }
        scheduler.NoteCommandRecorded(); pending = std::move(tuple);
    }
    DlssTemporalAssociation Association(const PreparedBindings& bindings, uint32_t frame) {
        auto& r = *recipe; auto& cache = context.GetTextureCache();
        DlssTemporalAssociation a;
        a.declared_hdr_before_tonemap = a.declared_preserve_each_output_alpha = a.immutable_input_copies = true;
        a.producer_guest_frame = active->guest_frame; a.consumer_guest_frame = frame;
        a.producer_sequence = active->sequence; a.consumer_sequence = active_sequence;
        a.producer_color = Handle(cache.GetImage(active->color_id).backing.image);
        a.consumer_color = Handle(cache.GetImage(bindings.images[r.accum_color].image_id).backing.image);
        a.producer_color_generation = active->color_generation;
        a.consumer_color_generation = cache.ImageGeneration(bindings.images[r.accum_color].image_id);
        a.temporal_serial = active->constants.serial;
        a.previous_temporal_serial = last_serial;
        if (!last_serial || *last_serial + 1 != a.temporal_serial) reset_required = true;
        for (size_t i = 0; i < 3; ++i) a.private_inputs[i] = Handle(active->images[i]->backing.image);
        for (size_t i = 0; i < 2; ++i) a.outputs[i] = Handle(cache.GetImage(output_ids[i]).backing.image);
        return a;
    }
};

std::optional<DlssProjectInfo> TemporalDlss::ProjectFromEnvironment() {
    return Metadata() ? std::optional<DlssProjectInfo>(Metadata()->project) : std::nullopt;
}
TemporalDlss::TemporalDlss(RenderContext& context, std::shared_ptr<DlssVulkan> backend)
    : m_state(std::make_unique<State>(context, std::move(backend))) {}
TemporalDlss::~TemporalDlss() {
    auto& s = *m_state;
    std::printf("temporal-dlss: evaluations=%llu merges=%llu completed=%llu active=%d\n",
        static_cast<unsigned long long>(s.counters->evaluations.load()), static_cast<unsigned long long>(s.counters->merges.load()),
        static_cast<unsigned long long>(s.counters->completed.load()), s.recipe && !s.disabled ? 1 : 0);
    s.active.reset(); s.pending.reset();
    (void)s.merge.DestroyAfterGpuIdle();
    if (s.recipe && s.backend) { (void)s.backend->DestroyFeatureAfterGpuIdle(); (void)s.backend->ShutdownAfterGpuIdle(); }
}
bool TemporalDlss::BeforeDispatch(const ShaderComputeInputInfo& input, const PreparedBindings& bindings,
                                std::array<uint32_t, 3> groups, uint32_t frame) {
    auto& s = *m_state;
    if (!s.recipe || s.disabled) return false;
    ++s.sequence; s.active.reset();
    if (s.pending && s.pending->guest_frame != frame) { s.pending.reset(); s.reset_required = true; }
    try {
        if (s.Match(s.recipe->prep, input, bindings, groups)) {
            s.Snapshot(input, bindings, frame); return true;
        }
        if (!s.Match(s.recipe->accum, input, bindings, groups)) return false;
        Require(s.pending && s.pending->guest_frame == frame, "no unique same-frame private prep tuple");
        const auto constants = s.ReadConstants(input, bindings);
        Require(constants.serial == s.pending->constants.serial && constants.jitter == s.pending->constants.jitter &&
                constants.dt_ms == s.pending->constants.dt_ms, "producer/consumer constants differ");
        auto& cache = s.context.GetTextureCache();
        Require(bindings.images[s.recipe->accum_color].image_id == s.pending->color_id &&
                cache.PeekImage(s.pending->color_id) && cache.ImageGeneration(s.pending->color_id) == s.pending->color_generation,
                "producer/consumer color identity or generation differs");
        for (size_t i = 0; i < 2; ++i) {
            s.output_ids[i] = bindings.images[s.recipe->outputs[i]].image_id;
            auto& output = s.Role(bindings, s.recipe->outputs[i], s.recipe->configuration.output, true);
            Require(output.backing.format == vk::Format::eR16G16B16A16Sfloat, "RGB merge requires RGBA16F outputs");
        }
        s.active = std::move(s.pending); s.active_sequence = s.sequence;
        const auto status = ValidateDlssTemporalAssociation(s.Association(bindings, frame));
        Require(bool(status), status.message); return true;
    } catch (const std::exception& error) { s.active.reset(); s.pending.reset(); s.Refuse(error.what()); return true; }
}
void TemporalDlss::AfterDispatch(const ShaderComputeInputInfo& input, const PreparedBindings& bindings,
                               std::array<uint32_t, 3> groups, uint32_t frame) {
    auto& s = *m_state;
    if (!s.active || !s.recipe || s.disabled) return;
    auto tuple = std::move(s.active);
    s.active = tuple;
    try {
        Require(s.Match(s.recipe->accum, input, bindings, groups) && s.sequence == s.active_sequence,
                "after-dispatch association changed");
        const auto association = ValidateDlssTemporalAssociation(s.Association(bindings, frame));
        Require(bool(association), association.message);
        auto& scheduler = s.context.GetCommandScheduler(); auto& buffer = scheduler.Current();
        auto& graphics = s.context.GetGraphics(); buffer.EndRendering(); auto command = buffer.Handle();
        if (!s.merge_ready) {
            const auto status = s.merge.Init(static_cast<VkDevice>(graphics.device), VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
            Require(bool(status), status.message); s.merge_ready = true;
        }
        tuple->images[3]->Transit(command, vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eMemoryWrite);
        DrainGpuThreadCommands();
        scheduler.DeferOperation([tuple, backend = s.backend] {});
        scheduler.NoteCommandRecorded();
        if (!s.backend->FeatureCreated()) {
            const auto status = s.backend->RecordCreate(static_cast<VkCommandBuffer>(command), s.recipe->configuration);
            buffer.InvalidateGraphicsState(); Require(bool(status), status.message);
        }
        DlssFrameInputs inputs;
        inputs.color = tuple->images[0]->Describe(); inputs.depth = tuple->images[1]->Describe();
        inputs.motion_vectors = tuple->images[2]->Describe(); inputs.output = tuple->images[3]->Describe();
        inputs.render = s.recipe->configuration.render;
        inputs.jitter_x = tuple->constants.jitter[0]; inputs.jitter_y = tuple->constants.jitter[1];
        inputs.motion_scale_x = s.recipe->motion_scale[0]; inputs.motion_scale_y = s.recipe->motion_scale[1];
        inputs.pre_exposure = s.recipe->pre_exposure; inputs.exposure_scale = s.recipe->exposure_scale;
        inputs.frame_time_ms = tuple->constants.dt_ms; inputs.reset_history = s.reset_required;
        ++s.counters->evaluations;
        const auto evaluated = s.backend->RecordEvaluate(static_cast<VkCommandBuffer>(command), inputs);
        buffer.InvalidateGraphicsState(); Require(bool(evaluated), evaluated.message);
        tuple->images[3]->Transit(command, vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead);
        std::array<vk::ImageView, 2> views{};
        auto& cache = s.context.GetTextureCache();
        for (size_t i = 0; i < 2; ++i) {
            auto& output = cache.GetImage(s.output_ids[i]);
            output.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite, {}, command);
            views[i] = bindings.images[s.recipe->outputs[i]].image_view;
        }
        // Record uses native entrypoints too: flush newly queued image dependencies first.
        DrainGpuThreadCommands();
        std::shared_ptr<void> merge_lease;
        const auto merged = s.merge.Record(static_cast<VkCommandBuffer>(command), static_cast<VkImageView>(tuple->images[3]->view),
            static_cast<VkImageView>(views[0]), static_cast<VkImageView>(views[1]), s.recipe->configuration.output, &merge_lease);
        scheduler.DeferOperation([merge_lease] {}); // Also retain a partially recorded failed call.
        buffer.InvalidateGraphicsState(); Require(bool(merged), merged.message);
        ShaderAccessBarrier(command, vk::PipelineStageFlagBits::eComputeShader);
        for (auto id : s.output_ids) cache.MarkGpuWritten(id);
        scheduler.NoteCommandRecorded(); ++s.counters->merges;
        const auto serial = tuple->constants.serial;
        if (s.counters->merges.load() <= 8) std::printf("temporal-dlss: native evaluate+RGB merge recorded frame=%u serial=%llu reset=%d\n",
            frame, static_cast<unsigned long long>(serial), inputs.reset_history ? 1 : 0);
        scheduler.DeferOperation([tuple, counters = s.counters, frame, serial] {
            const auto completed = ++counters->completed;
            if (completed <= 8) std::printf("temporal-dlss: completed=%llu frame=%u serial=%llu\n",
                static_cast<unsigned long long>(completed), frame, static_cast<unsigned long long>(serial));
        });
        s.last_serial = serial; s.reset_required = false;
    } catch (const std::exception& error) {
        s.disabled = true; s.Refuse(error.what());
    }
    s.active.reset();
}
} // namespace Libs::Graphics
#else
namespace Libs::Graphics {
struct TemporalDlss::State {};
TemporalDlss::TemporalDlss(RenderContext&, std::shared_ptr<DlssVulkan>) : m_state(std::make_unique<State>()) {}
TemporalDlss::~TemporalDlss() = default;
std::optional<DlssProjectInfo> TemporalDlss::ProjectFromEnvironment() { return std::nullopt; }
bool TemporalDlss::BeforeDispatch(const ShaderComputeInputInfo&, const PreparedBindings&,
                                std::array<uint32_t, 3>, uint32_t) { return false; }
void TemporalDlss::AfterDispatch(const ShaderComputeInputInfo&, const PreparedBindings&,
                               std::array<uint32_t, 3>, uint32_t) {}
} // namespace Libs::Graphics
#endif
