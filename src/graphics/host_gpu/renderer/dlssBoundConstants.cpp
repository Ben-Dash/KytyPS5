#include "graphics/host_gpu/renderer/dlssBoundConstants.h"

#include "graphics/host_gpu/renderer/renderContext.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>

namespace Libs::Graphics {

struct DlssBoundConstantSnapshot::State {
    RenderContext* owner = nullptr;
    uint32_t size = 0;
    uint64_t copy_tick = 0;
    bool ready = false;
    std::array<uint8_t, 1024> bytes{};
    std::shared_ptr<Buffer> staging;
};

DlssStatus ReadDlssBoundConstants(RenderContext& context, vk::DescriptorBufferInfo view,
                                 uint32_t adjustment, std::span<uint8_t> output) {
    // Validate size_t before narrowing it for the ticket API. Invalid requests
    // must never enqueue a smaller copy as a side effect of integer truncation.
    if (output.empty() || output.size() > 1024)
        return {DlssStatusCode::InvalidArgument, 0, "constant snapshot requires a bound buffer and 1..1024 bytes"};
    DlssBoundConstantSnapshot snapshot;
    if (const auto status = QueueDlssBoundConstants(context, view, adjustment,
            static_cast<uint32_t>(output.size()), snapshot); !status) return status;
    const std::array batch{DlssBoundConstantRead{&snapshot, output}};
    return FinishDlssBoundConstants(context, batch);
}

DlssStatus QueueDlssBoundConstants(RenderContext& context, vk::DescriptorBufferInfo view,
    uint32_t adjustment, uint32_t bytes, DlssBoundConstantSnapshot& output) {
    if (output.state || bytes == 0 || bytes > 1024)
        return {DlssStatusCode::InvalidArgument, 0, "constant snapshot requires an empty ticket and 1..1024 bytes"};
    if (!view.buffer)
        return {DlssStatusCode::InvalidArgument, 0, "constant snapshot requires a bound buffer and 1..1024 bytes"};
    if (view.range == VK_WHOLE_SIZE || adjustment > view.range || bytes > view.range - adjustment)
        return {DlssStatusCode::InvalidArgument, 0, "constant bytes outside actual descriptor range"};
    if (view.offset > std::numeric_limits<uint64_t>::max() - adjustment ||
        bytes > std::numeric_limits<uint64_t>::max() - (view.offset + adjustment))
        return {DlssStatusCode::InvalidArgument, 0, "constant native offset overflows"};
    const auto start = view.offset + adjustment;
    auto state = std::make_shared<DlssBoundConstantSnapshot::State>();
    state->owner = &context; state->size = bytes;
    auto& stream = context.GetBufferCache().ActiveStream();
    if (view.buffer == stream.Handle() && stream.Usage() == MemoryUsage::Stream &&
        (stream.MemoryProperties() & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        const auto mapped = stream.Mapped();
        if (view.offset > mapped.size() || adjustment > mapped.size() - view.offset)
            return {DlssStatusCode::InvalidArgument, 0, "constant start outside mapped stream"};
        if (bytes > mapped.size() - start)
            return {DlssStatusCode::InvalidArgument, 0, "constant bytes outside mapped stream"};
        // This host-written ring reservation is immutable until retirement.
        std::memcpy(state->bytes.data(), mapped.data() + static_cast<size_t>(start), bytes);
        state->ready = true;
        output.state = std::move(state);
        return {};
    }

    auto& scheduler = context.GetCommandScheduler();
    if (!scheduler.Active() || scheduler.Current().IsInvalid() || CommandScheduler::InDeferredOperation())
        return {DlssStatusCode::InvalidState, 0, "constant GPU snapshot requires an active recording boundary"};
    state->staging = std::make_shared<Buffer>(context.GetGraphics(), scheduler,
        MemoryUsage::Download, 0, AllFlags, bytes);
    state->copy_tick = scheduler.CurrentTick();
    // Own the destination before any copy is recorded. An abandoned ticket,
    // timeout or partial recording failure cannot free in-flight native memory.
    scheduler.DeferOperation([staging = state->staging] {});
    auto& current = scheduler.Current();
    current.EndRendering();
    auto command = current.Handle();
    const vk::MemoryBarrier before{
        .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
    };
    command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0, nullptr);
    const vk::BufferCopy region{start, 0, bytes};
    command.copyBuffer(view.buffer, state->staging->Handle(), 1, &region);
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
    output.state = std::move(state);
    return {};
}

DlssStatus FinishDlssBoundConstants(RenderContext& context,
    std::span<const DlssBoundConstantRead> batch) {
    if (batch.empty() || batch.size() > 3)
        return {DlssStatusCode::InvalidArgument, 0, "constant snapshot batch requires 1..3 tickets"};
    std::array<std::shared_ptr<DlssBoundConstantSnapshot::State>, 3> states;
    uint64_t required_tick = 0;
    for (size_t i = 0; i < batch.size(); ++i) {
        const auto& read = batch[i];
        if (!read.snapshot || !read.snapshot->state || read.snapshot->state->owner != &context ||
            read.output.size() != read.snapshot->state->size)
            return {DlssStatusCode::InvalidArgument, 0, "constant snapshot ticket owner or output extent mismatch"};
        states[i] = read.snapshot->state;
        if (!states[i]->ready) {
            if (!states[i]->staging || states[i]->copy_tick == 0)
                return {DlssStatusCode::InvalidState, 0, "constant snapshot has no recorded native copy"};
            required_tick = std::max(required_tick, states[i]->copy_tick);
        }
    }
    if (required_tick != 0) {
        auto& scheduler = context.GetCommandScheduler();
        if (!scheduler.Active() || CommandScheduler::InDeferredOperation() ||
            required_tick > scheduler.CurrentTick())
            return {DlssStatusCode::InvalidState, 0, "constant batch requires its owning recording boundary"};
        // Older ticks were submitted by the normal scheduler between producer
        // and consumer. Submit only when the maximum copy tick is still current.
        const bool submit_current = required_tick == scheduler.CurrentTick();
        if (submit_current) {
            if (scheduler.Current().IsInvalid())
                return {DlssStatusCode::InvalidState, 0, "constant batch has an invalid recording buffer"};
            required_tick = scheduler.Submit();
        }
        auto& master = scheduler.GetMasterSemaphore();
        master.Refresh();
        vk::Result result = vk::Result::eSuccess;
        if (!master.IsFree(required_tick)) {
            const vk::Semaphore semaphore = master.Handle();
            const vk::SemaphoreWaitInfo wait{
                .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &required_tick,
            };
            result = context.GetGraphics().device.waitSemaphores(&wait, 5'000'000'000ull);
            if (result == vk::Result::eSuccess) master.Refresh();
        }
        // Reacquire only a buffer this helper submitted, on success or timeout.
        // Never run retirement callbacks halfway through prepared bindings.
        if (submit_current) scheduler.BeginCommand();
        if (result != vk::Result::eSuccess)
            return {DlssStatusCode::NativeError, static_cast<uint32_t>(result),
                result == vk::Result::eTimeout ? "actual-bound constant GPU snapshot timed out" :
                                                "actual-bound constant GPU snapshot wait failed"};
        for (size_t i = 0; i < batch.size(); ++i) {
            auto& state = *states[i];
            if (state.ready) continue;
            state.staging->Invalidate(0, state.size);
            std::memcpy(state.bytes.data(), state.staging->Mapped().data(), state.size);
            state.ready = true;
            state.staging.reset();
        }
    }
    // No caller output is touched until the complete batch has passed its owner,
    // range and completion checks, including any bounded native wait.
    for (size_t i = 0; i < batch.size(); ++i)
        std::memcpy(batch[i].output.data(), states[i]->bytes.data(), batch[i].output.size());
    return {};
}

} // namespace Libs::Graphics
