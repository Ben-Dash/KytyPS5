#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <stdexcept>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

// A planned program the emitter cannot express (a translator bug for that shader). The caller
// skips the shader's draws and dispatches instead of ending the session.
class ValidationError: public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Strict per-compilation opt-in, also used by cache profile admission.
[[nodiscard]] bool DynamicFormatLookupSwitchEnabled();

std::vector<uint32_t> EmitProgram(const IR::Program& program,
                                  ShaderStageInputInfo input_info);

} // namespace Libs::Graphics::ShaderRecompiler::Spirv

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SPIRVEMITTER_H_ */
