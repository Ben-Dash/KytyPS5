#ifndef KYTY_GRAPHICS_DLSS_TEMPORAL_CONTRACT_H_
#define KYTY_GRAPHICS_DLSS_TEMPORAL_CONTRACT_H_

#include "graphics/presentation/dlss/dlssVulkan.h"
#include <array>
#include <optional>

namespace Libs::Graphics {

// Supplied by an explicitly declared temporal pass, never inferred from a
// shader identity or a texture's format. Private input copies preserve the
// producer's contents until their ordered GPU consumption.
struct DlssTemporalAssociation {
    bool declared_hdr_before_tonemap = false;
    bool declared_preserve_each_output_alpha = false;
    bool immutable_input_copies = false;
    uint64_t producer_guest_frame = 0;
    uint64_t consumer_guest_frame = 0;
    uint64_t producer_sequence = 0;
    uint64_t consumer_sequence = 0;
    uint64_t producer_color = 0;
    uint64_t consumer_color = 0;
    uint64_t producer_color_generation = 0;
    uint64_t consumer_color_generation = 0;
    uint64_t temporal_serial = 0;
    std::optional<uint64_t> previous_temporal_serial;
    std::array<uint64_t, 3> private_inputs{};
    std::array<uint64_t, 2> outputs{};
};

inline DlssStatus ValidateDlssTemporalAssociation(const DlssTemporalAssociation& proof) {
    const auto invalid = [](const char* message) {
        return DlssStatus{DlssStatusCode::InvalidArgument, 0, message};
    };
    if (!proof.declared_hdr_before_tonemap || !proof.declared_preserve_each_output_alpha ||
        !proof.immutable_input_copies)
        return invalid("DLSS needs explicit HDR roles, per-output alpha preservation and immutable input copies");
    if (proof.producer_guest_frame != proof.consumer_guest_frame ||
        proof.producer_sequence >= proof.consumer_sequence)
        return invalid("DLSS producer must precede reconstruction in the same guest frame");
    if (proof.producer_color == 0 || proof.producer_color != proof.consumer_color ||
        proof.producer_color_generation == 0 ||
        proof.producer_color_generation != proof.consumer_color_generation)
        return invalid("DLSS color producer association is stale or missing");
    if (proof.previous_temporal_serial && proof.temporal_serial <= *proof.previous_temporal_serial)
        return invalid("DLSS reconstruction serial was already consumed or moved backwards");
    for (size_t i = 0; i < proof.private_inputs.size(); ++i) {
        if (proof.private_inputs[i] == 0) return invalid("DLSS private input is missing");
        for (size_t j = i + 1; j < proof.private_inputs.size(); ++j)
            if (proof.private_inputs[i] == proof.private_inputs[j])
                return invalid("DLSS private input images must be distinct");
        for (const auto output : proof.outputs)
            if (output == proof.private_inputs[i]) return invalid("DLSS output aliases a private input");
    }
    if (proof.outputs[0] == 0 || proof.outputs[1] == 0 || proof.outputs[0] == proof.outputs[1])
        return invalid("DLSS needs two distinct populated RGB merge outputs");
    return {};
}

} // namespace Libs::Graphics
#endif
