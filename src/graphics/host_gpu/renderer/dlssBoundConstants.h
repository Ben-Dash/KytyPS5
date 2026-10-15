#ifndef KYTY_GRAPHICS_DLSS_BOUND_CONSTANTS_H_
#define KYTY_GRAPHICS_DLSS_BOUND_CONSTANTS_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/dlss/dlssVulkan.h"

#include <cstdint>
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
} // namespace Libs::Graphics

#endif
