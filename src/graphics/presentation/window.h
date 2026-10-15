#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class Presenter;

// Optional host backend preparation after Config/Log, before guest VA reservation.
// Without explicit temporal metadata this is a no-op. Renderer and controller
// integration remain deferred to WindowInit at the ordinary graphics lifecycle.
void                     WindowPrepareDevice(uint32_t width, uint32_t height);
[[nodiscard]] Presenter& WindowInit(uint32_t width, uint32_t height);
void                     WindowRun();
void                     WindowShutdown();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
