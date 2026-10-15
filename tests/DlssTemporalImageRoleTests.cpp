#include "graphics/host_gpu/renderer/image/imageView.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

using namespace Libs::Graphics;

namespace {
void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

vk::ImageView Handle(uintptr_t value) {
    return vk::ImageView{reinterpret_cast<VkImageView>(value)};
}

// CPU-only cache records, with the same fields as Image::views. No Vulkan device
// or fabricated image contents are involved in resolving recorded view metadata.
struct CachedView {
    ImageViewInfo info;
    vk::ImageView view;
};
}

int main() {
    ImageViewInfo color{};
    color.format = vk::Format::eR16G16B16A16Sfloat;
    const std::array controls{CachedView{color, Handle(17)}};
    Require(ResolveDlssTemporalRoleView(color, controls, nullptr) == nullptr,
            "null bound view must remain unsupported");
    const auto* unchanged = ResolveDlssTemporalRoleView(color, controls, Handle(17));
    Require(unchanged != nullptr && *unchanged == color,
            "ordinary identity color view must preserve its metadata");
    std::puts("PASS: null-handle and ordinary-color controls");

    ImageViewInfo requested{};
    requested.format = vk::Format::eR32Sfloat;
    requested.aspect = vk::ImageAspectFlagBits::eColor;
    requested.mapping = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eR,
                         vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eR};
    const auto original = requested;

    // Independent Vulkan descriptor contract: the selected native handle refers
    // to its creation metadata, even when an R32/COLOR alias request was converted
    // to a D32S8/DEPTH view by the image cache.
    auto native_depth = requested;
    native_depth.format = vk::Format::eD32SfloatS8Uint;
    native_depth.aspect = vk::ImageAspectFlagBits::eDepth;
    native_depth.usage = {};
    auto native_stencil = native_depth;
    native_stencil.aspect = vk::ImageAspectFlagBits::eStencil;
    const std::array cached{CachedView{color, Handle(17)},
                           CachedView{native_stencil, Handle(19)},
                           CachedView{native_depth, Handle(23)}};
    const auto* selected = ResolveDlssTemporalRoleView(requested, cached, Handle(23));
    Require(selected != nullptr && *selected == native_depth,
            "bound native depth view must resolve D32S8/DEPTH, not its R32/COLOR request");
    Require(requested == original, "resolving a native view must not mutate its request");

    const auto* stencil = ResolveDlssTemporalRoleView(requested, cached, Handle(19));
    Require(stencil != nullptr && *stencil == native_stencil,
            "a stencil handle must retain stencil semantics for the caller to reject");
    const auto* other = ResolveDlssTemporalRoleView(requested, cached, Handle(17));
    Require(other != nullptr && *other == color,
            "selection must use the exact handle among multiple cached views");
    Require(ResolveDlssTemporalRoleView(requested, cached, Handle(29)) == nullptr,
            "unknown or foreign native view must not fall back to the request");
    const std::array<CachedView, 0> empty{};
    Require(ResolveDlssTemporalRoleView(requested, empty, Handle(23)) == nullptr,
            "a handle absent from the owning image must remain unsupported");

    // Selection must not erase properties checked by the strict role guard.
    // These native views are deliberately unsuitable full-image role inputs.
    auto altered = native_depth;
    altered.base_level = 2;
    altered.level_count = 3;
    altered.base_layer = 4;
    altered.layer_count = 5;
    altered.min_lod = 6;
    altered.type = vk::ImageViewType::e2DArray;
    altered.mapping.r = vk::ComponentSwizzle::eZero;
    altered.usage = vk::ImageUsageFlagBits::eStorage;
    const std::array variation{CachedView{altered, Handle(31)}};
    const auto* retained = ResolveDlssTemporalRoleView(requested, variation, Handle(31));
    Require(retained != nullptr && *retained == altered,
            "range, type, consumed mapping and usage must reach the role guard unchanged");
    std::puts("PASS: native depth selection, exact handles, foreign/empty cache, stencil and guard properties");
}
