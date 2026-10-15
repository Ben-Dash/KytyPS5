// Synthetic contract: two existing RGBA16F outputs retain independent alpha,
// source RGB is visible to downstream readback, and an unrelated R8 is untouched.
int RunRgbMergeTest(const DlssProjectInfo& project) {
    Vulkan vk; vk.Initialize(project, false);
    Require(vk.validation_enabled, "RGB merge regression requires Vulkan validation");
    ResourceControl(vk); // A missing helper after this point is an intended RED.
    const DlssExtent backing{29, 19}, active{21, 13}; // Partial last workgroups + padding.
    const size_t count = size_t(backing.width) * backing.height, rgba_bytes = count * 8;
    const size_t source = vk.MakeImage(backing, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    const size_t target_a = vk.MakeImage(backing, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    const size_t target_b = vk.MakeImage(backing, VK_FORMAT_R16G16B16A16_SFLOAT, true);
    const size_t untouched = vk.MakeImage(backing, VK_FORMAT_R8_UNORM);
    const size_t source_upload = vk.MakeBuffer(rgba_bytes), a_upload = vk.MakeBuffer(rgba_bytes);
    const size_t b_upload = vk.MakeBuffer(rgba_bytes), r_upload = vk.MakeBuffer(count);
    const size_t a_readback = vk.MakeBuffer(rgba_bytes + ReadbackGuardBytes);
    const size_t b_readback = vk.MakeBuffer(rgba_bytes + ReadbackGuardBytes);
    const size_t r_readback = vk.MakeBuffer(count + ReadbackGuardBytes);
    DlssRgbMerge merge;
    CheckDlss(merge.Init(vk.device, vk.gdpa), "RGB merge Init (required mechanism)");
    Require(merge.Record(VK_NULL_HANDLE, vk.images[source].view, vk.images[target_a].view,
                vk.images[target_b].view, active).code == DlssStatusCode::InvalidArgument,
            "RGB merge accepted null command buffer");
    Require(merge.Record(vk.command, vk.images[source].view, vk.images[source].view,
                vk.images[target_b].view, active).code == DlssStatusCode::InvalidArgument,
            "RGB merge accepted source/destination alias");
    Require(merge.Record(vk.command, vk.images[source].view, vk.images[target_a].view,
                vk.images[target_b].view, {0, active.height}).code == DlssStatusCode::InvalidArgument,
            "RGB merge accepted empty active extent");
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        auto* src = static_cast<uint16_t*>(vk.buffers[source_upload].mapped);
        auto* a = static_cast<uint16_t*>(vk.buffers[a_upload].mapped);
        auto* b = static_cast<uint16_t*>(vk.buffers[b_upload].mapped);
        auto* r = static_cast<unsigned char*>(vk.buffers[r_upload].mapped);
        for (size_t i = 0; i < count; ++i) {
            src[i * 4] = Half(2.0f + float((i + iteration) % 7) * 0.25f);
            src[i * 4 + 1] = Half(0.125f + float((i + iteration) % 5) * 0.125f);
            src[i * 4 + 2] = Half(4.0f + float((i + iteration) % 3) * 0.5f);
            src[i * 4 + 3] = Half(1.0f);
            for (size_t channel = 0; channel < 3; ++channel) {
                a[i * 4 + channel] = Half(0.0625f + float(channel) * 0.0625f);
                b[i * 4 + channel] = Half(0.25f + float(channel) * 0.0625f);
            }
            a[i * 4 + 3] = Half(0.25f + float(i % 7) * 0.125f);
            b[i * 4 + 3] = i % 2 ? Half(-0.5f - float(i % 3) * 0.25f) : 0;
            r[i] = static_cast<unsigned char>((i * 37u + iteration) & 255u);
        }
        for (const auto index : {a_readback, b_readback, r_readback})
            std::memset(vk.buffers[index].mapped, 0xa5, size_t(vk.buffers[index].bytes));
        vk.Begin(); vk.Upload(source, source_upload); vk.Upload(target_a, a_upload);
        vk.Upload(target_b, b_upload); vk.Upload(untouched, r_upload);
        vk.Barrier(vk.images[source], VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT);
        vk.Barrier(vk.images[target_a], VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vk.Barrier(vk.images[target_b], VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        CheckDlss(merge.Record(vk.command, vk.images[source].view, vk.images[target_a].view,
                    vk.images[target_b].view, active), "RGB merge Record");
        vk.Download(target_a, a_readback); vk.Download(target_b, b_readback);
        vk.Download(untouched, r_readback); vk.Submit();
        const auto* got_a = static_cast<const uint16_t*>(vk.buffers[a_readback].mapped);
        const auto* got_b = static_cast<const uint16_t*>(vk.buffers[b_readback].mapped);
        for (size_t i = 0; i < count; ++i) {
            const bool inside = i % backing.width < active.width && i / backing.width < active.height;
            for (size_t channel = 0; channel < 4; ++channel) {
                const auto expected_a = inside && channel < 3 ? src[i * 4 + channel] : a[i * 4 + channel];
                const auto expected_b = inside && channel < 3 ? src[i * 4 + channel] : b[i * 4 + channel];
                Require(got_a[i * 4 + channel] == expected_a,
                        "RGB merge target A RGB/independent alpha/padding mismatch");
                Require(got_b[i * 4 + channel] == expected_b,
                        "RGB merge target B RGB/independent alpha/padding mismatch");
            }
        }
        Require(std::memcmp(r, vk.buffers[r_readback].mapped, count) == 0,
                "RGB merge changed unrelated R8 output");
        VerifyGuard(vk.buffers[a_readback], rgba_bytes);
        VerifyGuard(vk.buffers[b_readback], rgba_bytes); VerifyGuard(vk.buffers[r_readback], count);
    }
    CheckDlss(merge.DestroyAfterGpuIdle(), "RGB merge DestroyAfterGpuIdle");
    vk.ValidateAndCleanup();
    std::puts("RGB MERGE PASS: 3 completed downstream readbacks; two HDR RGB destinations; "
        "all independent alpha/padding exact; unrelated R8 exact; final validation0; no NGX/game");
    return 0;
}
