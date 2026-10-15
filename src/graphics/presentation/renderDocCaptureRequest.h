#ifndef EMULATOR_GRAPHICS_PRESENTATION_RENDERDOC_CAPTURE_REQUEST_H_
#define EMULATOR_GRAPHICS_PRESENTATION_RENDERDOC_CAPTURE_REQUEST_H_

#include <filesystem>
#include <system_error>

namespace Libs::Graphics {

// Diagnostic request only: the caller owns RenderDoc calls and capture boundaries.
// Construct once before guest execution with a unique, initially absent absolute
// gate path. Publish an empty regular file only when the capture is wanted.
// This class is polled by one thread; it never removes or modifies the gate.
class RenderDocCaptureRequest {
public:
    enum class Status {
        Disabled, InvalidPath, StaleGate, FileError, InvalidGate,
        Pending, Busy, ApiUnavailable, Request, Consumed,
    };

    explicit RenderDocCaptureRequest(const char* request_file) {
        if (request_file == nullptr || *request_file == '\0') {
            return;
        }
        m_arm_status = m_status = Status::InvalidPath;
        m_path = std::filesystem::path(request_file);
        if (!m_path.is_absolute()) {
            return;
        }
        std::error_code error;
        const auto file = std::filesystem::symlink_status(m_path, error);
        // A missing leaf or parent is the intended initially absent gate.
        if (file.type() == std::filesystem::file_type::not_found) {
            m_arm_status = m_status = Status::Pending;
        } else {
            m_arm_status = m_status = error ? Status::FileError : Status::StaleGate;
        }
    }

    [[nodiscard]] Status ArmStatus() const { return m_arm_status; }

    [[nodiscard]] Status Poll(bool api_available, bool idle) {
        if (m_status != Status::Pending) {
            return m_status;
        }
        std::error_code error;
        const auto file = std::filesystem::symlink_status(m_path, error);
        if (file.type() == std::filesystem::file_type::not_found) {
            return Status::Pending;
        }
        if (error) {
            return m_status = Status::FileError;
        }
        // symlink_status deliberately rejects symbolic links as well as directories.
        if (!std::filesystem::is_regular_file(file)) {
            return m_status = Status::InvalidGate;
        }
        const auto size = std::filesystem::file_size(m_path, error);
        if (error) {
            return m_status = Status::FileError;
        }
        if (size != 0) {
            return m_status = Status::InvalidGate;
        }
        if (!api_available) {
            return Status::ApiUnavailable;
        }
        if (!idle) {
            return Status::Busy;
        }
        // Consume before the caller attempts capture. Failure must not retry.
        m_status = Status::Consumed;
        return Status::Request;
    }

private:
    std::filesystem::path m_path;
    Status m_arm_status = Status::Disabled;
    Status m_status = Status::Disabled;
};

} // namespace Libs::Graphics

#endif
