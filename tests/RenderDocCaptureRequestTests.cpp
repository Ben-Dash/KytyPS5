#include "graphics/presentation/renderDocCaptureRequest.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using Libs::Graphics::RenderDocCaptureRequest;
using Status = RenderDocCaptureRequest::Status;
namespace fs = std::filesystem;

int main() {
    int failures = 0;
    const auto check = [&failures](bool value, const char* message) {
        if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    // Create an exclusively owned directory. Cleanup below is limited to its
    // explicitly created files; no recursive deletion of a computed temp path.
    std::error_code error;
    const auto base = fs::temp_directory_path(error);
    if (error || !base.is_absolute()) { std::fputs("FAIL: temp path\n", stderr); return 1; }
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path directory;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const auto candidate = base / ("kyty-renderdoc-request-" + std::to_string(nonce) +
                                       "-" + std::to_string(attempt));
        if (fs::create_directory(candidate, error)) { directory = candidate; break; }
    }
    if (directory.empty()) { std::fputs("FAIL: create unique temp directory\n", stderr); return 1; }
    const auto path = [&](const char* name) { return (directory / name).string(); };
    const auto empty_file = [&](const char* name) {
        std::ofstream output(path(name), std::ios::binary);
        check(output.good(), "create owned empty gate");
    };

    RenderDocCaptureRequest absent(nullptr), empty("");
    check(absent.ArmStatus() == Status::Disabled && absent.Poll(true, true) == Status::Disabled,
          "absent environment disables request");
    check(empty.ArmStatus() == Status::Disabled && empty.Poll(true, true) == Status::Disabled,
          "empty environment disables request");
    RenderDocCaptureRequest relative("relative-gate"), dot("./relative-gate");
    check(relative.ArmStatus() == Status::InvalidPath && dot.ArmStatus() == Status::InvalidPath,
          "relative paths rejected");
#ifdef _WIN32
    RenderDocCaptureRequest drive_relative("C:gate"), rooted("\\gate");
    check(drive_relative.ArmStatus() == Status::InvalidPath && rooted.ArmStatus() == Status::InvalidPath,
          "Windows drive-relative and root-relative paths rejected");
#endif
    empty_file("stale");
    RenderDocCaptureRequest stale(path("stale").c_str());
    check(stale.ArmStatus() == Status::StaleGate, "preexisting regular gate is stale");
    fs::remove(directory / "stale", error);
    empty_file("stale");
    check(stale.Poll(true, true) == Status::StaleGate, "recreated stale gate cannot rearm");

    RenderDocCaptureRequest request(path("fresh").c_str());
    check(request.ArmStatus() == Status::Pending, "initially absent absolute gate arms");
    check(request.Poll(true, true) == Status::Pending, "missing gate remains pending");
    empty_file("fresh");
    check(request.Poll(false, true) == Status::ApiUnavailable, "unavailable API is typed and pending");
    check(request.Poll(true, false) == Status::Busy, "active capture leaves gate pending");
    check(request.Poll(true, true) == Status::Request, "fresh empty regular gate requests once");
    // Deliberately do not invoke a capture callback: a failed start consumes the
    // same request and cannot cause automatic retries or a capture storm.
    for (int i = 0; i < 3; ++i) {
        check(request.Poll(true, true) == Status::Consumed, "failed capture does not retry");
    }
    check(fs::is_regular_file(directory / "fresh") && fs::file_size(directory / "fresh") == 0,
          "request never alters gate");

    fs::create_directory(directory / "preexisting-directory", error);
    RenderDocCaptureRequest existing_directory(path("preexisting-directory").c_str());
    check(existing_directory.ArmStatus() == Status::StaleGate, "preexisting directory rejected");
    RenderDocCaptureRequest new_directory(path("new-directory").c_str());
    fs::create_directory(directory / "new-directory", error);
    check(new_directory.Poll(true, true) == Status::InvalidGate, "fresh directory is not a request");
    RenderDocCaptureRequest nonempty(path("nonempty").c_str());
    { std::ofstream output(path("nonempty")); output << "unexpected payload"; }
    check(nonempty.Poll(true, true) == Status::InvalidGate, "nonempty gate rejected");

    // These exact children were created by this test inside its exclusive directory.
    for (const auto* name : {"stale", "fresh", "nonempty", "preexisting-directory", "new-directory"}) {
        error.clear();
        check(fs::remove(directory / name, error) && !error, "owned child cleanup");
    }
    error.clear();
    check(fs::remove(directory, error) && !error, "owned directory cleanup");
    if (failures == 0) { std::puts("RenderDocCaptureRequestTests: all passed"); }
    return failures == 0 ? 0 : 1;
}
