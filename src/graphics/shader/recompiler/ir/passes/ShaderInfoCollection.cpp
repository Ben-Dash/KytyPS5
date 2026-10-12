#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

[[noreturn]] void Fail(std::string_view message) {
	EXIT("shader info collection failed: %s", std::string(message).c_str());
	std::abort();
}

void AddInput(ShaderInfo& info, StageInputKind kind, uint32_t location, uint32_t components,
              std::string name, bool per_vertex = false) {
	const auto input = std::find_if(info.inputs.begin(), info.inputs.end(), [=](const auto& value) {
		return value.kind == kind && value.location == location;
	});
	if (input == info.inputs.end()) {
		info.inputs.push_back({kind, location, components, std::move(name), per_vertex});
	} else {
		input->component_count = std::max(input->component_count, components);
		input->per_vertex      = input->per_vertex || per_vertex;
	}
}

bool HasOutput(const ShaderInfo& info, StageOutputKind kind, uint32_t index) {
	return std::any_of(info.outputs.begin(), info.outputs.end(), [=](const auto& output) {
		return output.kind == kind && output.index == index;
	});
}

void AddOutput(ShaderInfo& info, StageOutputKind kind, uint32_t index, uint32_t location,
               std::string name) {
	if (!HasOutput(info, kind, index)) {
		info.outputs.push_back({kind, index, location, std::move(name)});
	}
}

void ValidateOptions(const Program& program, ShaderStageInputInfo input_info) {
	switch (program.stage) {
		case ShaderType::Vertex:
		case ShaderType::Local:
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation:
		case ShaderType::Mesh:
			if (input_info.vertex == nullptr) {
				return Fail("vertex shader has no input metadata");
			}
			if (input_info.vertex->resources_num < 0 ||
			    input_info.vertex->resources_num > ShaderVertexInputInfo::RES_MAX) {
				return Fail("vertex resource count is out of range");
			}
			return;
		case ShaderType::Pixel:
			if (input_info.pixel == nullptr) {
				return Fail("pixel shader has no input metadata");
			}
			if (input_info.pixel->input_num > std::size(input_info.pixel->interpolator_settings)) {
				return Fail("pixel input count is out of range");
			}
			return;
		case ShaderType::Compute:
			if (input_info.compute == nullptr) {
				return Fail("compute shader has no input metadata");
			}
			if (input_info.compute->thread_ids_num < 0 || input_info.compute->thread_ids_num > 3) {
				return Fail("compute thread ID count is out of range");
			}
			return;
		default: return Fail("unsupported shader stage for info collection");
	}
}

void ValidateValueReferences(const Program& program, ShaderStageInputInfo input_info) {
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			switch (inst.GetOpcode()) {
				case ValueOpcode::GetAttribute: {
					if (!inst.Arg(0).IsImmediate() || inst.Arg(0).GetType() != Type::U32 ||
					    !inst.Arg(1).IsImmediate() || inst.Arg(1).GetType() != Type::U32) {
						return Fail("typed attribute reference is not constant");
					}
					if ((program.stage == ShaderType::Vertex ||
					     program.stage == ShaderType::Local) &&
					    (inst.Arg(1).U32() >= 4u ||
					     inst.Arg(0).U32() >=
					         static_cast<uint32_t>(input_info.vertex->resources_num))) {
						return Fail("vertex input reference is out of range");
					}
					if (program.stage == ShaderType::Pixel &&
					    (inst.Arg(1).U32() >= 4u ||
					     inst.Arg(0).U32() >= input_info.pixel->input_num)) {
						return Fail("pixel input reference is out of range");
					}
					break;
				}
				case ValueOpcode::GetInterpolationParameter: {
					if (program.stage != ShaderType::Pixel || !inst.Arg(0).IsImmediate() ||
					    inst.Arg(0).GetType() != Type::U32 || !inst.Arg(1).IsImmediate() ||
					    inst.Arg(1).GetType() != Type::U32 || !inst.Arg(2).IsImmediate() ||
					    inst.Arg(2).GetType() != Type::U32) {
						return Fail("interpolation parameter reference is invalid");
					}
					if (inst.Arg(0).U32() >= input_info.pixel->input_num ||
					    inst.Arg(1).U32() >= 4u || inst.Arg(2).U32() >= 3u) {
						return Fail("interpolation parameter reference is out of range");
					}
					break;
				}
				case ValueOpcode::GetBuiltin: {
					if (!inst.Arg(0).IsImmediate() || inst.Arg(0).GetType() != Type::U32 ||
					    !inst.Arg(1).IsImmediate() || inst.Arg(1).GetType() != Type::U32) {
						return Fail("typed builtin reference is not constant");
					}
					const auto kind      = static_cast<StageInputKind>(inst.Arg(0).U32());
					const auto component = inst.Arg(1).U32();
					switch (kind) {
						case StageInputKind::PackedAncillary:
							return Fail("packed pixel ancillary input has an unsupported live use");
						case StageInputKind::Layer:
						case StageInputKind::SampleId:
							if (program.stage != ShaderType::Pixel || component != 0u) {
								return Fail("typed pixel scalar input is invalid");
							}
							break;
						case StageInputKind::VertexIndex:
						case StageInputKind::InvocationId:
						case StageInputKind::PrimitiveId:
						case StageInputKind::InstanceIndex:
						case StageInputKind::FrontFacing:
						case StageInputKind::LocalInvocationIndex:
							if (component != 0u) {
								return Fail("typed scalar builtin component is out of range");
							}
							break;
						case StageInputKind::FragCoord:
							if (component >= 4u) {
								return Fail("typed fragment-coordinate component is out of range");
							}
							break;
						case StageInputKind::BaryCoordSmooth:
						case StageInputKind::BaryCoordSmoothSample:
						case StageInputKind::BaryCoordSmoothCentroid:
						case StageInputKind::BaryCoordNoPerspective:
							if (component >= 2u) {
								return Fail("typed barycentric component is out of range");
							}
							break;
						case StageInputKind::TessCoord:
						case StageInputKind::WorkgroupId:
						case StageInputKind::LocalInvocationId:
						case StageInputKind::GlobalInvocationId:
							if (component >= 3u) {
								return Fail("typed invocation builtin component is out of range");
							}
							break;
						case StageInputKind::Parameter:
						default: return Fail("typed builtin kind is invalid");
					}
					break;
				}
				case ValueOpcode::SetAttribute: {
					const auto index = inst.Flags<ExportFlags>().index;
					if (index >= program.export_info.size()) {
						return Fail("typed export metadata index is out of range");
					}
					const auto& exp = program.export_info[index];
					if (exp.kind == ExportTargetKind::Position && exp.index != 0 && exp.en != 0 &&
					    program.stage != ShaderType::Vertex && program.stage != ShaderType::Mesh &&
					    program.stage != ShaderType::TessellationEvaluation) {
						return Fail("auxiliary position export requires a vertex, mesh, or "
						            "tessellation evaluation shader");
					}
					break;
				}
				default: break;
			}
		}
	}
}

void CollectVertexInputs(const Program& program, const ShaderVertexInputInfo* vertex,
                         ShaderInfo& info) {
	AddInput(info, StageInputKind::VertexIndex, 0, 1, "gl_VertexIndex");
	AddInput(info, StageInputKind::InstanceIndex, 0, 1, "gl_InstanceIndex");
	uint32_t used_components[ShaderVertexInputInfo::RES_MAX] = {};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::GetAttribute) {
				const auto attr       = inst.Arg(0).U32();
				const auto chan       = inst.Arg(1).U32();
				used_components[attr] = std::max(used_components[attr], chan + 1u);
			}
		}
	}
	for (uint32_t attr = 0; attr < static_cast<uint32_t>(vertex->resources_num); attr++) {
		if (used_components[attr] != 0) {
			AddInput(info, StageInputKind::Parameter, attr, used_components[attr],
			         fmt::format("in_attr_{}", attr));
		}
	}
}

void CollectPixelInputs(const Program& program, const ShaderPixelInputInfo* pixel,
                        ShaderInfo& info) {
	if (pixel->HasPositionInput()) {
		AddInput(info, StageInputKind::FragCoord, 0, 4, "gl_FragCoord");
	}
	if (pixel->ps_front_face) {
		AddInput(info, StageInputKind::FrontFacing, 0, 1, "gl_FrontFacing");
	}
	std::array<bool, 32> per_vertex {};
	std::array<bool, 32> interpolated {};
	std::array<bool, 32> read {};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::GetAttribute) {
				interpolated[inst.Arg(0).U32()] = true;
				read[inst.Arg(0).U32()]         = true;
			} else if (inst.GetOpcode() == ValueOpcode::GetInterpolationParameter) {
				const auto input = inst.Arg(0).U32();
				read[input]      = true;
				const auto mode  = inst.Arg(2).U32();
				per_vertex[input] =
				    per_vertex[input] || mode < 2u || !ShaderPixelParameterIsFlat(*pixel, input);
			}
		}
	}
	// Aliases of a vertex output share one SPIR-V interface variable. If any
	// alias reads raw vertices, interpolate the other aliases from those too. A flat and a
	// smooth alias of one output both read the raw vertices (one variable cannot be both).
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		for (uint32_t alias = 0; alias < pixel->input_num; alias++) {
			if (alias == input || !read[input] || !read[alias] ||
			    ShaderPixelParameterMappedLocation(*pixel, input) !=
			        ShaderPixelParameterMappedLocation(*pixel, alias)) {
				continue;
			}
			if (ShaderPixelParameterIsFlat(*pixel, input) !=
			    ShaderPixelParameterIsFlat(*pixel, alias)) {
				per_vertex[input] = true;
			}
		}
	}
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		for (uint32_t alias = 0; alias < pixel->input_num; alias++) {
			if (ShaderPixelParameterMappedLocation(*pixel, input) ==
			        ShaderPixelParameterMappedLocation(*pixel, alias) &&
			    ShaderPixelParameterIsFlat(*pixel, input) ==
			        ShaderPixelParameterIsFlat(*pixel, alias)) {
				per_vertex[input] = per_vertex[input] || per_vertex[alias];
			}
		}
	}
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		AddInput(info, StageInputKind::Parameter, input, 4, fmt::format("in_param_{}", input),
		         per_vertex[input]);
	}
	for (uint32_t input = 0; input < pixel->input_num; input++) {
		if (interpolated[input] && per_vertex[input]) {
			const auto kind = pixel->ps_no_perspective ? StageInputKind::BaryCoordNoPerspective
			                                           : StageInputKind::BaryCoordSmooth;
			AddInput(info, kind, 0, 3,
			         pixel->ps_no_perspective ? "gl_BaryCoordNoPerspKHR" : "gl_BaryCoordKHR");
			break;
		}
	}
}

void CollectComputeInputs(const ShaderComputeInputInfo* compute, ShaderInfo& info) {
	if (compute->group_id[0] || compute->group_id[1] || compute->group_id[2]) {
		AddInput(info, StageInputKind::WorkgroupId, 0, 3, "gl_WorkGroupID");
	}
	if (compute->thread_ids_num > 0) {
		AddInput(info, StageInputKind::LocalInvocationId, 0, 3, "gl_LocalInvocationID");
	}
	if (compute->thread_ids_num > 0 || compute->tg_size_en) {
		AddInput(info, StageInputKind::LocalInvocationIndex, 0, 1, "gl_LocalInvocationIndex");
	}
	if (compute->dispatch_thread_dimensions) {
		AddInput(info, StageInputKind::GlobalInvocationId, 0, 3, "gl_GlobalInvocationID");
	}
}

struct DmaAddressSources {
	std::vector<uint32_t> registers;
	std::vector<uint32_t> bases;
	bool complete = true;
};

// Track only known integer address/value semantics. `registers` includes
// scaled offset inputs too; `bases` excludes inputs whose value was scaled,
// permuted, or used only as a carry/condition. Loaded pointers are values,
// not the addresses from which they were loaded.
DmaAddressSources DmaAddressUserDataSources(Value root, uint32_t preserved_mask) {
	constexpr size_t MaxVisited = 256;
	struct Visit { const Inst* inst; bool base; };
	std::vector<Visit> pending;
	DmaAddressSources result;
	const auto append_register = [](std::vector<uint32_t>& registers, uint32_t reg) {
		if (std::find(registers.begin(), registers.end(), reg) == registers.end())
			registers.push_back(reg);
	};
	const auto visit = [&](Value value, bool base) {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr || inst->GetType() == Type::U1) return;
		if (std::any_of(pending.begin(), pending.end(), [=](const Visit& prior) {
			return prior.inst == inst && prior.base == base;
		})) return;
		if (pending.size() == MaxVisited) { result.complete = false; return; }
		pending.push_back({inst, base});
	};
	visit(root, true);
	for (size_t next = 0; next < pending.size(); ++next) {
		const auto [inst, base] = pending[next];
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::GetUserData) {
			if (inst->Arg(0).GetType() != Type::ScalarReg) { result.complete = false; continue; }
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			append_register(result.registers, reg);
			if (base) append_register(result.bases, reg);
			continue;
		}
		if (BufferAccessOf(op) != BufferAccess::None ||
		    AddressOpcodeInfoOf(op).access != AddressAccess::None ||
		    ImageOpcodeInfoOf(op).access != ImageAccess::None) continue;
		if (op == ValueOpcode::CompositeExtractU32x2 ||
		    op == ValueOpcode::CompositeExtractU32x3 ||
		    op == ValueOpcode::CompositeExtractU32x4 ||
		    op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->Arg(1).Resolve();
			const auto* source = inst->Arg(0).Resolve().TryInstruction();
			if (!index.IsImmediate() || index.GetType() != Type::U32 || source == nullptr) {
				result.complete = false; continue;
			}
			const auto source_op = source->GetOpcode();
			if (source_op == ValueOpcode::CompositeConstructU32x2 ||
			    source_op == ValueOpcode::CompositeConstructU32x3 ||
			    source_op == ValueOpcode::CompositeConstructU32x4 ||
			    source_op == ValueOpcode::CompositeConstructU64) {
				if (index.U32() >= source->NumArgs()) { result.complete = false; continue; }
				visit(source->Arg(index.U32()), base);
			} else if (source_op == ValueOpcode::IAddCarry32 && index.U32() < 2u) {
				// The sum retains an additive base; carry depends on both inputs
				// but neither input's raw value is an address high dword.
				visit(source->Arg(0), base && index.U32() == 0u);
				visit(source->Arg(1), base && index.U32() == 0u);
			} else if (BufferAccessOf(source_op) == BufferAccess::None &&
			           AddressOpcodeInfoOf(source_op).access == AddressAccess::None &&
			           ImageOpcodeInfoOf(source_op).access == ImageAccess::None) {
				result.complete = false;
			}
			continue;
		}
		switch (op) {
			case ValueOpcode::Phi:
				for (size_t i = 0; i < inst->NumArgs(); ++i) visit(inst->Arg(i), base);
				break;
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectF32:
				visit(inst->Arg(1), base); visit(inst->Arg(2), base); break;
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32:
				visit(inst->Arg(0), base); break;
			case ValueOpcode::IAdd32:
				visit(inst->Arg(0), base); visit(inst->Arg(1), base); break;
			case ValueOpcode::ISub32:
				visit(inst->Arg(0), base); visit(inst->Arg(1), false); break;
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::BitwiseXor32:
				for (size_t i = 0; i < 2; ++i) {
					const auto other = inst->Arg(1u - i).Resolve();
					const bool constant = other.IsImmediate() && other.GetType() == Type::U32;
					const bool preserve = constant &&
					    (op == ValueOpcode::BitwiseAnd32 ? (other.U32() & preserved_mask) == preserved_mask
					                                      : (other.U32() & preserved_mask) == 0u);
					visit(inst->Arg(i), base && preserve);
				}
				break;
			case ValueOpcode::IMul32:
			case ValueOpcode::UDiv32:
			case ValueOpcode::SMulHi:
			case ValueOpcode::UMulHi:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftRightArithmetic32:
			case ValueOpcode::BitFieldInsert:
			case ValueOpcode::BitFieldUExtract:
			case ValueOpcode::BitFieldSExtract:
			case ValueOpcode::BitwiseNot32:
			case ValueOpcode::BitReverse32:
			case ValueOpcode::BitCount32:
			case ValueOpcode::FindUMsb32:
			case ValueOpcode::FindILsb32:
			case ValueOpcode::SMin32:
			case ValueOpcode::UMin32:
			case ValueOpcode::SMax32:
			case ValueOpcode::UMax32:
				for (size_t i = 0; i < inst->NumArgs(); ++i) visit(inst->Arg(i), false);
				break;
			case ValueOpcode::UndefU32:
			case ValueOpcode::GetBuiltin:
			case ValueOpcode::GetAttribute:
			case ValueOpcode::GetDispatchThreadExtent:
				break;
			default: result.complete = false; break;
		}
	}
	if (!result.complete) result.bases.clear();
	return result;
}

void CollectDmaBaseRegisters(const Program& program, ShaderInfo& info) {
	info.dma_base_registers.clear();
	if (!info.uses_dma) return;
	const auto collect = [&](Value low_value, Value high_value, uint32_t high_mask) {
		const auto low = DmaAddressUserDataSources(low_value, UINT32_MAX);
		const auto high = DmaAddressUserDataSources(high_value, high_mask);
		if (!low.complete || !high.complete) return;
		for (const auto reg: low.bases) {
			if (reg == UINT32_MAX ||
			    std::find(high.bases.begin(), high.bases.end(), reg + 1u) == high.bases.end() ||
			    std::find(low.registers.begin(), low.registers.end(), reg + 1u) != low.registers.end()) continue;
			const ShaderInfo::DmaBaseRegister source {reg, high_mask};
			if (std::find(info.dma_base_registers.begin(), info.dma_base_registers.end(), source) == info.dma_base_registers.end())
				info.dma_base_registers.push_back(source);
		}
	};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op = inst.GetOpcode();
			const bool address = AddressOpcodeInfoOf(op).access != AddressAccess::None;
			const bool buffer = BufferAccessOf(op) != BufferAccess::None;
			if (!address && !buffer) continue;
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) return Fail("DMA metadata index is out of range");
			const auto& memory = program.memory_info[index];
			if (memory.planning_only) continue;
			if (address && (memory.kind == ResourceKind::Flat || memory.kind == ResourceKind::Global || memory.kind == ResourceKind::ScalarAddress)) {
				if (memory.address_is_full) {
					collect(inst.Arg(1), inst.Arg(2), UINT32_MAX);
				} else {
					const auto* handle = inst.Arg(0).Resolve().TryInstruction();
					if (handle != nullptr && handle->GetOpcode() == ValueOpcode::GetAddressResource && handle->NumArgs() == 2u)
						collect(handle->Arg(0), handle->Arg(1), UINT32_MAX);
				}
			} else if (buffer && memory.kind == ResourceKind::IndirectBuffer) {
				const auto* handle = inst.Arg(0).Resolve().TryInstruction();
				if (handle != nullptr && handle->GetOpcode() == ValueOpcode::GetBufferResource && handle->NumArgs() == 4u)
					collect(handle->Arg(0), handle->Arg(1), 0xffffu);
			}
		}
	}
	std::sort(info.dma_base_registers.begin(), info.dma_base_registers.end(), [](const auto& a, const auto& b) {
		return a.reg < b.reg || (a.reg == b.reg && a.high_mask < b.high_mask);
	});
}

void CollectBuiltinInputs(const Program& program, ShaderInfo& info) {
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (program.stage == ShaderType::TessellationControl &&
			    inst.GetOpcode() == ValueOpcode::LaneId) {
				AddInput(info, StageInputKind::InvocationId, 0, 1, "gl_InvocationID");
			}
			if (inst.GetOpcode() != ValueOpcode::GetBuiltin) {
				continue;
			}
			const auto kind = static_cast<StageInputKind>(inst.Arg(0).U32());
			switch (kind) {
				case StageInputKind::VertexIndex:
					AddInput(info, kind, 0, 1, "gl_VertexIndex");
					break;
				case StageInputKind::InstanceIndex:
					AddInput(info, kind, 0, 1, "gl_InstanceIndex");
					break;
				case StageInputKind::InvocationId:
					AddInput(info, kind, 0, 1, "gl_InvocationID");
					break;
				case StageInputKind::PrimitiveId:
					AddInput(info, kind, 0, 1, "gl_PrimitiveID");
					break;
				case StageInputKind::TessCoord: AddInput(info, kind, 0, 3, "gl_TessCoord"); break;
				case StageInputKind::FragCoord: AddInput(info, kind, 0, 4, "gl_FragCoord"); break;
				case StageInputKind::FrontFacing:
					AddInput(info, kind, 0, 1, "gl_FrontFacing");
					break;
				case StageInputKind::Layer: AddInput(info, kind, 0, 1, "gl_Layer"); break;
				case StageInputKind::SampleId: AddInput(info, kind, 0, 1, "gl_SampleID"); break;
				case StageInputKind::BaryCoordSmoothSample:
					AddInput(info, StageInputKind::SampleId, 0, 1, "gl_SampleID");
					[[fallthrough]];
				case StageInputKind::BaryCoordSmooth:
				case StageInputKind::BaryCoordSmoothCentroid:
					AddInput(info, StageInputKind::BaryCoordSmooth, 0, 3, "gl_BaryCoordKHR");
					break;
				case StageInputKind::BaryCoordNoPerspective:
					AddInput(info, kind, 0, 3, "gl_BaryCoordNoPerspKHR");
					break;
				case StageInputKind::WorkgroupId:
					AddInput(info, kind, 0, 3, "gl_WorkGroupID");
					break;
				case StageInputKind::LocalInvocationId:
					AddInput(info, kind, 0, 3, "gl_LocalInvocationID");
					break;
				case StageInputKind::LocalInvocationIndex:
					AddInput(info, kind, 0, 1, "gl_LocalInvocationIndex");
					break;
				case StageInputKind::GlobalInvocationId:
					AddInput(info, kind, 0, 3, "gl_GlobalInvocationID");
					break;
				case StageInputKind::PackedAncillary:
				case StageInputKind::Parameter: break;
			}
		}
	}
}

void CollectOutputs(const Program& program, ShaderStageInputInfo input_info, ShaderInfo& info) {
	const bool alpha_remap = program.stage == ShaderType::Pixel && input_info.pixel != nullptr &&
	                         input_info.pixel->alpha_blend_source != ShaderAlphaBlendSource::None;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::SetAttribute) {
				continue;
			}
			const auto& export_info = program.export_info[inst.Flags<ExportFlags>().index];
			if (export_info.kind == ExportTargetKind::MrtZ) {
				if (program.stage == ShaderType::Pixel && (export_info.en & 0x1u) != 0 &&
				    input_info.pixel->ps_depth_export_enable) {
					AddOutput(info, StageOutputKind::Depth, 0, 0, "gl_FragDepth");
				}
				if (program.stage == ShaderType::Pixel && (export_info.en & 0x4u) != 0 &&
				    input_info.pixel->ps_sample_mask_export_enable) {
					AddOutput(info, StageOutputKind::SampleMask, 0, 0, "gl_SampleMask");
				}
				continue;
			}
			if (export_info.en == 0) {
				continue;
			}
			switch (export_info.kind) {
				case ExportTargetKind::Position:
					if (export_info.index == 0) {
						AddOutput(info, StageOutputKind::Position, 0, 0, "out_position");
						break;
					}
					if (export_info.compr) {
						return Fail("compressed auxiliary position export is unsupported");
					}
					for (uint32_t component = 0; component < 4; component++) {
						if ((export_info.en & (1u << component)) == 0) {
							continue;
						}
						const auto output = DecodePositionExportComponent(
						    input_info.vertex->pa_cl_vs_out_cntl, export_info.index, component);
						if (output.viewport) {
							AddOutput(info, StageOutputKind::ViewportIndex, 0, 0, "gl_ViewportIndex");
						}
						if (output.point_size) {
							AddOutput(info, StageOutputKind::PointSize, 0, 0, "gl_PointSize");
						}
						if (output.layer) {
							AddOutput(info, StageOutputKind::Layer, 0, 0, "gl_Layer");
						}
						if (output.clip_distance != UINT32_MAX) {
							AddOutput(info, StageOutputKind::ClipDistance, output.clip_distance, 0,
							          "gl_ClipDistance");
						}
						if (output.cull_distance != UINT32_MAX) {
							AddOutput(info, StageOutputKind::CullDistance, output.cull_distance, 0,
							          "gl_CullDistance");
						}
					}
					break;
				case ExportTargetKind::Parameter:
					AddOutput(info, StageOutputKind::Parameter, export_info.index,
					          export_info.index, fmt::format("out_param_{}", export_info.index));
					break;
				case ExportTargetKind::Mrt: {
					if (alpha_remap && export_info.index != 0) {
						break;
					}
					const auto slot = input_info.pixel->dual_source_blending
					                      ? 0u : ShaderPixelExportTarget(input_info.pixel->target_shader_mask,
					                                                    export_info.index);
					if (slot >= 8) {
						break;
					}
					AddOutput(info, StageOutputKind::Mrt, export_info.index, slot,
					          fmt::format("out_mrt_{}", export_info.index));
					if (alpha_remap) {
						AddOutput(info, StageOutputKind::Mrt, 1, 0, "out_mrt_1");
					}
					break;
				}
				default: break;
			}
		}
	}
	// The hardware ID buffer slot: written from gl_PrimitiveID and the draw's object id.
	if (program.stage == ShaderType::Pixel && input_info.pixel != nullptr &&
	    input_info.pixel->id_buffer_slot != UINT32_MAX) {
		const auto slot = input_info.pixel->id_buffer_slot;
		AddOutput(info, StageOutputKind::Mrt, slot, slot, "out_id_buffer");
		AddInput(info, StageInputKind::PrimitiveId, 0, 1, "gl_PrimitiveID");
	}
}

} // namespace

void CollectShaderInfo(Program& program, ShaderStageInputInfo input_info) {
	if (!program.resource_tracking_complete || program.shader_info_complete) {
		return Fail(!program.resource_tracking_complete ? "shader resources were not tracked"
		                                                : "shader info already collected");
	}
	ValidateOptions(program, input_info);
	ValidateValueReferences(program, input_info);

	auto& next = program.info;
	next.inputs.clear();
	next.outputs.clear();
	CollectDmaBaseRegisters(program, next);
	next.has_bitwise_xor =
	    std::any_of(program.blocks.begin(), program.blocks.end(), [](const auto* block) {
		    return std::any_of(block->begin(), block->end(), [](const auto& inst) {
			    return inst.GetOpcode() == ValueOpcode::BitwiseXor32;
		    });
	    });
	switch (program.stage) {
		case ShaderType::Vertex:
		case ShaderType::Local: CollectVertexInputs(program, input_info.vertex, next); break;
		case ShaderType::TessellationControl:
		case ShaderType::TessellationEvaluation:
		case ShaderType::Mesh: break;
		case ShaderType::Pixel: CollectPixelInputs(program, input_info.pixel, next); break;
		case ShaderType::Compute: CollectComputeInputs(input_info.compute, next); break;
		default: return Fail("unsupported shader stage for info collection");
	}
	CollectBuiltinInputs(program, next);
	CollectOutputs(program, input_info, next);
	program.shader_info_complete = true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
