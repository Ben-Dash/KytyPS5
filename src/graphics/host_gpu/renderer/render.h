#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/bindlessTable.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

namespace HW {
class Context;
class UserConfig;
class Shader;
} // namespace HW

struct GraphicContext;
struct ShaderBufferResource;
struct ShaderComputeInputInfo;
struct RenderDepthInfo;
struct RenderColorInfo;
struct DrawCallInfo;
class MeshIndirectArgs;
struct DrawEmitInfo;
struct DrawIndexBufferSource;
struct DrawRenderState;
class RenderContext;
class CommandScheduler;
struct RenderExecutorTestAccess;

// KYTY_DEBUG_IMAGE_USERS=<guest address> (see renderDraw.cpp): whether it is set, and a draw's or
// dispatch's bindings to count (pixel_hash 0 and no targets for a dispatch).
[[nodiscard]] bool ImageUsersEnabled();
void NoteImageUsers(TextureCache& cache, std::span<PreparedBindings* const> stages,
                    std::span<const RenderColorInfo> colors, const RenderDepthInfo* depth,
                    uint64_t pixel_hash, uint64_t vertex_hash);

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	EopWrite,
	EopInterrupt,
	EopWriteBack,
	EopFlip,
	EopWriteBackFlip,
	EopOnlyFlip,
	DispatchIndirect,
	Unknown,
};

enum class DrawOffsetSource : uint8_t {
	DrawState,
	IndirectArgs,
};

struct DrawIndexArgs {
	uint32_t         index_count                = 0;
	const void*      index_addr                 = nullptr;
	uint32_t         instance_count             = 0;
	uint32_t         index_type_and_size        = 0;
	int32_t          base_vertex                = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
	// GPU-written DrawIndexedIndirectArgs for a mesh-emulated draw: the counts above are
	// placeholders, index_addr is the index buffer base, and index_limit its size in elements.
	uint64_t         indirect_args              = 0;
	uint32_t         index_limit                = 0;
};

struct DrawAutoArgs {
	uint32_t         vertex_count               = 0;
	uint32_t         instance_count             = 0;
	uint32_t         first_vertex               = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawState;
	uint32_t         render_target_slice_offset = 0;
	// GPU-written DrawIndirectArgs (VkDrawIndirectCommand layout) for a draw with a natively
	// supported primitive type: the counts above are placeholders.
	uint64_t         indirect_args              = 0;
};

struct SubmitInfo {
	static constexpr uint32_t MaxSemaphores = 3;

	std::array<vk::Semaphore, MaxSemaphores>          wait_semaphores {};
	std::array<uint64_t, MaxSemaphores>               wait_ticks {};
	std::array<vk::PipelineStageFlags, MaxSemaphores> wait_stages {};
	std::array<vk::Semaphore, MaxSemaphores>          signal_semaphores {};
	std::array<uint64_t, MaxSemaphores>               signal_ticks {};
	uint32_t                                          num_wait_semaphores   = 0;
	uint32_t                                          num_signal_semaphores = 0;

	void AddWait(vk::Semaphore semaphore, uint64_t tick = 1,
	             vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands) {
		EXIT_IF(semaphore == nullptr || num_wait_semaphores >= MaxSemaphores);
		wait_semaphores[num_wait_semaphores] = semaphore;
		wait_ticks[num_wait_semaphores]      = tick;
		wait_stages[num_wait_semaphores++]   = stage;
	}

	void AddSignal(vk::Semaphore semaphore, uint64_t tick = 1) {
		EXIT_IF(semaphore == nullptr || num_signal_semaphores >= MaxSemaphores);
		signal_semaphores[num_signal_semaphores] = semaphore;
		signal_ticks[num_signal_semaphores++]    = tick;
	}
};

class CommandBuffer {
public:
	~CommandBuffer() = default;

	KYTY_CLASS_NO_COPY(CommandBuffer);

	[[nodiscard]] bool IsInvalid() const;

	void SetDebugInfo(uint32_t op, uint64_t submit_id, uint32_t arg0 = 0, uint32_t arg1 = 0,
	                  uint32_t arg2 = 0, uint32_t arg3 = 0, uint64_t arg4 = 0);
	void BeginRendering(const RenderState& state) const;
	// Ends the rendering instance, then records the barrier of any deferred shader writes.
	void EndRendering() const;
	[[nodiscard]] bool IsRendering() const noexcept { return m_rendering; }
	// A draw whose shaders wrote buffers leaves the barrier that makes the writes visible pending
	// while rendering continues. Every command other than a draw ends rendering before it records,
	// which records the barrier; a later draw that may read the writes ends rendering first.
	void DeferShaderWriteBarrier(vk::PipelineStageFlags source_stages) const {
		m_pending_shader_writes |= source_stages;
	}
	[[nodiscard]] bool HasPendingShaderWrites() const noexcept {
		return static_cast<bool>(m_pending_shader_writes);
	}

	// The graphics pipeline and dynamic state draws last recorded into this command buffer, so
	// that a draw skips commands that would set the same values again. Every graphics pipeline a
	// draw binds keeps this state dynamic, except color write enables without color attachments.
	// Anything else that binds a graphics pipeline or sets dynamic state here must call
	// InvalidateGraphicsState.
	struct GraphicsState {
		static constexpr uint32_t ViewportSlots = 16;

		vk::Pipeline                                pipeline;
		uint32_t                                    viewport_count = 0; // 0: unknown.
		std::array<vk::Viewport, ViewportSlots>     viewports {};
		uint32_t                                    scissor_count = 0; // 0: unknown.
		std::array<vk::Rect2D, ViewportSlots>       scissors {};
		bool                                        fixed_valid   = false;
		float                                       line_width    = 1.0f;
		std::array<float, 4>                        blend_constants {};
		vk::Bool32                                  depth_test    = VK_FALSE;
		vk::Bool32                                  depth_write   = VK_FALSE;
		vk::CompareOp                               depth_compare = vk::CompareOp::eNever;
		vk::Bool32                                  depth_bias    = VK_FALSE;
		vk::Bool32                                  stencil_test  = VK_FALSE;
		bool                                        bias_valid    = false;
		std::array<float, 3>                        bias {};
		bool                                        stencil_valid = false;
		std::array<vk::StencilOpState, 2>           stencil {};
		uint32_t                                    color_write_count = 0; // 0: unknown.
		std::array<vk::Bool32, RENDER_COLOR_ATTACHMENTS_MAX> color_write {};
		bool                                        feedback_valid = false;
		vk::ImageAspectFlags                        feedback;
	};
	[[nodiscard]] GraphicsState& GetGraphicsState() const noexcept { return m_graphics_state; }
	void InvalidateGraphicsState() const noexcept { m_graphics_state = {}; }

	[[nodiscard]] vk::CommandBuffer Handle() const;
	// KYTY_COMPUTE_CHAIN=1: a dispatch leaves the barrier that orders it before later work pending
	// instead of recording it, so consecutive dispatches the guest did not separate by a cache
	// flush, partial flush, wait or end-of-pipe write run without host barriers between them, as
	// on the PS5. Every other command (Handle) records the pending barrier first.
	[[nodiscard]] vk::CommandBuffer ChainHandle() const;
	[[nodiscard]] bool ComputeChainPending() const noexcept { return m_compute_access_pending; }
	void ContinueComputeChain() const noexcept { m_compute_access_pending = true; }
	// For a caller that records a full memory dependency at once, which covers the pending one.
	[[nodiscard]] vk::CommandBuffer HandleForFullBarrier() const;
	[[nodiscard]] GraphicContext&   GetGraphics() const noexcept { return m_graphics; }
	[[nodiscard]] RenderContext&    GetContext() const noexcept { return m_context; }
	[[nodiscard]] HW::Context&      GetRegisters() const noexcept { return *m_registers; }
	[[nodiscard]] HW::UserConfig&   GetUserConfig() const noexcept { return *m_user_config; }
	[[nodiscard]] HW::Shader&       GetShaders() const noexcept { return *m_shaders; }

private:
	explicit CommandBuffer(CommandScheduler& scheduler);
	void Bind(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders) noexcept {
		m_registers   = &registers;
		m_user_config = &user_config;
		m_shaders     = &shaders;
	}

	void Begin();
	void End() const;

	RenderContext&      m_context;
	GraphicContext&     m_graphics;
	vk::CommandBuffer   m_buffer          = nullptr;
	uint32_t            m_debug_op        = 0;
	uint64_t            m_debug_submit_id = 0;
	uint32_t            m_debug_arg0      = 0;
	uint32_t            m_debug_arg1      = 0;
	uint32_t            m_debug_arg2      = 0;
	uint32_t            m_debug_arg3      = 0;
	uint64_t            m_debug_arg4      = 0;
	mutable RenderState m_render_state;
	mutable bool        m_rendering   = false;
	mutable bool        m_compute_access_pending = false;
	mutable vk::PipelineStageFlags m_pending_shader_writes;
	mutable GraphicsState          m_graphics_state;
	HW::Context*        m_registers   = nullptr;
	HW::UserConfig*     m_user_config = nullptr;
	HW::Shader*         m_shaders     = nullptr;

	friend class CommandScheduler;
};

class RenderExecutor {
public:
	explicit RenderExecutor(RenderContext& context);
	~RenderExecutor();
	KYTY_CLASS_NO_COPY(RenderExecutor);

	// With `may_defer`, a dispatch whose compute pipeline is still compiling records nothing and
	// returns false; the caller runs it again later (see CommandProcessor::DispatchDirect).
	// Otherwise it waits for the pipeline, and always returns true.
	bool DispatchDirect(uint64_t submit_id, CommandBuffer& buffer, uint32_t thread_group_x,
	                    uint32_t thread_group_y, uint32_t thread_group_z, uint32_t mode,
	                    bool may_defer = false);
	bool DispatchIndirect(uint64_t submit_id, CommandBuffer& buffer, uint64_t args_addr,
	                      uint32_t mode, bool may_defer = false);

	void PrepareBindings(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	// Bindless heaps: register the draw's heaps, patch their regions, and once per frame resolve
	// the keys shaders flagged as pending.
	void PrepareBindlessHeaps(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	void ResolveBindlessRequests();
	bool ResolveBindlessKey(BindlessTable::Heap& heap, uint32_t key);
	void                           FindBuffers(PreparedBindings& bindings);
	void                           RebindBuffers(PreparedBindings& bindings);
	void                           RebindImages(PreparedBindings& bindings);
	void CommitBindings(CommandBuffer& buffer, vk::PipelineBindPoint pipeline_bind_point,
	                    const PipelineCache::Pipeline&     pipeline,
	                    std::span<PreparedBindings* const> bindings, bool compute_chain = false);

private:
	void DrawIndex(uint64_t submit_id, CommandBuffer& buffer, const DrawIndexArgs& args);
	void DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args);

	struct GraphicsBindings {
		std::array<PreparedBindings, 3> vertex;
		std::optional<PreparedBindings> pixel;
		// KYTY_STAGE_RECORDS: the first vertex stage's and the pixel stage's bindings as the last
		// draw of each program and image descriptors left them (see StageRecord).
		std::array<std::unordered_map<uint64_t, std::unique_ptr<PreparedBindings>>, 2> records;
	};
	// The bindings to prepare a stage in: with KYTY_STAGE_RECORDS, the record of the runtime's
	// program and image descriptors (`slot` 0: vertex, 1: pixel), else `fallback`.
	[[nodiscard]] PreparedBindings& StageRecord(uint32_t slot, const ShaderStageRuntime& runtime,
	                                            PreparedBindings& fallback);

	[[nodiscard]] TextureBinding ResolveTexture(const ShaderRecompiler::IR::ImageResource& resource,
	                                            const ShaderRecompiler::IR::DescriptorValue& value,
	                                            PreparedBindings::ImageSource* source = nullptr);
	// ResolveTexture into `out`, keeping its mip view storage: bindings resolve into the draw's own
	// slots without copying the image description around.
	void ResolveTextureInto(const ShaderRecompiler::IR::ImageResource&   resource,
	                        const ShaderRecompiler::IR::DescriptorValue& value,
	                        PreparedBindings::ImageSource* source, TextureBinding& out);
	void PrepareGraphicsBindings(std::span<PreparedBindings* const> stages,
	                             std::span<RenderColorInfo> colors);
	void ResolveRenderColorTarget(CommandBuffer& buffer, RenderColorInfo& target,
	                              uint32_t render_target_slice_offset, uint32_t render_target_slot,
	                              bool ignore_target_mask = false, bool exact_format = false);
	void ResolveRenderDepthTarget(CommandBuffer& buffer, RenderDepthInfo& target);
	[[nodiscard]] bool DepthStencilCopy(CommandBuffer& buffer);
	[[nodiscard]] bool ConsumeMetadataColorOperation(CommandBuffer& buffer,
	                                                 uint32_t       render_target_slice_offset);
	[[nodiscard]] bool PrepareDrawRenderState(CommandBuffer& buffer,
	                                          const DrawCallInfo& draw,
	                                          uint32_t            render_target_slice_offset,
	                                          DrawRenderState& state);
	void ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer, const DrawCallInfo& draw,
	                         DrawRenderState& state, vk::PrimitiveTopology topology,
	                         const DrawEmitInfo& emit, const DrawIndexBufferSource& index_source,
	                         bool primitive_restart_enable);
	[[nodiscard]] RenderState AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
	                                               uint32_t color_count, RenderDepthInfo& depth,
	                                               vk::ImageAspectFlags& feedback_aspects,
	                                               std::span<PreparedBindings* const> stages = {});
	[[nodiscard]] bool        ResolveColorTargets(CommandBuffer& buffer,
	                                              uint32_t render_target_slice_offset);
	void                      BindImage(ImageId id, bool storage);
	void                      BindRenderTarget(ImageId id);
	void                      ResetBindings();
	// Rebuilds the HTile of depth surfaces the dispatch's buffers read (TextureCache::SyncHtile).
	void                      SyncHtileReads(const ShaderComputeInputInfo& input);
	[[nodiscard]] bool        TryConsumeComputeMetaClear(const ShaderComputeInputInfo& input,
	                                                     const CommandBuffer&          buffer);
	[[nodiscard]] bool TryConsumeComputeImageClear(const ShaderComputeInputInfo& input,
	                                              CommandBuffer& command, uint32_t group_x,
	                                              uint32_t group_y, uint32_t group_z, uint32_t mode);
	[[nodiscard]] bool ReadsPendingWrites(std::span<PreparedBindings* const> stages,
	                                      const ShaderVertexInputInfo&       vertex_input,
	                                      const DrawIndexBufferSource&       index_source,
	                                      const DrawCallInfo&                draw) const;
	void RecordPendingWrites(std::span<PreparedBindings* const> stages);

	// Buffer ranges that draws wrote while their barrier is still pending (see
	// CommandBuffer::DeferShaderWriteBarrier), and whether only atomics wrote each one.
	struct PendingWrite {
		uint64_t begin       = 0;
		uint64_t end         = 0;
		bool     atomic_only = false;
	};

	// What ResolveRenderColorTarget resolved for each target slot, so that a draw with the same
	// target registers keeps it (see TextureCache::RefindImage).
	struct ColorTargetSource {
		HW::RenderTarget registers;
		uint32_t         mask                = 0;
		uint32_t         slice_offset        = 0;
		bool             ignore_target_mask  = false;
		bool             exact_format        = false;
		uint64_t         generation          = 0; // 0: not reusable.
		bool             overlap             = false; // See TextureCache::LastFindOverlap.
		uint32_t         metadata_base_layer = 0;
		RenderColorInfo  target;
		// The view FindRenderTarget last returned for this slot (see
		// TextureCache::IsRenderTargetCurrent); view_generation 0: none.
		vk::ImageView    view;
		ImageId          view_image;
		ImageViewInfo    view_info;
		uint64_t         view_generation = 0;
	};
	[[nodiscard]] static bool TargetReuseEnabled();
	// The same for the depth target: its description and image, keyed by its registers.
	struct DepthTargetSource {
		HW::DepthRenderTarget   registers;
		uint64_t                generation          = 0; // 0: not reusable.
		bool                    overlap             = false; // See TextureCache::LastFindOverlap.
		uint32_t                metadata_base_layer = 0;
		TextureCache::ImageDesc desc;
		ImageId                 image_id;
		// The view FindDepthTarget last returned (see TextureCache::IsDepthTargetCurrent);
		// view_generation 0: none.
		vk::ImageView           view;
		ImageId                 view_image;
		ImageViewInfo           view_info;
		uint64_t                view_generation      = 0;
		uint64_t                view_meta_generation = 0;
	};

	// KYTY_DRAW_CHAIN: the targets the last indexed draw resolved, with what they were resolved
	// under. A draw after it with no context register write between them, the same slot filter,
	// slice offset and pixel outputs, and an unchanged target state takes them (see
	// TakeDrawChain in renderDraw.cpp).
	struct DrawChain {
		bool                      valid        = false;
		uint64_t                  cx_writes    = 0;
		uint32_t                  slice_offset = 0;
		uint32_t                  color_filter = ~0u;
		uint32_t                  mrt_mask     = 0;
		TextureCache::TargetState targets {};
		uint32_t                  color_count  = 0;
		std::array<RenderColorInfo, RENDER_COLOR_ATTACHMENTS_MAX> colors {};
		std::array<uint32_t, RENDER_COLOR_ATTACHMENTS_MAX>        color_base_layers {};
		RenderDepthInfo           depth {};
		uint32_t                  depth_base_layer = 0;
	};
	[[nodiscard]] bool TakeDrawChain(uint32_t slice_offset, uint32_t mrt_mask, DrawRenderState& state);
	void               RecordDrawChain(uint32_t slice_offset, uint32_t mrt_mask,
	                                   const DrawRenderState& state);

	// KYTY_PIPELINE_CHAIN: the pipeline the last draw looked up, with the inputs of the lookup that
	// do not come from context registers (programs, targets' formats, topology, vertex input; see
	// MakePipelineChainKey in renderDraw.cpp). A draw with no context register written since and
	// the same inputs takes it without building the pipeline key.
	struct PipelineChainKey {
		uint32_t                 size = 0;
		bool                     usable = true;
		std::array<uint64_t, 64> words; // The first `size` are set.
		void                     Push(uint64_t word) {
			if (size == words.size()) {
				usable = false;
				return;
			}
			words[size++] = word;
		}
		[[nodiscard]] bool operator==(const PipelineChainKey& other) const {
			return size == other.size && std::memcmp(words.data(), other.words.data(), size * sizeof(uint64_t)) == 0;
		}
	};
	struct PipelineChain {
		PipelineCache::Pipeline* pipeline  = nullptr;
		uint64_t                 cx_writes = 0;
		PipelineChainKey         key;
	};

	RenderContext&                        m_context;
	DrawChain                             m_draw_chain;
	PipelineChain                         m_pipeline_chain;
	std::array<ColorTargetSource, RENDER_COLOR_ATTACHMENTS_MAX> m_color_target_sources;
	// Colour slots PrepareDrawRenderState may bind: all but one pass of a draw that binds one
	// surface to several slots (see AliasedColorSlots in renderDraw.cpp).
	uint32_t                              m_color_slot_filter = ~0u;
	DepthTargetSource                     m_depth_target_source;
	std::vector<PendingWrite>             m_pending_writes;
	GraphicsBindings                     m_graphics_bindings;
	PreparedBindings                     m_compute_bindings;
	std::vector<ImageId>                  m_bound_images;

	void PrepareBindlessSamplers(const ShaderStageRuntime& runtime, PreparedBindings& prepared);
	uint64_t                              m_bindless_frame = UINT64_MAX;
	std::vector<uint32_t>                 m_bindless_requests;
	std::vector<uint32_t>                 m_bindless_srt;
	// KYTY_TABLE_MODE: the block of the stage being rebound (RebindBuffers).
	std::vector<uint32_t>                 m_table_block;
	std::vector<vk::DescriptorBufferInfo> m_descriptor_buffers;
	std::vector<vk::DescriptorImageInfo>  m_descriptor_images;
	std::vector<vk::WriteDescriptorSet>   m_descriptor_writes;
	std::vector<uint32_t>                 m_image_occurrences;
	std::unique_ptr<MeshIndirectArgs>     m_mesh_indirect; // Created on first GPU-args draw.

	friend class CommandProcessor;
	friend struct RenderExecutorTestAccess;
};

[[nodiscard]] bool ResolveComputeBufferFill(const ShaderComputeInputInfo& input, uint32_t group_x,
                                            uint32_t group_y, uint32_t group_z, uint32_t mode,
                                            ShaderBufferResource& descriptor,
                                            uint32_t& packed_clear, uint64_t& size);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRENDER_H_ */
