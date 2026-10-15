#ifndef KYTY_GRAPHICS_DLSS_BOUND_CONSTANTS_H_
#define KYTY_GRAPHICS_DLSS_BOUND_CONSTANTS_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/dlss/dlssVulkan.h"

#include <cstdint>
#include <memory>
#include <span>

namespace Libs::Graphics {
class RenderContext;

// Reads the actual prepared native descriptor, including its packed byte
// adjustment, for 1..1024 caller-owned bytes. The caller supplies a validated
// native descriptor: its finite range is within the live allocation, with
// TRANSFER_SRC usage, owned through the scheduler's current tick. Ordinary
// buffers require an active recording boundary and may submit/wait up to 5s.
// The stable current wrapper gets a fresh native command handle; callers must
// acquire that handle afterwards. No retirement callbacks run here. Failure
// leaves output unchanged, retaining any in-flight staging allocation until
// actual retirement. Current guest backing is never consulted.
[[nodiscard]] DlssStatus ReadDlssBoundConstants(RenderContext& context,
                                               vk::DescriptorBufferInfo view,
                                               uint32_t adjustment,
                                               std::span<uint8_t> output);

// A caller-owned immutable snapshot. Its source is the actual prepared native
// descriptor; no subsequent read of guest backing is involved. Keep each ticket
// with its producer/consumer association until the batch is resolved.
struct DlssBoundConstantSnapshot {
    struct State;
    std::shared_ptr<State> state;
};
struct DlssBoundConstantRead {
    const DlssBoundConstantSnapshot* snapshot = nullptr;
    std::span<uint8_t> output;
};

// Record without submitting/waiting, or snapshot an immutable host stream at
// once. Leave the destination ticket unchanged on failure. Source ownership and
// TRANSFER_SRC requirements are the same as the synchronous API.
[[nodiscard]] DlssStatus QueueDlssBoundConstants(RenderContext& context,
    vk::DescriptorBufferInfo view, uint32_t adjustment, uint32_t bytes,
    DlssBoundConstantSnapshot& output);

// Resolve 1..3 tickets from this exact RenderContext together. Validate the
// entire batch before changing any output; never pop retirement callbacks while
// the caller still owns prepared bindings. Native waits are bounded to five
// seconds. Submit once only if an unresolved copy still belongs to the current
// recording tick; copies submitted by unrelated work wait their recorded tick.
// Immutable host-stream and already resolved snapshots need no GPU submission.
[[nodiscard]] DlssStatus FinishDlssBoundConstants(RenderContext& context,
    std::span<const DlssBoundConstantRead> batch);
} // namespace Libs::Graphics

#endif
