#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_

#include <array>
#include <cstdint>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Descriptor set 2 of a pipeline that samples bindless images (the host side is BindlessTable).
// Sets 0 and 1 are the pipeline's own descriptors (vertex-side and pixel stages).
inline constexpr uint32_t BindlessDescriptorSet  = 2;
inline constexpr uint32_t BindlessImages2D       = 0;
inline constexpr uint32_t BindlessImages2DArray  = 1;
inline constexpr uint32_t BindlessImagesCube     = 2;
inline constexpr uint32_t BindlessImages3D       = 3;
inline constexpr uint32_t BindlessTranslation    = 4;
inline constexpr uint32_t BindlessFeedback       = 5;
// Samplers mirrored from guest sampler heaps (16-byte S# records); slot 0 is the default sampler.
inline constexpr uint32_t BindlessSamplers       = 6;
// 2D and 2D-array images of an integer format (Ghost of Yotei: R8_UINT terrain maps): a shader
// whose heap holds them reads them through these arrays with an integer result type.
inline constexpr uint32_t BindlessImages2DUint      = 7;
inline constexpr uint32_t BindlessImages2DArrayUint = 8;
inline constexpr uint32_t BindlessImages2DSint      = 9;
inline constexpr uint32_t BindlessImages2DArraySint = 10;
// The image arrays, in placeholder order: the four float arrays, then the four integer arrays.
inline constexpr uint32_t BindlessImageArrayCount   = 8;
inline constexpr std::array<uint32_t, BindlessImageArrayCount> BindlessImageArrayBindings {
    BindlessImages2D,         BindlessImages2DArray,     BindlessImagesCube,
    BindlessImages3D,         BindlessImages2DUint,      BindlessImages2DArrayUint,
    BindlessImages2DSint,     BindlessImages2DArraySint};
// The index of an image array's binding in BindlessImageArrayBindings (UINT32_MAX: none).
constexpr uint32_t BindlessImageArrayIndex(uint32_t binding) {
	if (binding <= BindlessImages3D) {
		return binding;
	}
	if (binding >= BindlessImages2DUint && binding <= BindlessImages2DArraySint) {
		return binding - BindlessImages2DUint + 4u;
	}
	return UINT32_MAX;
}
// A translation entry for a key whose texture is not resident yet; the shader samples slot 0.
inline constexpr uint32_t BindlessPending        = 0xffffffffu;
// A translation entry of a 3D-array key whose T# is a 2D texture: the slot is in the 2D float
// array. The GPU reads a T# by its own type, so a 3D sample instruction reads a 2D texture as 2D
// (Ghost of Yotei builds its moon-shadow volumes from 2D shadow maps this way).
inline constexpr uint32_t BindlessFlat2DInVolume = 0x40000000u;
// A translation entry of a 2D float-array key whose T# is a 3D texture: the slot is in the 3D
// array, and an explicit-LOD 2D sample reads it at (u, v, 0) (the instruction has no r coordinate).
inline constexpr uint32_t BindlessVolumeIn2D     = 0x20000000u;
// KYTY_BINDLESS_TYPED: the GPU reads a key by its own T#, so a translation entry names the image
// array its slot lies in (bits 24-27: BindlessImageArrayIndex + 1; 0 is the array of the
// shader's instruction). The shader reads the other arrays its instruction can read
// (BindlessTypedAlternates) in a branch.
inline constexpr uint32_t BindlessKindShift      = 24;
inline constexpr uint32_t BindlessKindMask       = 0xfu << BindlessKindShift;
inline constexpr uint32_t BindlessSlotFlags      = BindlessFlat2DInVolume | BindlessVolumeIn2D | BindlessKindMask;
// The image arrays (as BindlessImageArrayIndex bits) an instruction of `binding` also reads with
// KYTY_BINDLESS_TYPED: a 2D sample reads a 3D T# at (u, v, 0) and a 3D sample a 2D T# at (u, v);
// any 2D or 2D-array sample reads T#s of the other numeric classes of its shape as raw bits. Depth
// compares and cubes read their own array only.
constexpr uint32_t BindlessTypedAlternates(uint32_t binding, bool depth_compare) {
	if (depth_compare) {
		return 0u;
	}
	const auto bit = [](uint32_t array_binding) { return 1u << BindlessImageArrayIndex(array_binding); };
	switch (binding) {
		case BindlessImages2D: return bit(BindlessImages3D) | bit(BindlessImages2DUint) | bit(BindlessImages2DSint);
		case BindlessImages2DUint: return bit(BindlessImages2D) | bit(BindlessImages2DSint);
		case BindlessImages2DSint: return bit(BindlessImages2D) | bit(BindlessImages2DUint);
		case BindlessImages2DArray: return bit(BindlessImages2DArrayUint) | bit(BindlessImages2DArraySint);
		case BindlessImages2DArrayUint: return bit(BindlessImages2DArray) | bit(BindlessImages2DArraySint);
		case BindlessImages2DArraySint: return bit(BindlessImages2DArray) | bit(BindlessImages2DArrayUint);
		case BindlessImages3D: return bit(BindlessImages2D);
		default: return 0u;
	}
}
// The array a resolved translation entry's slot lies in, for a heap whose instruction reads
// `heap_binding`.
constexpr uint32_t BindlessEntryBinding(uint32_t entry, uint32_t heap_binding) {
	if ((entry & BindlessKindMask) != 0u) {
		return BindlessImageArrayBindings[((entry & BindlessKindMask) >> BindlessKindShift) - 1u];
	}
	if ((entry & BindlessFlat2DInVolume) != 0u) {
		return BindlessImages2D;
	}
	if ((entry & BindlessVolumeIn2D) != 0u) {
		return BindlessImages3D;
	}
	return heap_binding;
}
// The S# a sampler without a usable heap entry gets: wrap on every axis, trilinear, full LOD range.
inline constexpr std::array<uint32_t, 4> BindlessDefaultSampler {
    0x00000000u,                             // wrap on every axis
    0xfffu << 12u,                           // max_lod 255.9
    (1u << 20u) | (1u << 22u) | (2u << 26u), // bilinear mag/min, linear mip
    0x00000000u};

// Research: the tail of the feedback buffer holds loop-watchdog trip reports. Word
// WatchdogReportBase counts reports; report n is WatchdogReportWords words starting at
// WatchdogReportBase + WatchdogReportWords * (n + 1). No heap region reaches this far.
inline constexpr uint32_t WatchdogReportBase     = 0xf0000u;
inline constexpr uint32_t WatchdogReportWords    = 64;
inline constexpr uint32_t WatchdogReportSlots    = 1023;
inline constexpr uint32_t WatchdogReportPhis     = 24; // header phi values per lane half

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_BINDLESSBINDINGS_H_
