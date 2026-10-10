#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_VERTEXEXPLODELOG_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_VERTEXEXPLODELOG_H_

#include "common/switches.h"

#include <cstdint>
#include <cstdlib>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Diagnostics, KYTY_VTX_EXPLODE_CHECK=1 (off by default): every vertex and mesh shader checks the
// clip-space position it writes and logs a record when it explodes, without changing it. Kinds:
// a NaN or infinite component; |w| below 1e-6 unless x and y are zero too; in front of the camera
// (w > 1e-6) |x| or |y| above KYTY_VTX_EXPLODE_LIMIT * w (default 1000); and x = y = w = 0 (the usual ways to cull a vertex,
// (0,0,0,0) and (0,0,10,0); only counted).
// The shaders then bind the fault buffer and write into a region after its page bitmap;
// FaultManager copies that region back once per frame and VertexExplode prints the records.
// Without the switch no shader, binding or cache changes.
struct VertexExplodeSettings {
	bool  enabled = false;
	float limit   = 1000.0f;
};

inline const VertexExplodeSettings& VertexExplodeCheck() {
	static const VertexExplodeSettings settings = [] {
		VertexExplodeSettings result;
		const char* text = KYTY_GETENV("KYTY_VTX_EXPLODE_CHECK");
		result.enabled   = text != nullptr && *text != '\0' && *text != '0';
		if (const char* limit = KYTY_GETENV("KYTY_VTX_EXPLODE_LIMIT"); limit != nullptr) {
			const auto value = std::strtof(limit, nullptr);
			if (value > 0.0f) {
				result.limit = value;
			}
		}
		return result;
	}();
	return settings;
}

enum class VertexExplodeKind : uint32_t {
	NonFinite = 0, // a NaN or infinite component
	WZero     = 1, // |w| < 1e-6, x or y not zero
	Huge      = 2, // w > 0 and |x| or |y| above limit * w
	Zero      = 3, // x = y = w = 0 (culling idiom)
	Count     = 4,
};

// The log region, in DWORDs of the fault buffer. Its page bitmap ends at ExplodeLogBaseWord
// (FaultManager checks this). Header: word 0 counts every hit. Slot s = bucket(hash) * 4 + kind
// has a hit counter at ExplodeLogCounterWord + s; the hit that finds it 0 writes the slot's
// record at ExplodeLogRecordWord + s * ExplodeLogRecordWords. Record: marker 0x56580000 | kind |
// stage << 8, shader hash low and high, vertex index (mesh: local invocation * 2 + lane half),
// instance index (mesh: workgroup x), position x, y, z, w (float bits).
inline constexpr uint32_t ExplodeLogBaseWord    = 0x300000u;
inline constexpr uint32_t ExplodeLogHeaderWords = 16;
inline constexpr uint32_t ExplodeLogBuckets     = 512;
inline constexpr uint32_t ExplodeLogSlots =
    ExplodeLogBuckets * static_cast<uint32_t>(VertexExplodeKind::Count);
inline constexpr uint32_t ExplodeLogRecordWords = 16;
inline constexpr uint32_t ExplodeLogCounterWord = ExplodeLogHeaderWords;
inline constexpr uint32_t ExplodeLogRecordWord  = ExplodeLogCounterWord + ExplodeLogSlots;
inline constexpr uint32_t ExplodeLogWords =
    ExplodeLogRecordWord + ExplodeLogSlots * ExplodeLogRecordWords;
inline constexpr uint32_t ExplodeLogMarker      = 0x56580000u;

constexpr uint32_t ExplodeLogBucket(uint64_t shader_hash) {
	return static_cast<uint32_t>((shader_hash ^ (shader_hash >> 32u)) % ExplodeLogBuckets);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_VERTEXEXPLODELOG_H_
