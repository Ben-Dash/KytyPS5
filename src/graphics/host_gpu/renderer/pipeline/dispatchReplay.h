#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DISPATCHREPLAY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DISPATCHREPLAY_H_

namespace Libs::Graphics::DispatchReplay {

// kyty_emulator --dispatch-replay <snapshot dir | dir of snapshots> [options]: runs compute
// dispatches recorded with KYTY_DISPATCH_SNAPSHOT (dispatchSnapshot.h) again on an own Vulkan
// device, without the game: the shader translated by this build from its journal record (or the
// live or a given SPIR-V), every input as recorded. Reports a lost device or timeout, validation
// messages, and per output whether it matches what the dispatch wrote in the game.
// argv[0] is "--dispatch-replay".
int Run(int argc, char* argv[]);

} // namespace Libs::Graphics::DispatchReplay

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DISPATCHREPLAY_H_
