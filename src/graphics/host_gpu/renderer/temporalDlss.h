#ifndef KYTY_GRAPHICS_TEMPORAL_DLSS_H_
#define KYTY_GRAPHICS_TEMPORAL_DLSS_H_

#include "graphics/presentation/dlss/dlssVulkan.h"
#include <array>
#include <memory>
#include <optional>

namespace Libs::Graphics {
class RenderContext;
struct ShaderComputeInputInfo;
struct PreparedBindings;

// Explicit opt-in pass metadata supplies roles. This provider never identifies a
// game, shader, guest address, or executable. The owner drains its scheduler before
// destruction and retains the renderer's device/allocator through that destruction.
class TemporalDlss {
public:
    TemporalDlss(RenderContext& context, std::shared_ptr<DlssVulkan> backend);
    ~TemporalDlss();
    TemporalDlss(const TemporalDlss&) = delete;
    TemporalDlss& operator=(const TemporalDlss&) = delete;

    [[nodiscard]] static std::optional<DlssProjectInfo> ProjectFromEnvironment();
    // Native NGX evaluation + RGB merge ticks retired by the real GPU timeline.
    // This is neither an initialization flag nor an SDL-presented-frame count.
    [[nodiscard]] uint64_t CompletedCount() const noexcept;
    // Call after final image/buffer rebinding. A true result requires an ordinary
    // primary dispatch, with compute chaining disabled, followed by AfterDispatch.
    [[nodiscard]] bool BeforeDispatch(const ShaderComputeInputInfo&, const PreparedBindings&,
                                     std::array<uint32_t, 3> groups, uint32_t guest_frame);
    void AfterDispatch(const ShaderComputeInputInfo&, const PreparedBindings&,
                       std::array<uint32_t, 3> groups, uint32_t guest_frame);

private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace Libs::Graphics
#endif
