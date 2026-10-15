#include "graphics/host_gpu/renderer/dlssBoundConstants.h"

#include "graphics/host_gpu/renderer/renderContext.h"

#include <cstring>
#include <limits>
#include <memory>

namespace Libs::Graphics {

DlssStatus ReadDlssBoundConstants(RenderContext& context, vk::DescriptorBufferInfo view,
                                 uint32_t adjustment, std::span<uint8_t> output) {
    if (!view.buffer || output.empty() || output.size() > 1024) {
        return {DlssStatusCode::InvalidArgument, 0, "constant snapshot requires a bound buffer and 1..1024 bytes"};
    }
    if (view.range == VK_WHOLE_SIZE || adjustment > view.range ||
        output.size() > view.range - adjustment) {
        return {DlssStatusCode::InvalidArgument, 0, "constant bytes outside actual descriptor range"};
    }
    if (view.offset > std::numeric_limits<uint64_t>::max() - adjustment ||
        output.size() > std::numeric_limits<uint64_t>::max() - (view.offset + adjustment)) {
        return {DlssStatusCode::InvalidArgument, 0, "constant native offset overflows"};
    }
    const auto start = view.offset + adjustment;
    auto& stream = context.GetBufferCache().ActiveStream();
    if (view.buffer == stream.Handle() && stream.Usage() == MemoryUsage::Stream &&
        (stream.MemoryProperties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        const auto mapped = stream.Mapped();
        if (view.offset > mapped.size() || adjustment > mapped.size() - view.offset) {
            return {DlssStatusCode::InvalidArgument, 0, "constant start outside mapped stream"};
        }
        if (output.size() > mapped.size() - start) {
            return {DlssStatusCode::InvalidArgument, 0, "constant bytes outside mapped stream"};
        }
        // This host-written ring reservation is immutable until retirement.
        std::memcpy(output.data(), mapped.data() + static_cast<size_t>(start), output.size());
        return {};
    }

    auto& scheduler = context.GetCommandScheduler();
    if (!scheduler.Active() || scheduler.Current().IsInvalid() || CommandScheduler::InDeferredOperation()) {
        return {DlssStatusCode::InvalidState, 0, "constant GPU snapshot requires an active recording boundary"};
    }
    auto staging = std::make_shared<Buffer>(context.GetGraphics(), scheduler,
        MemoryUsage::Download, 0, AllFlags, output.size());
    // Register before the first command. A timeout or later partial failure must
    // never destroy an allocation a queued native copy can still write.
    scheduler.DeferOperation([staging] {});
    auto& current = scheduler.Current();
    current.EndRendering();
    auto command = current.Handle();
    const vk::MemoryBarrier before{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0, nullptr);
    const vk::BufferCopy region{start, 0, output.size()};
    command.copyBuffer(view.buffer, staging->Handle(), 1, &region);
    const vk::MemoryBarrier after[]{
        {.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
         .dstAccessMask = vk::AccessFlagBits::eHostRead},
        {.srcAccessMask = vk::AccessFlagBits::eTransferRead,
         .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite},
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eHost | vk::PipelineStageFlagBits::eAllCommands,
        {}, 2, after, 0, nullptr, 0, nullptr);
    scheduler.NoteCommandRecorded();
    const auto tick = scheduler.Submit();
    auto& master = scheduler.GetMasterSemaphore();
    const vk::Semaphore semaphore = master.Handle();
    const vk::SemaphoreWaitInfo wait{
        .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &tick,
    };
    const auto result = context.GetGraphics().device.waitSemaphores(&wait, 5'000'000'000ull);
    if (result == vk::Result::eSuccess) master.Refresh();
    // The C++ CommandBuffer wrapper stays stable; its native pooled handle is
    // replaced. Do not run retirement callbacks midway through prepared bindings.
    scheduler.BeginCommand();
    if (result != vk::Result::eSuccess) {
        return {DlssStatusCode::NativeError, static_cast<uint32_t>(result),
            result == vk::Result::eTimeout ? "actual-bound constant GPU snapshot timed out" :
                                            "actual-bound constant GPU snapshot wait failed"};
    }
    staging->Invalidate(0, output.size());
    std::memcpy(output.data(), staging->Mapped().data(), output.size());
    return {};
}

} // namespace Libs::Graphics
