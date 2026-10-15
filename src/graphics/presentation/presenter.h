#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_

#include "common/common.h"

#include <memory>
#include <span>

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
struct ImageInfo;
struct DlssPresentationInput;
struct DlssStatus;
#if defined(KYTY_DLSS_PRESENTER_GPU_TEST_ACCESS)
struct DlssImage;
#endif
struct WindowContext;

class Presenter final {
public:
	struct Frame;
	struct Layer {
		Frame* frame;
		int    bus;
		bool   premultiplied_alpha;
	};

	explicit Presenter(WindowContext& window);
	~Presenter();
	KYTY_CLASS_NO_COPY(Presenter);

	[[nodiscard]] Frame&         PrepareFrame(CommandBuffer& command, const ImageInfo& info);
    // Optional explicit semantic path. Null on failure; the caller may present
    // its original guest color with PrepareFrame. No input discovery/fabrication.
    // Producer command buffer must still be submitted/retired on SDK failure.
    [[nodiscard]] Frame* PrepareTemporalFrame(CommandBuffer& command,
        const DlssPresentationInput& input, DlssStatus& status);
	[[nodiscard]] Frame&         PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
	                                               CommandBuffer* producer = nullptr);
	[[nodiscard]] bool           PresentLastFrame();
	[[nodiscard]] bool           IsGuestPaused() const noexcept;
	[[nodiscard]] bool           NeedsSystemOverlayRefresh() const noexcept;
	[[nodiscard]] RenderContext& Renderer() const noexcept;
	void                         Present(Frame& frame);
	void                         Present(std::span<const Layer> layers);
	void                         ClearLayer(int bus);
	void                         Discard(Frame& frame);
#if defined(KYTY_DLSS_PRESENTER_GPU_TEST_ACCESS)
    // Native fixture read-only observation; no shipping API or resource mutation.
    [[nodiscard]] static DlssImage TestFrameImage(const Frame& frame);
    [[nodiscard]] uint64_t TestLastTemporalFrameId() const;
    [[nodiscard]] uint64_t TestEvaluationAttempts() const;
    [[nodiscard]] bool TestLastEvaluationReset() const;
#endif

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
