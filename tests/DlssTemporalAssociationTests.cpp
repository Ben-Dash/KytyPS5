#include "graphics/presentation/dlss/dlssTemporalContract.h"
#include <cstdio>
#include <cstdlib>

using namespace Libs::Graphics;

static void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() {
    DlssTemporalAssociation valid;
    valid.declared_hdr_before_tonemap = true;
    valid.declared_preserve_each_output_alpha = true;
    valid.immutable_input_copies = true;
    valid.producer_guest_frame = valid.consumer_guest_frame = 7;
    valid.producer_sequence = 11; valid.consumer_sequence = 15;
    valid.producer_color = valid.consumer_color = 30;
    valid.producer_color_generation = valid.consumer_color_generation = 19;
    valid.temporal_serial = 9; valid.previous_temporal_serial = 8;
    valid.private_inputs = {101, 102, 103}; valid.outputs = {201, 202};
    Require(bool(ValidateDlssTemporalAssociation(valid)), "declared matching immutable temporal tuple must be accepted");
    unsigned rejected = 0;
    auto Reject = [&](DlssTemporalAssociation input) {
        Require(!ValidateDlssTemporalAssociation(input), "invalid association was accepted"); ++rejected;
    };
    auto v = valid; v.declared_hdr_before_tonemap = false; Reject(v);
    v = valid; v.declared_preserve_each_output_alpha = false; Reject(v);
    v = valid; v.immutable_input_copies = false; Reject(v);
    v = valid; ++v.consumer_guest_frame; Reject(v);
    v = valid; v.consumer_sequence = v.producer_sequence; Reject(v);
    v = valid; --v.consumer_sequence; v.producer_sequence = v.consumer_sequence + 1; Reject(v);
    v = valid; ++v.consumer_color; Reject(v);
    v = valid; v.producer_color = v.consumer_color = 0; Reject(v);
    v = valid; ++v.consumer_color_generation; Reject(v);
    v = valid; v.producer_color_generation = v.consumer_color_generation = 0; Reject(v);
    v = valid; v.previous_temporal_serial = v.temporal_serial; Reject(v);
    v = valid; v.previous_temporal_serial = v.temporal_serial + 1; Reject(v);
    for (size_t i = 0; i < valid.private_inputs.size(); ++i) {
        v = valid; v.private_inputs[i] = 0; Reject(v);
        for (size_t j = i + 1; j < valid.private_inputs.size(); ++j) {
            v = valid; v.private_inputs[j] = v.private_inputs[i]; Reject(v);
        }
        for (size_t j = 0; j < valid.outputs.size(); ++j) {
            v = valid; v.outputs[j] = v.private_inputs[i]; Reject(v);
        }
    }
    v = valid; v.outputs[0] = 0; Reject(v);
    v = valid; v.outputs[1] = 0; Reject(v);
    v = valid; v.outputs[1] = v.outputs[0]; Reject(v);
    v = valid; v.previous_temporal_serial.reset(); v.temporal_serial = 0;
    Require(bool(ValidateDlssTemporalAssociation(v)), "first frame zero with explicit producer is valid");
    v = valid; v.temporal_serial += 5;
    Require(bool(ValidateDlssTemporalAssociation(v)), "serial gap is valid and caller must reset history");
    std::printf("PASS: temporal association, %u rejection cases, first frame and serial gap\n", rejected);
}
